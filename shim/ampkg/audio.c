/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg: IAudioClient lifetime fixes (see ampkg.h). */
#include "ampkg.h"

WINE_DEFAULT_DEBUG_CHANNEL(ampkg);

/* IAudioClient::SetEventHandle: keep the event alive for as long as the audio client lives.
 *
 * Wine's audio timer thread signals the very handle value the application passed in, until the
 * client is released. The app closes its event before that (harmless on Windows, where the audio
 * engine holds its own reference). The value is then reused, typically by combase for the
 * completion event of an outgoing COM call, and the stray SetEvent wakes that call before its
 * reply has arrived: it parses garbage and the agent crashes after a few song switches.
 *
 * So give Wine a duplicate, closed when the client is finally released (which stops the timer).
 * The audio client vtable is shared by all clients; it is patched once, found through a
 * throw-away client made when the app creates its first device enumerator. */

static const GUID my_CLSID_MMDeviceEnumerator = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const GUID my_IID_IMMDeviceEnumerator  = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const GUID my_IID_IAudioClient         = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};

static CRITICAL_SECTION ac_cs;
static CRITICAL_SECTION_DEBUG ac_cs_debug =
{
    0, 0, &ac_cs, { &ac_cs_debug.ProcessLocksList, &ac_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)"ampkg ac_cs" }
};
static CRITICAL_SECTION ac_cs = { &ac_cs_debug, -1, 0, 0, 0, 0 };
static struct { void *client; HANDLE event; } ac_events[64];
static LONG ac_patched;
ULONG (WINAPI *real_ac_Release)(void *);
HRESULT (WINAPI *real_ac_SetEventHandle)(void *, HANDLE);

HRESULT (WINAPI *real_ac_GetService)(void *, const GUID *, void **);

