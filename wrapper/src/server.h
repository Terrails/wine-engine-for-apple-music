/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Local control API server: HTTP/1.1 + JSON on 127.0.0.1 (endpoints: api.c).
 *
 * A socket thread accepts connections (one short-lived thread each, at most 16) and queues
 * their requests for the main (UI) thread, which is the only thread allowed to talk to
 * AMP.Services; server_process() runs there, from the message loop, one request at a time in
 * order. The one exception: GET /status is answered while a /enqueue runs (server.c).
 * Every request must carry "Authorization: Bearer <token>"; the token is random per start and written to %LOCALAPPDATA%\AppleMusicWrapper\token (so only local programs
 * that can read the user's files get in, and web pages cannot drive it cross-site: there are
 * no CORS headers and the custom header forces a preflight that is refused).
 *
 *   GET  /status                              state + now playing
 *   POST /play?song=<id>&album=<id>[&storefront=<cc>]   play a catalogue song
 *   POST /play?album=<id>[&storefront=<cc>]             play a catalogue album
 *   POST /pause  /resume  /stop  /next  /previous
 *   POST /seek?ms=<position>
 *   POST /volume?v=<0..1>
 *   POST /quality?q=aac|lossless|hires
 *   POST /crossfade?on=0|1[&s=<seconds>]       POST /autoplay?on=0|1
 *   POST /enqueue?song=<id>&album=<id>[&storefront=<cc>][&at=next|end]   Play Next / Add to Queue
 *   POST /dequeue[?id=<queueItemId>]
 *   POST /quit                                shut the wrapper down
 */
#pragma once
#include <windows.h>
#include <stddef.h>

struct sreq
{
    struct sreq *next;
    char method[8], path[64], query[512];
    char body[8192];        /* response JSON */
    int status;
    HANDLE done;
    int taken, finished, abandoned;   /* hand-over between the socket thread and the main thread, under srv_cs */
};

/* runs on the main thread, fills r->status and r->body */
typedef void (*server_handler)(struct sreq *r);

/* start listening; returns 0 on failure. token_path receives where the token was written. */
int server_start(int port, server_handler handler, WCHAR *token_path, size_t token_path_len);
/* run queued requests; called from the message loop on the main thread */
void server_process(void);
/* value of a query parameter ("a=1&b=2"), or "" */
const char *srv_param(const struct sreq *r, const char *name, char *buf, size_t size);

/* building a JSON response in a fixed buffer */
struct json { char *b; size_t n, cap; };
void json_init(struct json *j, char *buf, size_t cap);
void json_raw(struct json *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void json_str(struct json *j, const char *utf8);     /* a quoted, escaped string literal */
