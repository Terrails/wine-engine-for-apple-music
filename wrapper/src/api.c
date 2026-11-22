/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Endpoints of the local control API (see api.h, server.h). Runs on the main thread. */
#include "common.h"
#include "api.h"
#include "play.h"
#include "player.h"
#include "signin.h"

int api_quit_requested;

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
static int all_lower(const char *s) { for (; *s; s++) if (*s < 'a' || *s > 'z') return 0; return 1; }

/* GET /status */
static void api_status(struct sreq *r)
{
    struct player_status st; struct json j;
    player_status(&st);
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
        json_raw(&j, ",\"positionMs\":%I64d,\"durationMs\":%I64d}", st.position_ms, st.duration_ms);
    }
    else json_raw(&j, "null");
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

/* POST /play?song=<id>&album=<id>[&storefront=<cc>][&queue=single] or /play?album=<id>[&storefront=<cc>]
 * A song is played in its album (the album queued, starting at the song) unless queue=single. */
static void api_play(struct sreq *r)
{
    char song[32], album[32], cc[8], queue[16];
    struct play_request req; struct play_result res;

    srv_param(r, "song", song, sizeof(song)); srv_param(r, "album", album, sizeof(album));
    srv_param(r, "storefront", cc, sizeof(cc)); srv_param(r, "queue", queue, sizeof(queue));
    if (!cc[0] && !GetEnvironmentVariableA("AM_STOREFRONT", cc, sizeof(cc))) strcpy(cc, "us");
    if (!all_lower(cc)) strcpy(cc, "us");
    if (!album[0] || !all_digits(album) || !all_digits(song) || !g_main_vm)
    { reply(r, 400, "{\"ok\":false,\"error\":\"need album=<id> (and song=<id> for a single song), numeric\"}"); return; }

    req.cc = cc; req.album = album; req.song = song; req.single = !strcmp(queue, "single");
    play_catalog(&req, &res);
    if (!res.found && SUCCEEDED(res.hr))
    {
        r->status = 404;
        snprintf(r->body, sizeof(r->body), "{\"ok\":false,\"error\":\"item not found on the album page\",\"pageError\":%s,\"loadMs\":%lu}",
                 res.page_error ? "true" : "false", res.ms);
        return;
    }
    r->status = SUCCEEDED(res.hr) ? 200 : 500;
    snprintf(r->body, sizeof(r->body), "{\"ok\":%s,\"hresult\":\"%08lx\",\"loadMs\":%lu}", SUCCEEDED(res.hr) ? "true" : "false", res.hr, res.ms);
}

/* POST /pause /resume /stop /next /previous: IPlayer methods without arguments */
static const struct { const char *path; int slot; } simple_cmds[] = {
    { "/resume", 12 }, { "/pause", 13 }, { "/stop", 14 }, { "/previous", 16 }, { "/next", 17 },
};

void api_handle(struct sreq *r)
{
    void *player = get_player(), *act = NULL; char v[32];
    if (!strcmp(r->path, "/status") && !strcmp(r->method, "GET")) { api_status(r); return; }
    if (strcmp(r->method, "POST")) { reply(r, 405, "{\"error\":\"use POST (GET only for /status)\"}"); return; }
    if (!strcmp(r->path, "/quit")) { api_quit_requested = 1; reply(r, 200, "{\"ok\":true}"); return; }
    if (!player) { reply(r, 503, "{\"error\":\"player not ready\"}"); return; }
    for (int i = 0; i < (int)ARRAY_SIZE(simple_cmds); i++)
        if (!strcmp(r->path, simple_cmds[i].path)) { api_result(r, CALL1(player, simple_cmds[i].slot, &act)); return; }
    if (!strcmp(r->path, "/play")) api_play(r);
    else if (!strcmp(r->path, "/quality")) api_quality(r);
    else if (!strcmp(r->path, "/seek"))
    {
        INT64 ticks = _atoi64(srv_param(r, "ms", v, sizeof(v))) * 10000;     /* TimeSpan: 100 ns units */
        api_result(r, ((HRESULT (WINAPI *)(void *, INT64, boolean, void **))VT(player)[22])(player, ticks, FALSE, &act));
    }
    else if (!strcmp(r->path, "/volume"))
    {
        double vol = atof(srv_param(r, "v", v, sizeof(v)));
        if (vol < 0) vol = 0;
        if (vol > 1) vol = 1;
        api_result(r, ((HRESULT (WINAPI *)(void *, double, void **))VT(player)[27])(player, vol, &act));
    }
    else reply(r, 404, "{\"error\":\"unknown endpoint\"}");
}
