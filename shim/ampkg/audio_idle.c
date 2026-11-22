/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg: the idle audio client (see ampkg.h). */
#include "ampkg.h"

WINE_DEFAULT_DEBUG_CHANNEL(ampkg);

/* Third difference, this one in what a listener sees rather than in behaviour: the agent opens one
 * output stream at start-up (shared mode, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) and feeds it silence
 * for as long as it runs; music goes through a separate client per track. On Linux that is a
 * permanent second "AMPLibraryAgent" playback stream in PipeWire/PulseAudio.
 * Such a client is kept "quiet": Initialize creates no stream, a 10 ms timer stands in for the
 * device, and its render client / stream volume are small proxies. The real stream is created the
 * moment it is needed: when non-silent audio is written, or a service other than those two is
 * requested. AM_AUDIO_IDLE_STREAM=1 turns this off. */
static const GUID my_IID_IAudioRenderClient = {0xf294acfc,0x3146,0x4483,{0xa7,0xbf,0xad,0xdc,0xa7,0xc2,0x60,0xe2}};
static const GUID my_IID_IAudioStreamVolume = {0x93014887,0x242d,0x4068,{0x8a,0x15,0xcf,0x5e,0x93,0xb9,0x0f,0xe3}};

static CRITICAL_SECTION qc_cs;
static CRITICAL_SECTION_DEBUG qc_cs_debug =
{
    0, 0, &qc_cs, { &qc_cs_debug.ProcessLocksList, &qc_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)"ampkg qc_cs" }
};
static CRITICAL_SECTION qc_cs = { &qc_cs_debug, -1, 0, 0, 0, 0 };

static struct
{
    void *client;                   /* the quiet client, NULL if there is none (one at a time) */
    BOOL real;                      /* its stream exists now: everything is passed through */
    LONG gen, thread_gen;
    int mode; DWORD flags; LONGLONG duration, period; BYTE fmt[64]; GUID session; BOOL has_session;
    UINT32 frames, rate, align, channels; BOOL is_float;
    HANDLE event; BOOL started;
    BYTE *buf; BOOL buf_out;        /* buffer handed out by the proxy's GetBuffer */
    double written, played; LARGE_INTEGER last;
    float vol[8];
    void ***real_render, ***real_vol;
    void **render, **volume;        /* the proxies: objects that are just a vtable pointer */
} qc;

static HRESULT (WINAPI *real_ac_Initialize)(void *, int, DWORD, LONGLONG, LONGLONG, const void *, const GUID *);
static HRESULT (WINAPI *real_ac_GetBufferSize)(void *, UINT32 *);
static HRESULT (WINAPI *real_ac_GetStreamLatency)(void *, LONGLONG *);
static HRESULT (WINAPI *real_ac_GetCurrentPadding)(void *, UINT32 *);
static HRESULT (WINAPI *real_ac_Start)(void *);
static HRESULT (WINAPI *real_ac_Stop)(void *);
static HRESULT (WINAPI *real_ac_Reset)(void *);

static BOOL qc_quiet(void *client) { return qc.client == client && !qc.real; }

/* frames the pretend device has consumed so far; call with qc_cs held */
static void qc_advance(void)
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (qc.started) qc.played += (double)(now.QuadPart - qc.last.QuadPart) * qc.rate / freq.QuadPart;
    if (qc.played > qc.written) qc.played = qc.written;
    qc.last = now;
}

static DWORD WINAPI qc_thread(void *arg)
{
    LONG gen = (LONG)(LONG_PTR)arg;
    for (;;)
    {
        Sleep(10);
        EnterCriticalSection(&qc_cs);
        if (qc.gen != gen || !qc.client || qc.real) { LeaveCriticalSection(&qc_cs); return 0; }
        if (qc.started && qc.event) NtSetEvent(qc.event, NULL);
        LeaveCriticalSection(&qc_cs);
    }
}

