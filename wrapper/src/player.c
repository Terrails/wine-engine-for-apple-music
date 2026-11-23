/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Playback objects of the app (see player.h). */
#include "player.h"

static const GUID IID_IPlayerStatics = {0x34d100f9,0x0f0a,0x5980,{0x8f,0x04,0x7a,0xf5,0x7f,0xdb,0x29,0xb0}};
static const GUID IID_INowPlayingItem = {0x2dc5f65c,0x28f6,0x58aa,{0x87,0xcf,0xe4,0xa7,0xdf,0x9b,0x9a,0x4b}};
static const GUID IID_IContentDescriptorDriven = {0x0db3eb30,0xb156,0x5201,{0xac,0x4a,0x13,0x26,0x64,0x8c,0x30,0x7b}};
static const GUID IID_IPlayerState = {0xbefb99eb,0x336f,0x5199,{0x9f,0xb2,0x63,0x63,0x5b,0xc4,0x73,0x19}};

/* PSAudioQuality: 1 = AAC 256, 2 = lossless ALAC up to 48 kHz, 3 = hi-res lossless up to 192 kHz */
static const char *const quality_names[] = { "unknown", "aac", "lossless", "hires" };
/* AudioFormat of the playing item */
static const char *const format_names[] = { "other", "lossless", "hires", "atmos", "spatial", "surround" };

void *get_player(void)
{
    static void *player;
    if (!player)
    {
        void *statics = get_statics(L"AMP.Services.Player", &IID_IPlayerStatics);
        if (statics) CALL1(statics, 6, &player);     /* Player.Instance */
    }
    return player;
}

void *get_playback_settings(void)
{
    static void *ps;
    if (!ps)
    {
        void *as = NULL;
        CALL1(g_sp, 27, &as);                       /* ServiceProvider.AppSettings */
        if (as) CALL1(as, 8, &ps);                  /* PlaybackSettings */
    }
    return ps;
}

const char *quality_get(void)
{
    void *ps = get_playback_settings(); boolean lossless = 0; INT32 q = 0;
    if (!ps) return "unknown";
    CALL1(ps, 21, &lossless);                       /* LosslessAudio */
    CALL1(ps, 23, &q);                              /* StreamingAudioQuality */
    if (!lossless) return "aac";
    return q >= 0 && q < (INT32)ARRAY_SIZE(quality_names) ? quality_names[q] : "unknown";
}

HRESULT quality_set(const char *name)
{
    void *ps = get_playback_settings(); INT32 want = 0; HRESULT hr;
    for (int i = 1; i < (int)ARRAY_SIZE(quality_names); i++) if (!strcmp(name, quality_names[i])) want = i;
    if (!ps || !want) return E_INVALIDARG;
    hr = ((HRESULT (WINAPI *)(void *, boolean))VT(ps)[22])(ps, want > 1);              /* put_LosslessAudio */
    if (SUCCEEDED(hr)) hr = ((HRESULT (WINAPI *)(void *, INT32))VT(ps)[24])(ps, want);  /* put_StreamingAudioQuality */
    return hr;
}

void crossfade_get(int *on, int *seconds)
{
    void *ps = get_playback_settings(); boolean b = 0; INT32 v = 0;
    if (ps) { CALL1(ps, 11, &b); CALL1(ps, 13, &v); }   /* CrossfadeSongs, CrossfadeValue (seconds) */
    *on = b; *seconds = v;
}

HRESULT crossfade_set(int on, int seconds)
{
    void *ps = get_playback_settings(); HRESULT hr = S_OK;
    if (!ps) return E_UNEXPECTED;
    if (seconds) hr = ((HRESULT (WINAPI *)(void *, INT32))VT(ps)[14])(ps, seconds);    /* put_CrossfadeValue */
    if (SUCCEEDED(hr)) hr = ((HRESULT (WINAPI *)(void *, boolean))VT(ps)[12])(ps, !!on);  /* put_CrossfadeSongs */
    return hr;
}

