/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Endpoints of the local control API (see api.h, server.h). Runs on the main thread. */
#include "common.h"
#include "api.h"
#include "play.h"
#include "player.h"
#include "queue.h"
#include "vec.h"
#include "signin.h"

int api_quit_requested;
/* the last volume /volume set (-1: none yet): PlayerState's Volume can be stale while nothing plays */
static double api_volume = -1;

static void reply(struct sreq *r, int status, const char *json)
{
    r->status = status;
    lstrcpynA(r->body, json, sizeof(r->body));
}

static void api_result(struct sreq *r, HRESULT hr)
{
    r->status = SUCCEEDED(hr) ? 200 : 500;
    snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\"}", SUCCEEDED(hr) ? "true" : "false", hr);
}

static int all_digits(const char *s) { for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static const char *const section_names[] = { "", "history", "current", "next-added", "next", "autoplay" };
static int all_lower(const char *s) { for (; *s; s++) if (*s < 'a' || *s > 'z') return 0; return 1; }

/* GET /status */
static void api_status(struct sreq *r)
{
    struct player_status st; struct json j;
    player_status(&st);
    if (!st.playing && api_volume >= 0) st.volume = api_volume;
    json_init(&j, r->body, sizeof(r->body));
    json_raw(&j, "{\"ready\":%s,\"signedIn\":%s,\"playing\":%s,\"paused\":%s,\"volume\":%.3f,\"quality\":\"%s\",\"nowPlaying\":",
             st.ready ? "true" : "false", account_signed_in() ? "true" : "false", st.playing ? "true" : "false",
             st.paused ? "true" : "false", st.volume, quality_get());
    if (st.has_item)
    {
        json_raw(&j, "{\"title\":"); json_str(&j, ws(st.title));
        json_raw(&j, ",\"songId\":"); json_str(&j, ws(st.song_id));
        json_raw(&j, ",\"genre\":"); json_str(&j, ws(st.genre));
        json_raw(&j, ",\"releaseDate\":"); json_str(&j, ws(st.release_date));
        json_raw(&j, ",\"artist\":"); json_str(&j, ws(st.artist));
        json_raw(&j, ",\"album\":"); json_str(&j, ws(st.album));
        json_raw(&j, ",\"format\":\"%s\",\"sampleRate\":", st.format); json_str(&j, ws(st.sample_rate));
        json_raw(&j, ",\"bitDepth\":"); json_str(&j, ws(st.bit_depth));
        json_raw(&j, ",\"positionMs\":%I64d,\"durationMs\":%I64d,\"queueItemId\":", st.position_ms, st.duration_ms);
        if (st.queue_item_id) json_raw(&j, "\"%I64u\"}", st.queue_item_id); else json_raw(&j, "\"\"}");
    }
    else json_raw(&j, "null");
    {
        int on, secs, n, shown = 0; struct queue_entry up[6]; DWORD t0 = GetTickCount();
        crossfade_get(&on, &secs);
        json_raw(&j, ",\"crossfade\":{\"on\":%s,\"seconds\":%d},\"autoplay\":%s,\"upNext\":[", on ? "true" : "false", secs,
                 queue_autoplay() ? "true" : "false");
        n = st.has_item ? queue_upcoming(up, ARRAY_SIZE(up), 1) : 0;
        for (int i = 0; i < n && shown < 5; i++)
        {
            char adam[32];
            /* the queue's sections follow a track change a moment later than the now-playing
             * item: skip the current item while it is still listed as up next */
            if (up[i].id == st.queue_item_id) continue;
            WideCharToMultiByte(CP_UTF8, 0, up[i].adam, -1, adam, sizeof(adam), NULL, NULL);
            json_raw(&j, "%s{\"songId\":", shown++ ? "," : ""); json_str(&j, adam);
            json_raw(&j, ",\"queueItemId\":\"%I64u\",\"section\":\"%s\"}", up[i].id, section_names[up[i].kind]);
        }
        json_raw(&j, "]");
        if (GetTickCount() - t0 > 20) LOG("status: queue took %lu ms", GetTickCount() - t0);
    }
    {   /* the agent watchdog: every agent call arms a 5 s timer; AgentIsHung while one overran.
         * AgentPhase names the call, but keeps its last value after a recovery, so only while hung */
        boolean hung = 0; HSTRING phase = NULL;
        if (g_sp) CALL1(g_sp, 18, &hung);   /* IServiceProvider.get_AgentIsHung */
        if (hung) CALL1(g_sp, 16, &phase);  /* get_AgentPhase */
        json_raw(&j, ",\"agentHung\":%s,\"agentPhase\":", hung ? "true" : "false");
        if (phase) json_str(&j, ws(phase)); else json_raw(&j, "null");
        if (phase) WindowsDeleteString(phase);
    }
    json_raw(&j, "}");
    player_status_free(&st);
    r->status = 200;
}

/* POST /quality?q=aac|lossless|hires */
static void api_quality(struct sreq *r)
{
    char q[16]; HRESULT hr;
    srv_param(r, "q", q, sizeof(q));
    hr = quality_set(q);
    if (hr == E_INVALIDARG) { reply(r, 400, "{\"ok\":false,\"error\":\"need q=aac|lossless|hires\"}"); return; }
    pump(200);
    LOG("API quality '%s' -> %08lx, now %s", q, hr, quality_get());
    r->status = SUCCEEDED(hr) ? 200 : 500;
    snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"quality\":\"%s\"}", SUCCEEDED(hr) ? "true" : "false", hr, quality_get());
}