/* create the real stream and bring it to the quiet client's state; call with qc_cs held */
static HRESULT qc_make_real(const char *why)
{
    void *client = qc.client;
    HRESULT hr;

    if (qc.real) return S_OK;
    hr = real_ac_Initialize(client, qc.mode, qc.flags, qc.duration, qc.period, qc.fmt, qc.has_session ? &qc.session : NULL);
    if (FAILED(hr)) { ERR("cannot create the stream for quiet client %p: %#lx\n", client, hr); return hr; }
    if (qc.event) real_ac_SetEventHandle(client, qc.event);
    /* Wine's services are interfaces of the client object sharing its reference count; the
     * proxies must not keep the client alive, so those two references are dropped again */
    if (SUCCEEDED(real_ac_GetService(client, &my_IID_IAudioRenderClient, (void **)&qc.real_render))) real_ac_Release(client);
    else qc.real_render = NULL;
    if (SUCCEEDED(real_ac_GetService(client, &my_IID_IAudioStreamVolume, (void **)&qc.real_vol)))
    {
        real_ac_Release(client);
        ((HRESULT (WINAPI *)(void *, UINT32, const float *))(*qc.real_vol)[6])(qc.real_vol, qc.channels, qc.vol);
    }
    else qc.real_vol = NULL;
    if (qc.started) real_ac_Start(client);
    qc.real = TRUE;
    MESSAGE("ampkg: idle audio client %p now has a real stream (%s)\n", client, why);
    return S_OK;
}

static BOOL qc_is_silence(const BYTE *data, UINT32 frames)
{
    UINT32 i, n = frames * qc.align;
    if (qc.is_float)
    {
        const float *f = (const float *)data;
        for (i = 0; i < n / sizeof(float); i++) if (f[i] > 1e-6f || f[i] < -1e-6f) return FALSE;    /* below -120 dB */
        return TRUE;
    }
    for (i = 0; i < n; i++) if (data[i]) return FALSE;
    return TRUE;
}

/* --- proxies; reference counting is the client's, as with the real services --- */
static HRESULT WINAPI qcp_QueryInterface(void *iface, const GUID *iid, void **obj)
{
    ((ULONG (WINAPI *)(void *))(*(void ***)qc.client)[1])(qc.client);
    *obj = iface;
    return S_OK;
}
static ULONG WINAPI qcp_AddRef(void *iface) { return ((ULONG (WINAPI *)(void *))(*(void ***)qc.client)[1])(qc.client); }
static ULONG WINAPI qcp_Release(void *iface) { return ((ULONG (WINAPI *)(void *))(*(void ***)qc.client)[2])(qc.client); }

static HRESULT WINAPI qcr_GetBuffer(void *iface, UINT32 frames, BYTE **data)
{
    HRESULT hr = S_OK;
    EnterCriticalSection(&qc_cs);
    if (qc.real)
    {
        void ***r = qc.real_render;
        LeaveCriticalSection(&qc_cs);
        return r ? ((HRESULT (WINAPI *)(void *, UINT32, BYTE **))(*r)[3])(r, frames, data) : E_FAIL;
    }
    qc_advance();
    if (qc.buf_out) hr = 0x88890007;                                            /* AUDCLNT_E_OUT_OF_ORDER */
    else if (frames > qc.frames - (UINT32)(qc.written - qc.played)) hr = 0x88890006;    /* AUDCLNT_E_BUFFER_TOO_LARGE */
    else { memset(qc.buf, 0, frames * qc.align); *data = qc.buf; qc.buf_out = frames != 0; }
    LeaveCriticalSection(&qc_cs);
    return hr;
}

