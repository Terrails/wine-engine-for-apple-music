/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Playing catalogue items (see play.h).
 *
 * The album page (musics://music.apple.com/<cc>/album/<album>?i=<song>) has a header item whose
 * play action queues the whole album, and a row per track whose action queues that track alone
 * (followed by autoplay). To play a song "in its album" like the real UI's track list does, run
 * the header's action and then jump to the song in the play queue. */
#include "play.h"
#include "oslog.h"
#include "page.h"
#include "player.h"
#include "queue.h"

#define JUMP_WAIT_MS 5000      /* how long the album may take to appear in the queue */

/* the actions for the request on the album page that is loading: the song's own (row) action
 * and the album's (header) action. Returns nonzero once what is needed is there. */
static int find_actions(void *cpage, void *page, int loading, const WCHAR *song, const WCHAR *album,
                        IInspectable **track, IInspectable **whole, const char **how)
{
    /* IsLoading can stay set for many seconds (it also covers the social/library extras) although
     * the track list is complete, so look for a song regardless */
    if (song[0])
    {
        if (!*track) *track = find_play_action(cpage, song);
        if (*track && !*whole) *whole = find_play_action(cpage, album);   /* header lockup */
        return *track != NULL;
    }
    if (loading) return 0;
    CALL1(page, 25, whole);                                             /* IPage.PlayAllAction */
    /* often null on a loaded album page: ask the page to build it, else use the play action of
     * the header lockup (its AdamId is the album's), which queues the whole album */
    if (!*whole)
    {
        CALL0(page, 32);                                                /* UpdatePlayAllAction */
        CALL1(page, 25, whole);
        if (*whole) *how = "UpdatePlayAllAction";
    }
    if (!*whole && (*whole = find_play_action(cpage, album))) *how = "header lockup";
    return *whole != NULL;
}

/* run the album's action, then jump to the song once it is in the queue; falls back to the
 * song's own action */
static HRESULT play_in_album(IInspectable *whole, IInspectable *track, const WCHAR *song, const char **how)
{
    UINT64 before = queue_max_id(), id = 0;
    INT32 kind = 0;
    DWORD t0 = GetTickCount();
    HRESULT hr = dispatch_action(whole);
    if (FAILED(hr)) return hr;
    while (!id && GetTickCount() - t0 < JUMP_WAIT_MS && !oslog_agent_lost)
    {
        pump(50);
        id = queue_find(song, before, &kind);
    }
    if (!id)
    {
        LOG("play: song not in the album's queue after %lu ms, playing it alone", GetTickCount() - t0);
        *how = "song alone (not in the album queue)";
        return dispatch_action(track);
    }
    if (kind == QUEUE_CURRENT)
    {
        /* the song is the album's first track: it is already starting, and jumping to the item
         * that is loading stalls the player (stays stopped at 0 ms) */
        *how = "album (the song is its first track)";
        LOG("play: the song is the album's current item after %lu ms, no jump", GetTickCount() - t0);
        return S_OK;
    }
    *how = "album, jumped to the song";
    {
        struct player_status st;
        player_status(&st);
        LOG("play: before the jump the player is on '%s' playing=%d at %I64d ms", ws(st.title), st.playing, st.position_ms);
        player_status_free(&st);
    }
    hr = queue_play_item(id);
    LOG("play: queue item %I64u is the song, jump after %lu ms -> %08lx", id, GetTickCount() - t0, hr);
    return hr;
}

void play_catalog(const struct play_request *req, struct play_result *res)
{
    WCHAR url[256], song[32], album[32];
    void *cpage = NULL, *page = NULL; IInspectable *track = NULL, *whole = NULL;
    boolean loading = 1, err = 0; DWORD t0 = GetTickCount(); int ready = 0;

    memset(res, 0, sizeof(*res));
    res->how = "direct";
    queue_open();
    MultiByteToWideChar(CP_UTF8, 0, req->song, -1, song, ARRAY_SIZE(song));
    MultiByteToWideChar(CP_UTF8, 0, req->album, -1, album, ARRAY_SIZE(album));
    if (song[0]) swprintf(url, ARRAY_SIZE(url), L"musics://music.apple.com/%hs/album/%hs?i=%hs", req->cc, req->album, req->song);
    else swprintf(url, ARRAY_SIZE(url), L"musics://music.apple.com/%hs/album/%hs", req->cc, req->album);
    if (FAILED(res->hr = open_deeplink(url))) return;

    for (int i = 0; i < 100 && !ready; i++)             /* up to 10 s */
    {
        pump(100);
        if (!(cpage = current_content_page())) continue;
        page_state(cpage, &loading, &err, &page);
        if (!page || i < 2) continue;
        ready = find_actions(cpage, page, loading, song, album, &track, &whole, &res->how);
        if (oslog_agent_lost) break;
        /* now and then (about 1 in 12 quick switches) the navigation ends on a loaded page without
         * the item, and stays that way; asking again works */
        if (!ready && !loading && (i == 25 || i == 55))
        {
            LOG("play: item not on the page after %lu ms, navigating again", GetTickCount() - t0);
            open_deeplink(url);
        }
    }
    LOG("play song=%s album=%s -> %s after %lu ms (loading=%d pageError=%d album action=%s)", req->song, req->album,
        ready ? "found" : "NOT FOUND", GetTickCount() - t0, loading, err, whole ? "yes" : "no");
    res->page_error = err;
    if (!ready)
    {
        if (cpage && page) page_list(cpage, NULL);
        res->ms = GetTickCount() - t0;
        return;
    }
    res->found = 1;
    if (!song[0]) res->hr = dispatch_action(whole);
    else if (req->single || !whole) { res->hr = dispatch_action(track); res->how = "song alone"; }
    else res->hr = play_in_album(whole, track, song, &res->how);
    res->ms = GetTickCount() - t0;
    LOG("play dispatched (%s) -> %08lx at %lu ms", res->how, res->hr, res->ms);
}