static void api_command(struct sreq *r, HRESULT hr, void *act);

/* The agent can show an item that was just started as playing and stay at 0 ms, with no sound,
 * until a Pause and a Play (seen on every /next right after a /pause, 2026-11-26). */
#define START_SWITCH_MS 3000   /* how long the item may take to change (none: nothing next) */
#define START_MOVE_MS 1500     /* and the position, before a pause and play (chosen) */
static void player_cmd(void *player, int slot)
{
    void *act = NULL; HRESULT err;
    if (SUCCEEDED(CALL1(player, slot, &act)) && act) { await_async(act, 5, 100, &err); IUnknown_Release((IUnknown *)act); }
}
static void pause_play(void *player) { player_cmd(player, 13); player_cmd(player, 12); }   /* Pause, Play */

struct started { UINT64 id; INT64 pos; DWORD ms; int switched, kicks; };
/* wait until the current item is no longer `before` (0: any item), then until it plays off 0 ms;
 * pause and play it at once with `kick_now`, else after START_MOVE_MS at 0 ms, twice at most */
static void wait_started(void *player, UINT64 before, int kick_now, struct started *o)
{
    struct player_status st; DWORD t0 = GetTickCount(), since = 0;
    memset(o, 0, sizeof(*o));
    for (;;)
    {
        DWORD now = GetTickCount(); int playing;
        player_status(&st);
        o->id = st.has_item ? st.queue_item_id : 0; playing = st.playing; o->pos = st.position_ms;
        player_status_free(&st);
        if (!o->switched)
        {
            if (!o->id || o->id == before) { if (now - t0 >= START_SWITCH_MS) break; pump(50); continue; }
            o->switched = 1; since = now;
        }
        if (playing && o->pos > 0) break;
        if (o->kicks < 2 && ((kick_now && !o->kicks) || now - since >= START_MOVE_MS))
        {
            LOG("API start: item %I64u %s at %I64d ms, pause and play", o->id, playing ? "playing" : "not playing", o->pos);
            pause_play(player);
            o->kicks++;
            since = GetTickCount();
        }
        else if (o->kicks == 2 && now - since >= START_MOVE_MS) break;
        pump(50);
    }
    o->ms = GetTickCount() - t0;
}

static UINT64 current_item(int *paused)
{
    struct player_status st; UINT64 id;
    player_status(&st);
    id = st.has_item ? st.queue_item_id : 0;
    if (paused) *paused = st.paused;
    player_status_free(&st);
    return id;
}

/* the current item once it is no longer `before`, 0 if that takes START_SWITCH_MS (from t0) */
static UINT64 wait_new_item(UINT64 before, DWORD t0)
{
    UINT64 id;
    while (((id = current_item(NULL)) == 0 || id == before) && GetTickCount() - t0 < START_SWITCH_MS) pump(20);
    return id && id != before ? id : 0;
}

/* next=<song>&nextAlbum=<album> of /play and /next: 0 none, 1 both numeric, -1 malformed */
static int next_params(struct sreq *r, char *next, char *album)
{
    srv_param(r, "next", next, 32); srv_param(r, "nextAlbum", album, 32);
    if (!next[0] && !album[0]) return 0;
    return next[0] && album[0] && all_digits(next) && all_digits(album) ? 1 : -1;
}