static HRESULT WINAPI qcr_ReleaseBuffer(void *iface, UINT32 frames, DWORD flags)
{
    void ***r;
    BYTE *dst;
    HRESULT hr = S_OK;

    EnterCriticalSection(&qc_cs);
    if (!qc.buf_out)
    {
        r = qc.real ? qc.real_render : NULL;
        LeaveCriticalSection(&qc_cs);
        return r ? ((HRESULT (WINAPI *)(void *, UINT32, DWORD))(*r)[4])(r, frames, flags) : S_OK;
    }
    qc.buf_out = FALSE;
    if (!qc.real && ((flags & 2 /* AUDCLNT_BUFFERFLAGS_SILENT */) || qc_is_silence(qc.buf, frames)))
    {
        qc_advance();
        qc.written += frames;
    }
    else if (SUCCEEDED(hr = qc_make_real("audio written")) && (r = qc.real_render)
             && SUCCEEDED(hr = ((HRESULT (WINAPI *)(void *, UINT32, BYTE **))(*r)[3])(r, frames, &dst)))
    {
        memcpy(dst, qc.buf, frames * qc.align);
        hr = ((HRESULT (WINAPI *)(void *, UINT32, DWORD))(*r)[4])(r, frames, flags);
    }
    LeaveCriticalSection(&qc_cs);
    return hr;
}

static HRESULT WINAPI qcv_GetChannelCount(void *iface, UINT32 *count) { *count = qc.channels; return S_OK; }
static HRESULT WINAPI qcv_SetAllVolumes(void *iface, UINT32 count, const float *levels)
{
    HRESULT hr = S_OK;
    EnterCriticalSection(&qc_cs);
    if (count != qc.channels || !levels) hr = E_INVALIDARG;
    else
    {
        memcpy(qc.vol, levels, count * sizeof(float));
        if (qc.real && qc.real_vol) hr = ((HRESULT (WINAPI *)(void *, UINT32, const float *))(*qc.real_vol)[6])(qc.real_vol, count, levels);
    }
    LeaveCriticalSection(&qc_cs);
    return hr;
}
static HRESULT WINAPI qcv_GetAllVolumes(void *iface, UINT32 count, float *levels)
{
    if (count != qc.channels || !levels) return E_INVALIDARG;
    memcpy(levels, qc.vol, count * sizeof(float));
    return S_OK;
}
static HRESULT WINAPI qcv_SetChannelVolume(void *iface, UINT32 index, float level)
{
    float levels[8];
    if (index >= qc.channels) return E_INVALIDARG;
    memcpy(levels, qc.vol, sizeof(levels));
    levels[index] = level;
    return qcv_SetAllVolumes(iface, qc.channels, levels);
}
static HRESULT WINAPI qcv_GetChannelVolume(void *iface, UINT32 index, float *level)
{
    if (index >= qc.channels || !level) return E_INVALIDARG;
    *level = qc.vol[index];
    return S_OK;
}

static void *qc_render_vtbl[] = { qcp_QueryInterface, qcp_AddRef, qcp_Release, qcr_GetBuffer, qcr_ReleaseBuffer };
static void *qc_volume_vtbl[] = { qcp_QueryInterface, qcp_AddRef, qcp_Release, qcv_GetChannelCount, qcv_SetChannelVolume,
                                  qcv_GetChannelVolume, qcv_SetAllVolumes, qcv_GetAllVolumes };