/* Artist and Album of a catalogue item are empty; its Subtitle is "<artist> — <album>" */
static void split_subtitle(void *ini, struct player_status *st)
{
    HSTRING sub = NULL; const WCHAR *s, *dash;
    CALL1(ini, 6, &sub);                              /* Subtitle */
    s = wz(sub);
    WindowsDeleteString(st->artist); WindowsDeleteString(st->album);
    st->artist = st->album = NULL;
    if ((dash = wcsstr(s, L" \x2014 ")))
    {
        WindowsCreateString(s, dash - s, &st->artist);
        WindowsCreateString(dash + 3, lstrlenW(dash + 3), &st->album);
    }
    else WindowsCreateString(s, lstrlenW(s), &st->artist);
    WindowsDeleteString(sub);
}

void player_status(struct player_status *st)
{
    void *player = get_player(), *ist = NULL, *ini = NULL; IUnknown *state = NULL, *item = NULL;
    boolean playing = 0, paused = 0, placeholder = 0; INT32 format = 0; INT64 pos = 0, dur = 0;

    memset(st, 0, sizeof(*st));
    st->format = format_names[0];
    if (!player) return;
    st->ready = 1;
    CALL1(player, 7, &state);
    CALL1(player, 6, &item);
    if (state && SUCCEEDED(IUnknown_QueryInterface(state, &IID_IPlayerState, &ist)))
    {
        CALL1(ist, 25, &playing); CALL1(ist, 24, &paused); CALL1(ist, 21, &st->volume);
        st->playing = playing; st->paused = paused;
        IUnknown_Release((IUnknown *)ist);
    }
    if (item && SUCCEEDED(IUnknown_QueryInterface(item, &IID_INowPlayingItem, &ini)))
    {
        void *cdd = NULL; IUnknown *cd = NULL;
        CALL1(ini, 19, &st->title); CALL1(ini, 7, &st->artist); CALL1(ini, 8, &st->album);
        CALL1(ini, 21, &st->genre); CALL1(ini, 18, &st->release_date);
        if (!WindowsGetStringLen(st->artist) && !WindowsGetStringLen(st->album)) split_subtitle(ini, st);
        if (SUCCEEDED(IUnknown_QueryInterface(item, &IID_IContentDescriptorDriven, &cdd)))
        {
            CALL1(cdd, 6, &cd);                       /* ContentDescriptor */
            if (cd) { CALL1(cd, 21, &st->song_id); IUnknown_Release(cd); }   /* AdamId */
            IUnknown_Release((IUnknown *)cdd);
        }
        CALL1(ini, 30, &pos); CALL1(ini, 31, &dur); CALL1(ini, 24, &placeholder);
        CALL1(ini, 45, &st->queue_item_id);           /* PlayQueueItemID */
        CALL1(ini, 38, &format); CALL1(ini, 39, &st->sample_rate); CALL1(ini, 40, &st->bit_depth);
        st->has_item = !placeholder && WindowsGetStringLen(st->title);
        if (format >= 0 && format < (INT32)ARRAY_SIZE(format_names)) st->format = format_names[format];
        st->position_ms = pos / 10000;              /* TimeSpan: 100 ns units */
        st->duration_ms = dur / 10000;
        IUnknown_Release((IUnknown *)ini);
    }
    if (state) IUnknown_Release(state);
    if (item) IUnknown_Release(item);
}

void player_status_free(struct player_status *st)
{
    WindowsDeleteString(st->title); WindowsDeleteString(st->artist); WindowsDeleteString(st->album);
    WindowsDeleteString(st->sample_rate); WindowsDeleteString(st->bit_depth);
    WindowsDeleteString(st->song_id); WindowsDeleteString(st->genre); WindowsDeleteString(st->release_date);
    memset(st, 0, sizeof(*st));
}