static HRESULT WINAPI my_ac_SetEventHandle(void *client, HANDLE event)
{
    HANDLE dup = NULL;
    HRESULT hr;
    unsigned int i;

    if (qc_SetEventHandle(client, event, &hr)) return hr;
    if (!event || !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return real_ac_SetEventHandle(client, event);
    hr = real_ac_SetEventHandle(client, dup);
    TRACE("client %p event %p -> %p, hr %#lx\n", client, event, dup, hr);
    if (FAILED(hr)) { NtClose(dup); return hr; }
    EnterCriticalSection(&ac_cs);
    for (i = 0; i < ARRAY_SIZE(ac_events); i++)
        if (!ac_events[i].client) { ac_events[i].client = client; ac_events[i].event = dup; break; }
    LeaveCriticalSection(&ac_cs);       /* table full: the duplicate is leaked, which is safe */
    return hr;
}

/* Second difference from Windows: Wine's IAudioSessionControl (from IAudioClient::GetService) holds
 * a reference on the audio client, and the app never releases its session control. So no client
 * was ever destroyed: every song switch left one more stream (visible as one more "AMPLibraryAgent"
 * output in PipeWire/PulseAudio), each with a timer thread still signalling its event.
 * Treat the session control's reference as weak: once it is the only one left, drop it. The session
 * control is then an orphan whose own final release must be swallowed (it would touch the freed
 * client); its other methods only use the session, which outlives clients. */
static const GUID my_IID_IAudioSessionControl = {0xf4b1a599,0x7266,0x4319,{0xa8,0xca,0xe7,0x0a,0xcb,0x11,0xe8,0xcd}};
static const GUID my_IID_IChannelAudioVolume  = {0x1c158861,0xb533,0x4b30,{0xb1,0xcf,0xe8,0x53,0xe5,0x1c,0x59,0xb8}};
static const GUID my_IID_ISimpleAudioVolume   = {0x87ce5498,0x68d6,0x44e5,{0x92,0x15,0x6d,0xa4,0x7e,0xf8,0x83,0xd8}};

static struct { void *client; void *ctl; } ac_ctls[64];
static void **ac_orphans;
static unsigned int ac_orphan_count, ac_orphan_size;
static int ac_in_ctl_release;
static ULONG (WINAPI *real_ctl_Release)(void *);

static ULONG ctl_addref(void *ctl) { return ((ULONG (WINAPI *)(void *))(*(void ***)ctl)[1])(ctl); }

static ULONG WINAPI my_ctl_Release(void *ctl)
{
    ULONG ref;
    unsigned int i;

    EnterCriticalSection(&ac_cs);
    for (i = 0; i < ac_orphan_count; i++)
    {
        if (ac_orphans[i] != ctl) continue;
        ctl_addref(ctl);
        if ((ref = real_ctl_Release(ctl)) == 1)
        {
            TRACE("orphaned session control %p: final release swallowed\n", ctl);
            ac_orphans[i] = ac_orphans[--ac_orphan_count];
            ref = 0;
        }
        else ref = real_ctl_Release(ctl);
        LeaveCriticalSection(&ac_cs);
        return ref;
    }
    ac_in_ctl_release++;            /* its final release releases the client: nothing special then */
    ref = real_ctl_Release(ctl);
    ac_in_ctl_release--;
    if (!ref)
        for (i = 0; i < ARRAY_SIZE(ac_ctls); i++)
            if (ac_ctls[i].ctl == ctl) ac_ctls[i].client = ac_ctls[i].ctl = NULL;
    LeaveCriticalSection(&ac_cs);
    return ref;
}

static HRESULT WINAPI my_ac_GetService(void *client, const GUID *iid, void **obj)
{
    HRESULT hr;
    void ***ctl = NULL;
    unsigned int i, free_slot = ARRAY_SIZE(ac_ctls);
    DWORD old;

    if (qc_GetService(client, iid, obj, &hr)) return hr;
    hr = real_ac_GetService(client, iid, obj);
    if (FAILED(hr) || !iid || !obj || !*obj) return hr;
    if (memcmp(iid, &my_IID_IAudioSessionControl, sizeof(GUID)) && memcmp(iid, &my_IID_IChannelAudioVolume, sizeof(GUID))
        && memcmp(iid, &my_IID_ISimpleAudioVolume, sizeof(GUID))) return hr;
    if (FAILED(((HRESULT (WINAPI *)(void *, const GUID *, void **))(*(void ***)*obj)[0])(*obj, &my_IID_IAudioSessionControl, (void **)&ctl)) || !ctl)
        return hr;

    EnterCriticalSection(&ac_cs);
    if (!real_ctl_Release && VirtualProtect(*ctl, 3 * sizeof(void *), PAGE_READWRITE, &old))
    {
        real_ctl_Release = (*ctl)[2];
        (*ctl)[2] = my_ctl_Release;
        VirtualProtect(*ctl, 3 * sizeof(void *), old, &old);
    }
    if (real_ctl_Release)
    {
        real_ctl_Release(ctl);      /* the reference from QueryInterface above */
        for (i = 0; i < ARRAY_SIZE(ac_ctls); i++)
        {
            if (ac_ctls[i].client == client) break;
            if (!ac_ctls[i].client && free_slot == ARRAY_SIZE(ac_ctls)) free_slot = i;
        }
        if (i == ARRAY_SIZE(ac_ctls) && free_slot < ARRAY_SIZE(ac_ctls))
        {
            ac_ctls[free_slot].client = client;
            ac_ctls[free_slot].ctl = ctl;
            TRACE("client %p has session control %p\n", client, ctl);
        }
    }
    else ((ULONG (WINAPI *)(void *))(*ctl)[2])(ctl);
    LeaveCriticalSection(&ac_cs);
    return hr;
}

static ULONG WINAPI my_ac_Release(void *client)
{
    ULONG ref;
    unsigned int i;

    /* held across the release so the address cannot be reused before the entries are gone */
    EnterCriticalSection(&ac_cs);
    ref = real_ac_Release(client);
    if (ref == 1 && !ac_in_ctl_release)
        for (i = 0; i < ARRAY_SIZE(ac_ctls); i++)
        {
            if (ac_ctls[i].client != client) continue;
            /* only the session control still holds the client */
            if (ac_orphan_count == ac_orphan_size)
            {
                unsigned int size = ac_orphan_size ? ac_orphan_size * 2 : 64;
                void **p = ac_orphans ? HeapReAlloc(GetProcessHeap(), 0, ac_orphans, size * sizeof(*p))
                                      : HeapAlloc(GetProcessHeap(), 0, size * sizeof(*p));
                if (!p) break;      /* keep the old behaviour: the stream stays */
                ac_orphans = p;
                ac_orphan_size = size;
            }
            ac_orphans[ac_orphan_count++] = ac_ctls[i].ctl;
            TRACE("client %p: dropping the reference held by session control %p\n", client, ac_ctls[i].ctl);
            ac_ctls[i].client = ac_ctls[i].ctl = NULL;
            ref = real_ac_Release(client);
            break;
        }
    if (!ref) qc_released(client);
    if (!ref)
        for (i = 0; i < ARRAY_SIZE(ac_events); i++)
            if (ac_events[i].client == client)
            {
                TRACE("client %p released, closing %p\n", client, ac_events[i].event);
                NtClose(ac_events[i].event);
                ac_events[i].client = NULL;
            }
    LeaveCriticalSection(&ac_cs);
    return ref;
}

static void patch_audio_client(void *unk)
{
    void ***enumerator = NULL, ***device = NULL, ***client = NULL;
    void **vtbl;
    DWORD old;

    if (ac_patched) return;
    if (FAILED(((HRESULT (WINAPI *)(void *, const GUID *, void **))(*(void ***)unk)[0])(unk, &my_IID_IMMDeviceEnumerator, (void **)&enumerator)))
        return;
    /* GetDefaultAudioEndpoint(eRender, eConsole), then IMMDevice::Activate(IAudioClient) */
    if (SUCCEEDED(((HRESULT (WINAPI *)(void *, int, int, void *))(*enumerator)[4])(enumerator, 0, 0, &device)) && device)
    {
        if (SUCCEEDED(((HRESULT (WINAPI *)(void *, const GUID *, DWORD, void *, void **))(*device)[3])
                      (device, &my_IID_IAudioClient, 1 /* CLSCTX_INPROC_SERVER */, NULL, (void **)&client)) && client)
        {
            vtbl = *client;
            EnterCriticalSection(&ac_cs);
            if (!ac_patched && VirtualProtect(vtbl, 15 * sizeof(void *), PAGE_READWRITE, &old))
            {
                real_ac_Release = vtbl[2];
                real_ac_SetEventHandle = vtbl[13];
                real_ac_GetService = vtbl[14];
                idle_patch_vtbl(vtbl);
                vtbl[14] = my_ac_GetService;
                vtbl[13] = my_ac_SetEventHandle;
                vtbl[2] = my_ac_Release;
                VirtualProtect(vtbl, 15 * sizeof(void *), old, &old);
                ac_patched = 1;
                MESSAGE("ampkg: audio client event handles are now kept alive (vtable %p)\n", vtbl);
            }
            LeaveCriticalSection(&ac_cs);
            ((ULONG (WINAPI *)(void *))(*client)[2])(client);
        }
        else WARN("no audio client\n");
        ((ULONG (WINAPI *)(void *))(*device)[2])(device);
    }
    else WARN("no default render endpoint yet\n");
    ((ULONG (WINAPI *)(void *))(*enumerator)[2])(enumerator);
}

static HRESULT WINAPI my_CoCreateInstance(const GUID *clsid, void *outer, DWORD ctx, const GUID *iid, void **obj)
{
    static HRESULT (WINAPI *real)(const GUID *, void *, DWORD, const GUID *, void **);
    HRESULT hr;

    if (!real)
    {
        ANSI_STRING str;
        void *fn = NULL;
        RtlInitAnsiString(&str, "CoCreateInstance");
        LdrGetProcedureAddress(LoadLibraryW(L"combase.dll"), &str, 0, &fn);
        real = fn;
    }
    hr = real(clsid, outer, ctx, iid, obj);
    if (SUCCEEDED(hr) && !ac_patched && clsid && obj && *obj && !memcmp(clsid, &my_CLSID_MMDeviceEnumerator, sizeof(GUID)))
        patch_audio_client(*obj);
    return hr;
}

const struct hook audio_hooks[] =
{
    HOOK(CoCreateInstance),
    { NULL }
};
