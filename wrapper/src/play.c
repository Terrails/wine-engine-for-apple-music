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
#include "vec.h"

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

/* open the album page and wait (up to 10 s, navigating again when needed) until ready() finds
 * what is needed on it; returns nonzero then. Sets res->page_error. */
typedef int (*page_ready_fn)(void *cpage, void *page, int loading, void *ctx);

static int load_album_page(const struct play_request *req, page_ready_fn ready_fn, void *ctx, struct play_result *res,
                           const char *what)
{
    WCHAR url[256];
    boolean loading = 1, err = 0; DWORD t0 = GetTickCount(); int ready = 0;

    if (req->song[0]) swprintf(url, ARRAY_SIZE(url), L"musics://music.apple.com/%hs/album/%hs?i=%hs", req->cc, req->album, req->song);
    else swprintf(url, ARRAY_SIZE(url), L"musics://music.apple.com/%hs/album/%hs", req->cc, req->album);
    if (FAILED(res->hr = open_deeplink(url))) return 0;

    for (int i = 0; i < 100 && !ready; i++)             /* up to 10 s */
    {
        void *cpage, *page = NULL;
        pump(100);
        if (!(cpage = current_content_page())) continue;
        page_state(cpage, &loading, &err, &page);
        if (page && i >= 2) ready = ready_fn(cpage, page, loading, ctx);
        /* now and then (about 1 in 12 quick switches) the navigation ends on a loaded page without
         * the item, and stays that way; asking again works */
        if (page && !ready && !loading && (i == 25 || i == 55))
        {
            LOG("%s: item not on the page after %lu ms, navigating again", what, GetTickCount() - t0);
            open_deeplink(url);
        }
        if (page && !ready && i == 99) page_list(cpage, NULL);
        if (page) IUnknown_Release((IUnknown *)page);
        IUnknown_Release((IUnknown *)cpage);
        if (oslog_agent_lost) break;
    }
    LOG("%s song=%s album=%s -> %s after %lu ms (loading=%d pageError=%d)", what, req->song, req->album,
        ready ? "found" : "NOT FOUND", GetTickCount() - t0, loading, err);
    res->page_error = err;
    res->found = ready;
    return ready;
}

struct play_ctx { const WCHAR *song, *album; IInspectable *track, *whole; const char **how; };

static int play_ready(void *cpage, void *page, int loading, void *ctx)
{
    struct play_ctx *p = ctx;
    return find_actions(cpage, page, loading, p->song, p->album, &p->track, &p->whole, p->how);
}

void play_catalog(const struct play_request *req, struct play_result *res)
{
    WCHAR song[32], album[32];
    struct play_ctx p = { song, album, NULL, NULL, &res->how };
    DWORD t0 = GetTickCount();

    memset(res, 0, sizeof(*res));
    res->how = "direct";
    queue_open();
    MultiByteToWideChar(CP_UTF8, 0, req->song, -1, song, ARRAY_SIZE(song));
    MultiByteToWideChar(CP_UTF8, 0, req->album, -1, album, ARRAY_SIZE(album));
    if (!load_album_page(req, play_ready, &p, res, "play")) { res->ms = GetTickCount() - t0; return; }
    if (!song[0]) res->hr = dispatch_action(p.whole);
    else if (req->single || !p.whole) { res->hr = dispatch_action(p.track); res->how = "song alone"; }
    else res->hr = play_in_album(p.whole, p.track, song, &res->how);
    res->ms = GetTickCount() - t0;
    LOG("play dispatched (%s) -> %08lx at %lu ms", res->how, res->hr, res->ms);
    if (p.track) IInspectable_Release(p.track);
    if (p.whole) IInspectable_Release(p.whole);
}

/* ---- Play Next and Add to Queue ----
 * The app has no "queue this catalogue song" call (IPlayer.StartOrQueueLibraryPlaybackAsync with
 * QueueNext accepts catalogue descriptors and does nothing; IPlayQueueViewModel's
 * InsertItemsIntoPlayQueue only moves queue items). What the UI does is the track's context menu:
 * MenuPresenter.GetObjectMenuItemsAsync(sender, [row]) builds it (Play "<title>", Play Next,
 * Add to Queue, Create Station, ...) and the chosen item's Segue is an action run like a play
 * action. The items have no IDs, only localised titles. The app's menu builder adds Play, Shuffle,
 * Play Next, Add to Queue and Create Station in that order (Shuffle: from the binary only). */
static const GUID IID_IMenuItem = {0xbc1ce5b9,0x11c2,0x5748,{0x9e,0x5e,0xa7,0xfb,0x27,0x48,0x01,0x01}};
static const GUID IID_IVector_IInspectable = {0xb32bdca4,0x5e52,0x5b27,{0xbc,0x5d,0xd6,0x6a,0x1a,0x26,0x8c,0x2a}};

#define ENQUEUE_WAIT_MS 5000   /* how long the song may take to appear in the queue */

/* the two menu commands. Titles are the English ones. Only Play Next has a positional fallback: where
 * Add to Queue is missing, the item in its place is Create Station, which must not be run by mistake */
static const struct queue_cmd { const char *name; const WCHAR *title; int after_play; } queue_cmds[2] = {
    { "Play Next",    L"Play Next",    1 },
    { "Add to Queue", L"Add to Queue", 0 },
};

struct row_ctx { const WCHAR *song; IInspectable *item; HSTRING title; };

static int row_cb(const struct page_item *it, void *ctx)
{
    struct row_ctx *r = ctx;
    if (!it->action || wcscmp(wz(it->adam), r->song)) return 0;   /* the track row has a play action */
    r->item = it->item;
    IInspectable_AddRef(r->item);
    WindowsDuplicateString(it->title, &r->title);
    return 1;
}