/* with the player paused on the item that was just started: queue `next` behind it (Play Next),
 * then start the item again (wait_started() with a pause and play at once, which makes the agent
 * decide about a crossfade into it again). nid: its queue item ID ("" if it did not show up). */
static int queue_next_and_start(void *player, const char *cc, const char *next, const char *album,
                                char *nid, struct started *o)
{
    struct play_request nreq = { cc, album, next, 0 }; struct play_result nres = { 0 }; int queued;
    play_enqueue(&nreq, &nres, 0);
    queued = nres.found && SUCCEEDED(nres.hr);
    nid[0] = 0;
    if (nres.queue_item_id) sprintf(nid, "%I64u", nres.queue_item_id);
    wait_started(player, 0, 1, o);
    return queued;
}

static void next_reply(struct sreq *r, HRESULT hr, DWORD ms, int queued, const char *nid)
{
    r->status = SUCCEEDED(hr) ? 200 : 500;
    if (queued)   /* nextQueueItemId "" when it did not show up in the queue within 5 s */
        snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"loadMs\":%lu,\"nextQueueItemId\":\"%s\"}",
                 SUCCEEDED(hr) ? "true" : "false", hr, ms, nid);
    else
        snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"loadMs\":%lu,\"nextQueued\":false}",
                 SUCCEEDED(hr) ? "true" : "false", hr, ms);
}

static const char *storefront_param(struct sreq *r, char *cc)
{
    srv_param(r, "storefront", cc, 8);
    if (!cc[0] && !GetEnvironmentVariableA("AM_STOREFRONT", cc, 8)) strcpy(cc, "us");
    if (!all_lower(cc)) strcpy(cc, "us");
    return cc;
}

/* POST /next[?next=<song>&nextAlbum=<album>[&storefront=<cc>]]: SkipNext, and the next item started
 * (see wait_started(); at once when the player was paused before: a UI that is told to skip pauses
 * first). With next=: that song is queued behind the new item before it goes on, as with /play. */
static void api_next(struct sreq *r, void *player)
{
    char cc[8], next[32], next_album[32], nid[24] = "";
    void *act = NULL; HRESULT hr; int was_paused, with_next, queued = 0; struct started o = { 0 };
    UINT64 before, id = 0; DWORD t0;
    if ((with_next = next_params(r, next, next_album)) < 0)
    { reply(r, 400, "{\"ok\":false,\"error\":\"next=<id> needs nextAlbum=<id>, numeric\"}"); return; }
    storefront_param(r, cc);
    before = current_item(&was_paused);
    t0 = GetTickCount();
    hr = CALL1(player, 17, &act);                                       /* SkipNext */
    api_command(r, hr, act);
    if (FAILED(hr)) return;
    if (with_next)
    {
        if ((id = wait_new_item(before, t0)))
        {
            player_cmd(player, 13);                                     /* Pause */
            queued = queue_next_and_start(player, cc, next, next_album, nid, &o);
            LOG("API next next=%s: %I64u -> %I64u, %s, queue item %s, %d pause and play, started at %I64d ms after %lu ms",
                next, before, id, queued ? "queued" : "not queued", nid[0] ? nid : "-", o.kicks, o.pos, GetTickCount() - t0);
        }
        else LOG("API next next=%s: no new item in %d ms, not queued", next, START_SWITCH_MS);
        next_reply(r, hr, GetTickCount() - t0, queued, nid);
        return;
    }
    wait_started(player, before, was_paused, &o);
    LOG("API next: %I64u -> %I64u, %s at %I64d ms after %lu ms, %d pause and play", before, o.id,
        !o.switched ? "no new item" : o.pos > 0 ? "started" : "still", o.pos, o.ms, o.kicks);
}

/* POST /play?song=<id>&album=<id>[&storefront=<cc>][&queue=single] or /play?album=<id>[&storefront=<cc>]
 * A song is played in its album (the album queued, starting at the song) unless queue=single.
 * &next=<song>&nextAlbum=<album>: that song is also queued as Play Next before the played one goes on,
 * so the agent's decision about a crossfade at its end has it: the agent decides when it starts an
 * item, and an item queued later only counts after a Pause and a Play. The played song is paused as
 * soon as it is the current item, the next one queued, then it is paused and played (about the
 * enqueue's time later; it may have played a moment). */