/* --- IAudioClient methods --- */
static HRESULT WINAPI my_ac_Initialize(void *client, int mode, DWORD flags, LONGLONG duration, LONGLONG period,
                                       const void *format, const GUID *session)
{
    const WAVEFORMATEX *wfx = format;
    static int enabled = -1;
    unsigned int i, size;

    if (enabled < 0) enabled = !GetEnvironmentVariableW(L"AM_AUDIO_IDLE_STREAM", NULL, 0);
    if (!enabled || mode != 0 /* shared */ || !(flags & 0x80000000) /* AUTOCONVERTPCM */ || !wfx)
        return real_ac_Initialize(client, mode, flags, duration, period, format, session);
    size = sizeof(*wfx) + (wfx->wFormatTag == WAVE_FORMAT_PCM ? 0 : wfx->cbSize);

    EnterCriticalSection(&qc_cs);
    if (qc.client || size > sizeof(qc.fmt) || !wfx->nBlockAlign || !wfx->nSamplesPerSec || wfx->nChannels > ARRAY_SIZE(qc.vol))
    {
        LeaveCriticalSection(&qc_cs);
        return qc.client == client ? 0x88890002 /* AUDCLNT_E_ALREADY_INITIALIZED */
                                   : real_ac_Initialize(client, mode, flags, duration, period, format, session);
    }
    qc.gen++;
    qc.real = FALSE;
    qc.mode = mode; qc.flags = flags; qc.duration = duration; qc.period = period;
    memcpy(qc.fmt, wfx, size);
    if ((qc.has_session = session != NULL)) qc.session = *session;
    qc.rate = wfx->nSamplesPerSec; qc.align = wfx->nBlockAlign; qc.channels = wfx->nChannels;
    qc.is_float = wfx->wBitsPerSample == 32 && (wfx->wFormatTag == 3 /* IEEE_FLOAT */
                  || (wfx->wFormatTag == 0xfffe && size >= 40 && *(const DWORD *)((const BYTE *)wfx + 24) == 3));
    qc.frames = (UINT32)(duration * qc.rate / 10000000);
    if (qc.frames < qc.rate / 100) qc.frames = qc.rate / 100;
    qc.buf = calloc(qc.frames, qc.align);
    qc.buf_out = FALSE; qc.started = FALSE; qc.event = NULL;
    qc.written = qc.played = 0;
    for (i = 0; i < ARRAY_SIZE(qc.vol); i++) qc.vol[i] = 1.0f;
    qc.real_render = qc.real_vol = NULL;
    qc.render = qc_render_vtbl; qc.volume = qc_volume_vtbl;
    if (!qc.buf) { LeaveCriticalSection(&qc_cs); return real_ac_Initialize(client, mode, flags, duration, period, format, session); }
    qc.client = client;
    LeaveCriticalSection(&qc_cs);
    MESSAGE("ampkg: idle audio client %p kept without a stream until it plays something\n", client);
    return S_OK;
}

static HRESULT WINAPI my_ac_GetBufferSize(void *client, UINT32 *frames)
{
    if (!qc_quiet(client)) return real_ac_GetBufferSize(client, frames);
    if (!frames) return E_POINTER;
    *frames = qc.frames;
    return S_OK;
}

static HRESULT WINAPI my_ac_GetStreamLatency(void *client, LONGLONG *latency)
{
    if (!qc_quiet(client)) return real_ac_GetStreamLatency(client, latency);
    if (!latency) return E_POINTER;
    *latency = 100000;      /* 10 ms */
    return S_OK;
}

static HRESULT WINAPI my_ac_GetCurrentPadding(void *client, UINT32 *padding)
{
    if (!qc_quiet(client)) return real_ac_GetCurrentPadding(client, padding);
    if (!padding) return E_POINTER;
    EnterCriticalSection(&qc_cs);
    qc_advance();
    *padding = (UINT32)(qc.written - qc.played);
    LeaveCriticalSection(&qc_cs);
    return S_OK;
}

static HRESULT WINAPI my_ac_Start(void *client)
{
    HRESULT hr = S_OK;
    HANDLE thread;

    EnterCriticalSection(&qc_cs);
    if (!qc_quiet(client)) { LeaveCriticalSection(&qc_cs); return real_ac_Start(client); }
    if (qc.started) hr = 0x88890005;        /* AUDCLNT_E_NOT_STOPPED */
    else
    {
        qc_advance();
        qc.started = TRUE;
        if (qc.thread_gen != qc.gen && (thread = CreateThread(NULL, 0, qc_thread, (void *)(LONG_PTR)qc.gen, 0, NULL)))
        {
            qc.thread_gen = qc.gen;
            NtClose(thread);
        }
    }
    LeaveCriticalSection(&qc_cs);
    return hr;
}