static int row_ready(void *cpage, void *page, int loading, void *ctx)
{
    struct row_ctx *r = ctx;
    if (!r->item) page_walk(cpage, row_cb, r, 0);
    return r->item != NULL;
}

/* the Segue of the menu item titled cmd->title; for Play Next only, else (another UI language) the
 * item right after the one naming the track (Play "<title>") */
static IInspectable *menu_segue(IInspectable *menu, const WCHAR *title, const struct queue_cmd *cmd)
{
    void *v = NULL; UINT32 n = 0; IInspectable *found = NULL, *after_play = NULL; int prev_names_track = 0;
    if (FAILED(IInspectable_QueryInterface(menu, &IID_IVector_IInspectable, &v))) return NULL;
    CALL1(v, 7, &n);
    for (UINT32 i = 0; i < n && !found; i++)
    {
        IInspectable *mi = NULL, *segue = NULL; void *m = NULL; HSTRING t = NULL; int names_track = 0;
        ((HRESULT (WINAPI *)(void *, UINT32, void **))VT(v)[6])(v, i, (void **)&mi);
        if (mi && SUCCEEDED(IInspectable_QueryInterface(mi, &IID_IMenuItem, &m)))
        {
            CALL1(m, 9, &t); CALL1(m, 11, &segue);                    /* Title, Segue */
            IUnknown_Release((IUnknown *)m);
            names_track = title[0] && wcsstr(wz(t), title);
            if (segue && !wcscmp(wz(t), cmd->title)) { found = segue; segue = NULL; }
            else if (cmd->after_play && segue && prev_names_track && !after_play) { after_play = segue; segue = NULL; }
            if (segue) IInspectable_Release(segue);
            WindowsDeleteString(t);
        }
        prev_names_track = names_track;
        if (mi) IInspectable_Release(mi);
    }
    IUnknown_Release((IUnknown *)v);
    if (found) { if (after_play) IInspectable_Release(after_play); return found; }
    if (after_play) LOG("enqueue: no item titled \"%s\" in the menu, using the one after the item naming the track", cmd->name);
    return after_play;
}

/* build the row's context menu and run the command's item */
static HRESULT run_queue_cmd(IInspectable *row, const WCHAR *title, const struct queue_cmd *cmd)
{
    void *mp = NULL, *vec, *op = NULL; IInspectable *menu = NULL, *segue; HRESULT hr, err = S_OK; INT32 st;
    CALL1(g_sp, 30, &mp);                            /* ServiceProvider.MenuPresenter */
    if (!mp) return E_FAIL;
    if (!(vec = vec_new(VEC_INSPECTABLE, &row, 1))) { IUnknown_Release((IUnknown *)mp); return E_OUTOFMEMORY; }
    hr = ((HRESULT (WINAPI *)(void *, void *, void *, void **))VT(mp)[6])(mp, NULL, vec, &op);   /* GetObjectMenuItemsAsync */
    IUnknown_Release((IUnknown *)vec);
    IUnknown_Release((IUnknown *)mp);
    if (FAILED(hr) || !op) return FAILED(hr) ? hr : E_FAIL;
    st = await_async(op, 20, 250, &err);                                  /* up to 5 s */
    if (st == 1) ((HRESULT (WINAPI *)(void *, void **))VT(op)[8])(op, (void **)&menu);   /* GetResults */
    IUnknown_Release((IUnknown *)op);
    if (!menu) { LOG("enqueue: no context menu (status %d, %08lx)", st, err); return FAILED(err) ? err : E_FAIL; }
    segue = menu_segue(menu, title, cmd);
    IInspectable_Release(menu);
    if (!segue) { LOG("enqueue: the context menu has no %s", cmd->name); return E_NOTIMPL; }
    hr = dispatch_action(segue);
    IInspectable_Release(segue);
    return hr;
}

void play_enqueue(const struct play_request *req, struct play_result *res, int at_end)
{
    const struct queue_cmd *cmd = &queue_cmds[!!at_end];
    WCHAR song[32];
    struct row_ctx r = { song, NULL, NULL };
    DWORD t0 = GetTickCount(), t1; UINT64 before; INT32 kind = 0;

    memset(res, 0, sizeof(*res));
    res->how = cmd->name;
    queue_open();
    MultiByteToWideChar(CP_UTF8, 0, req->song, -1, song, ARRAY_SIZE(song));
    if (!load_album_page(req, row_ready, &r, res, "enqueue")) { res->ms = GetTickCount() - t0; return; }
    before = queue_max_id();
    res->hr = run_queue_cmd(r.item, wz(r.title), cmd);
    t1 = GetTickCount();
    while (SUCCEEDED(res->hr) && !res->queue_item_id && GetTickCount() - t1 < ENQUEUE_WAIT_MS && !oslog_agent_lost)
    {
        pump(50);
        res->queue_item_id = queue_find(song, before, &kind);
        /* the station can be rebuilt meanwhile with the same song under a new ID; the walk reaches the
         * manual sections first */
        if (kind == QUEUE_AUTOPLAY) res->queue_item_id = 0;
    }
    res->kind = res->queue_item_id ? kind : 0;
    res->ms = GetTickCount() - t0;
    LOG("enqueue song=%s -> %08lx, queue item %I64u (section kind %d) after %lu ms (%lu ms after %s)", req->song,
        res->hr, res->queue_item_id, res->kind, res->ms, GetTickCount() - t1, cmd->name);
    IInspectable_Release(r.item);
    WindowsDeleteString(r.title);
}