static void api_play(struct sreq *r)
{
    char song[32], album[32], cc[8], queue[16], next[32], next_album[32], nid[24] = "";
    int queued = 0;
    struct play_request req; struct play_result res;
    struct started o = { 0 };
    void *player = get_player(); UINT64 before; int with_next;

    srv_param(r, "song", song, sizeof(song)); srv_param(r, "album", album, sizeof(album));
    srv_param(r, "queue", queue, sizeof(queue)); storefront_param(r, cc);
    if (!album[0] || !all_digits(album) || !all_digits(song) || !g_main_vm)
    { reply(r, 400, "{\"ok\":false,\"error\":\"need album=<id> (and song=<id> for a single song), numeric\"}"); return; }
    with_next = next_params(r, next, next_album);
    if (with_next < 0 || (with_next && !song[0]))
    { reply(r, 400, "{\"ok\":false,\"error\":\"next=<id> needs nextAlbum=<id> and a song, numeric\"}"); return; }

    before = current_item(NULL);
    req.cc = cc; req.album = album; req.song = song; req.single = !strcmp(queue, "single");
    play_catalog(&req, &res);
    if (with_next && res.found && SUCCEEDED(res.hr))
    {
        DWORD t0 = GetTickCount();
        if (wait_new_item(before, t0))
        {
            player_cmd(player, 13);                                     /* Pause */
            queued = queue_next_and_start(player, cc, next, next_album, nid, &o);
            LOG("API play next=%s: %s, queue item %s, %d pause and play, started at %I64d ms after %lu ms",
                next, queued ? "queued" : "not queued", nid[0] ? nid : "-", o.kicks, o.pos, GetTickCount() - t0);
        }
        else LOG("API play next=%s: the played item did not show up in %d ms, not queued", next, START_SWITCH_MS);
    }
    if (!res.found && SUCCEEDED(res.hr))
    {
        r->status = 404;
        snprintf(r->body, sizeof(r->body), "{\"ok\":false,\"error\":\"item not found on the album page\",\"pageError\":%s,\"loadMs\":%lu}",
                 res.page_error ? "true" : "false", res.ms);
        return;
    }
    if (with_next) { next_reply(r, res.hr, res.ms, queued, nid); return; }
    r->status = SUCCEEDED(res.hr) ? 200 : 500;
    snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"loadMs\":%lu}", SUCCEEDED(res.hr) ? "true" : "false", res.hr, res.ms);
}

/* POST /crossfade?on=0|1[&s=<seconds 1..12>] */
static void api_crossfade(struct sreq *r)
{
    char on[8], sec[8]; int secs, now_on, now_s; HRESULT hr;
    srv_param(r, "on", on, sizeof(on)); srv_param(r, "s", sec, sizeof(sec));
    secs = sec[0] && all_digits(sec) ? atoi(sec) : -1;
    if ((strcmp(on, "0") && strcmp(on, "1")) || (sec[0] && (secs < 1 || secs > 12)) || !get_playback_settings())
    { reply(r, 400, "{\"ok\":false,\"error\":\"need on=0|1 and optionally s=<seconds 1..12>\"}"); return; }
    hr = crossfade_set(on[0] == '1', secs > 0 ? secs : 0);
    pump(100);
    crossfade_get(&now_on, &now_s);
    LOG("API crossfade on=%s s=%s -> %08lx, now on=%d %d s", on, sec, hr, now_on, now_s);
    r->status = SUCCEEDED(hr) ? 200 : 500;
    snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"crossfade\":{\"on\":%s,\"seconds\":%d}}",
             SUCCEEDED(hr) ? "true" : "false", hr, now_on ? "true" : "false", now_s);
}

/* Autoplay asked for while the service had played nothing yet: the app takes the call but keeps
 * the setting as it was, so it is set again once something plays (api_idle). -1: nothing pending. */
static int autoplay_wanted = -1;