static HRESULT WINAPI my_ac_Stop(void *client)
{
    HRESULT hr;
    EnterCriticalSection(&qc_cs);
    if (!qc_quiet(client)) { LeaveCriticalSection(&qc_cs); return real_ac_Stop(client); }
    qc_advance();
    hr = qc.started ? S_OK : S_FALSE;
    qc.started = FALSE;
    LeaveCriticalSection(&qc_cs);
    return hr;
}

static HRESULT WINAPI my_ac_Reset(void *client)
{
    EnterCriticalSection(&qc_cs);
    if (!qc_quiet(client)) { LeaveCriticalSection(&qc_cs); return real_ac_Reset(client); }
    qc.written = qc.played = 0;
    LeaveCriticalSection(&qc_cs);
    return S_OK;
}

/* SetEventHandle on a quiet client: returns FALSE if the client is (now) a real one */
BOOL qc_SetEventHandle(void *client, HANDLE event, HRESULT *hr)
{
    HANDLE dup = NULL;
    EnterCriticalSection(&qc_cs);
    if (!qc_quiet(client)) { LeaveCriticalSection(&qc_cs); return FALSE; }
    if (qc.event) *hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);     /* what Wine returns for a second call */
    else if (!event || !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        *hr = E_INVALIDARG;
    else { qc.event = dup; *hr = S_OK; }
    LeaveCriticalSection(&qc_cs);
    return TRUE;
}

/* GetService on a quiet client: returns FALSE if the client is (now) a real one */
BOOL qc_GetService(void *client, const GUID *iid, void **obj, HRESULT *hr)
{
    EnterCriticalSection(&qc_cs);
    if (!qc_quiet(client)) { LeaveCriticalSection(&qc_cs); return FALSE; }
    if (iid && obj && (!memcmp(iid, &my_IID_IAudioRenderClient, sizeof(GUID)) || !memcmp(iid, &my_IID_IAudioStreamVolume, sizeof(GUID))))
    {
        *obj = !memcmp(iid, &my_IID_IAudioRenderClient, sizeof(GUID)) ? (void *)&qc.render : (void *)&qc.volume;
        ((ULONG (WINAPI *)(void *))(*(void ***)client)[1])(client);
        *hr = S_OK;
        LeaveCriticalSection(&qc_cs);
        return TRUE;
    }
    *hr = qc_make_real("other service requested");
    LeaveCriticalSection(&qc_cs);
    return FAILED(*hr);
}

/* the client is gone (its final Release returned 0) */
void qc_released(void *client)
{
    EnterCriticalSection(&qc_cs);
    if (qc.client == client)
    {
        TRACE("idle audio client %p released (%s)\n", client, qc.real ? "real" : "quiet");
        qc.gen++;               /* ends the timer thread */
        qc.client = NULL;
        qc.real = FALSE;
        if (qc.event) NtClose(qc.event);
        qc.event = NULL;
        free(qc.buf);
        qc.buf = NULL;
    }
    LeaveCriticalSection(&qc_cs);
}


void idle_patch_vtbl(void **vtbl)
{
    real_ac_Initialize = vtbl[3];        vtbl[3] = my_ac_Initialize;
    real_ac_GetBufferSize = vtbl[4];     vtbl[4] = my_ac_GetBufferSize;
    real_ac_GetStreamLatency = vtbl[5];  vtbl[5] = my_ac_GetStreamLatency;
    real_ac_GetCurrentPadding = vtbl[6]; vtbl[6] = my_ac_GetCurrentPadding;
    real_ac_Start = vtbl[10];            vtbl[10] = my_ac_Start;
    real_ac_Stop = vtbl[11];             vtbl[11] = my_ac_Stop;
    real_ac_Reset = vtbl[12];            vtbl[12] = my_ac_Reset;
}