/* POST /autoplay?on=0|1 */
static void api_autoplay(struct sreq *r)
{
    char on[8]; HRESULT hr; int want, now;
    srv_param(r, "on", on, sizeof(on));
    if (strcmp(on, "0") && strcmp(on, "1")) { reply(r, 400, "{\"ok\":false,\"error\":\"need on=0|1\"}"); return; }
    want = on[0] == '1';
    hr = queue_set_autoplay(want);
    for (int i = 0; i < 20 && SUCCEEDED(hr) && queue_autoplay() != want; i++) pump(50);
    now = queue_autoplay();
    autoplay_wanted = SUCCEEDED(hr) && now != want ? want : -1;
    LOG("API autoplay %s -> %08lx, now %d%s", on, hr, now, autoplay_wanted >= 0 ? " (set again once something plays)" : "");
    r->status = FAILED(hr) ? 500 : autoplay_wanted >= 0 ? 202 : 200;
    snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"autoplay\":%s%s}",
             SUCCEEDED(hr) ? "true" : "false", hr, now ? "true" : "false",
             autoplay_wanted >= 0 ? ",\"pending\":true" : "");
}

/* Between requests: apply a pending /autoplay once something plays (at most every 2 s). */
void api_idle(void)
{
    static DWORD last; struct player_status st; HRESULT hr;
    if (autoplay_wanted < 0) return;
    if (queue_autoplay() == autoplay_wanted) { autoplay_wanted = -1; return; }
    if (GetTickCount() - last < 2000) return;
    player_status(&st);
    if (st.playing)
    {
        last = GetTickCount();
        hr = queue_set_autoplay(autoplay_wanted);
        LOG("autoplay %d set again after playback started -> %08lx, now %d", autoplay_wanted, hr, queue_autoplay());
    }
    player_status_free(&st);
}

/* POST /enqueue?song=<id>&album=<id>[&storefront=<cc>][&at=next|end]: Play Next (default) or Add to Queue */
static void api_enqueue(struct sreq *r)
{
    char song[32], album[32], cc[8], at[8];
    struct play_request req; struct play_result res; struct player_status st; int playing;

    srv_param(r, "song", song, sizeof(song)); srv_param(r, "album", album, sizeof(album));
    srv_param(r, "storefront", cc, sizeof(cc));
    if (!cc[0] && !GetEnvironmentVariableA("AM_STOREFRONT", cc, sizeof(cc))) strcpy(cc, "us");
    if (!all_lower(cc)) strcpy(cc, "us");
    if (!album[0] || !song[0] || !all_digits(album) || !all_digits(song) || !g_main_vm)
    { reply(r, 400, "{\"ok\":false,\"error\":\"need song=<id>&album=<id>, numeric\"}"); return; }
    srv_param(r, "at", at, sizeof(at));
    if (at[0] && strcmp(at, "next") && strcmp(at, "end"))
    { reply(r, 400, "{\"ok\":false,\"error\":\"at must be next or end\"}"); return; }
    player_status(&st);
    playing = st.has_item && st.queue_item_id;
    player_status_free(&st);
    if (!playing) { reply(r, 409, "{\"ok\":false,\"error\":\"nothing playing\"}"); return; }

    req.cc = cc; req.album = album; req.song = song; req.single = 0;
    play_enqueue(&req, &res, !strcmp(at, "end"));
    if (api_quit_requested) { reply(r, 503, "{\"error\":\"the service is quitting\"}"); return; }   /* answered meanwhile */
    if (!res.found && SUCCEEDED(res.hr))
    {
        r->status = 404;
        snprintf(r->body, sizeof(r->body), "{\"ok\":false,\"error\":\"item not found on the album page\",\"pageError\":%s,\"loadMs\":%lu}",
                 res.page_error ? "true" : "false", res.ms);
        return;
    }
    {   /* queueItemId "" when the song did not show up in the queue within 5 s */
        char id[24] = "";
        if (res.queue_item_id) sprintf(id, "%I64u", res.queue_item_id);
        r->status = SUCCEEDED(res.hr) ? 200 : 500;
        snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"queueItemId\":\"%s\",\"loadMs\":%lu}",
                 SUCCEEDED(res.hr) ? "true" : "false", res.hr, id, res.ms);
    }
}

/* POST /dequeue[?id=<queueItemId>]: that item, or everything after the current one except autoplay.
 * Never the current item: the sections still list it as up next for a moment after a track change */
static void api_dequeue(struct sreq *r)
{
    static struct queue_entry up[VEC_MAX]; static UINT64 ids[VEC_MAX];
    char id[32]; struct player_status st; UINT64 cur; int n = 0; HRESULT hr;
    srv_param(r, "id", id, sizeof(id));
    if (id[0] && !all_digits(id)) { reply(r, 400, "{\"ok\":false,\"error\":\"id must be a numeric queueItemId\"}"); return; }
    player_status(&st);
    cur = st.has_item ? st.queue_item_id : 0;
    player_status_free(&st);
    if (id[0]) { if ((ids[n] = _strtoui64(id, NULL, 10)) != cur) n++; }
    else
    {
        int m = queue_upcoming(up, ARRAY_SIZE(up), 0);
        for (int i = 0; i < m; i++) if (up[i].id != cur) ids[n++] = up[i].id;
    }
    /* the list was read after the current item; look again right before deleting, as the agent can
     * move on to the next item between the two (nothing here pumps, but its calls can dispatch) */
    player_status(&st);
    cur = st.has_item ? st.queue_item_id : 0;
    player_status_free(&st);
    for (int i = 0; i < n; i++) if (ids[i] == cur) ids[i--] = ids[--n];
    hr = queue_delete(ids, n);
    pump(100);
    LOG("API dequeue id=%s: %d item(s) -> %08lx", id[0] ? id : "(all upcoming)", n, hr);
    r->status = SUCCEEDED(hr) ? 200 : 500;
    snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"removed\":%d}", SUCCEEDED(hr) ? "true" : "false", hr, n);
}

/* POST /pause /resume /stop /previous: IPlayer methods without arguments (/next: api_next()) */
static const struct { const char *path; int slot; } simple_cmds[] = {
    { "/resume", 12 }, { "/pause", 13 }, { "/stop", 14 }, { "/previous", 16 },
};

/* Answer a player command once AMP.Services has run it (a few ms, on its own threads; 0.5 s at most):
 * commands in flight together reach the agent in any order, so the next request must not start before
 * (of several /volume queued behind an /enqueue the last one did not always win). */
static void api_command(struct sreq *r, HRESULT hr, void *act)
{
    HRESULT err;
    api_result(r, hr);
    if (FAILED(hr) || !act) return;
    await_async(act, 5, 100, &err);
    IUnknown_Release((IUnknown *)act);
}

void api_handle(struct sreq *r)
{
    void *player = get_player(), *act = NULL; char v[32]; HRESULT hr;
    if (!strcmp(r->path, "/status") && !strcmp(r->method, "GET")) { api_status(r); return; }
    if (strcmp(r->method, "POST")) { reply(r, 405, "{\"error\":\"use POST (GET only for /status)\"}"); return; }
    if (!strcmp(r->path, "/quit")) { api_quit_requested = 1; reply(r, 200, "{\"ok\":true}"); return; }
    /* requests queued behind a /quit must not start anything: the engine is being stopped */
    if (api_quit_requested) { reply(r, 503, "{\"error\":\"the service is quitting\"}"); return; }
    if (!player) { reply(r, 503, "{\"error\":\"player not ready\"}"); return; }
    for (int i = 0; i < (int)ARRAY_SIZE(simple_cmds); i++)
        if (!strcmp(r->path, simple_cmds[i].path))
        {
            hr = CALL1(player, simple_cmds[i].slot, &act);
            api_command(r, hr, act);
            return;
        }
    if (!strcmp(r->path, "/next")) api_next(r, player);
    else if (!strcmp(r->path, "/play")) api_play(r);
    else if (!strcmp(r->path, "/quality")) api_quality(r);
    else if (!strcmp(r->path, "/crossfade")) api_crossfade(r);
    else if (!strcmp(r->path, "/autoplay")) api_autoplay(r);
    else if (!strcmp(r->path, "/enqueue")) api_enqueue(r);
    else if (!strcmp(r->path, "/dequeue")) api_dequeue(r);
    else if (!strcmp(r->path, "/seek"))
    {
        INT64 ticks = _atoi64(srv_param(r, "ms", v, sizeof(v))) * 10000;     /* TimeSpan: 100 ns units */
        hr = ((HRESULT (WINAPI *)(void *, INT64, boolean, void **))VT(player)[22])(player, ticks, FALSE, &act);
        api_command(r, hr, act);
    }
    else if (!strcmp(r->path, "/volume"))
    {
        double vol = atof(srv_param(r, "v", v, sizeof(v)));
        if (vol < 0) vol = 0;
        if (vol > 1) vol = 1;
        hr = ((HRESULT (WINAPI *)(void *, double, void **))VT(player)[27])(player, vol, &act);
        if (SUCCEEDED(hr)) api_volume = vol;
        api_command(r, hr, act);
    }
    else reply(r, 404, "{\"error\":\"unknown endpoint\"}");
}
