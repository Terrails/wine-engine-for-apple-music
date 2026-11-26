/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Local control API server (see server.h for the protocol). */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdarg.h>
#include "server.h"

static struct sreq *srv_head, *srv_tail;
static CRITICAL_SECTION srv_cs;
static CRITICAL_SECTION_DEBUG srv_cs_debug = { 0, 0, &srv_cs, { &srv_cs_debug.ProcessLocksList, &srv_cs_debug.ProcessLocksList }, 0, 0, { 0 } };
static CRITICAL_SECTION srv_cs = { &srv_cs_debug, -1, 0, 0, 0, 0 };
static char srv_token[65];
static int srv_busy;
static DWORD srv_main_tid;
static server_handler srv_handler;

const char *srv_param(const struct sreq *r, const char *name, char *buf, size_t size)
{
    size_t n = strlen(name);
    const char *p = r->query;
    buf[0] = 0;
    while (*p)
    {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if (!strncmp(p, name, n) && p[n] == '=')
        {
            size_t len = end - p - n - 1;
            if (len >= size) len = size - 1;
            memcpy(buf, p + n + 1, len);
            buf[len] = 0;
            break;
        }
        p = *end ? end + 1 : end;
    }
    return buf;
}

void json_init(struct json *j, char *buf, size_t cap) { j->b = buf; j->n = 0; j->cap = cap; buf[0] = 0; }

void json_raw(struct json *j, const char *fmt, ...)
{
    va_list ap; int n;
    if (j->n >= j->cap - 1) return;
    va_start(ap, fmt); n = vsnprintf(j->b + j->n, j->cap - j->n, fmt, ap); va_end(ap);
    if (n > 0) j->n = j->n + n < j->cap ? j->n + n : j->cap - 1;
}

void json_str(struct json *j, const char *s)
{
    json_raw(j, "\"");
    for (; *s && j->n + 8 < j->cap; s++)
    {
        unsigned char c = *s;
        if (c == '"' || c == '\\') json_raw(j, "\\%c", c);
        else if (c < 0x20) json_raw(j, "\\u%04x", c);
        else json_raw(j, "%c", c);
    }
    json_raw(j, "\"");
}

static void srv_send(SOCKET s, int status, const char *body)
{
    char hdr[256];
    const char *text = status == 200 ? "OK" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" :
                       status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict" : status == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", status, text, (unsigned)strlen(body));
    send(s, hdr, n, 0);
    send(s, body, strlen(body), 0);
}

#define SRV_MAX_CONN 16        /* connection threads at once; more wait in the listen backlog */
#define SRV_HEADER_MS 5000     /* the whole request head must arrive within this time */
#define SRV_WAIT_MS 60000      /* how long a request may wait for the main thread */
#define SRV_ORDER_MS 50        /* how long a request lets connections accepted before it get queued first */

static volatile LONG srv_conns;
static struct srv_conn { SOCKET s; DWORD seq; int used, reading; } srv_slots[SRV_MAX_CONN];   /* under srv_cs */
static int srv_status_ok, srv_nested;   /* main thread only: see server_process() */

/* take the first queued request off the list (nested: the first GET /status or POST /quit); call with srv_cs held */
static struct sreq *srv_take_locked(int nested)
{
    struct sreq *r, *prev = NULL;
    for (r = srv_head; r; prev = r, r = r->next)
        if (!nested || (!strcmp(r->method, "GET") && !strcmp(r->path, "/status"))
            || (!strcmp(r->method, "POST") && !strcmp(r->path, "/quit"))) break;
    if (!r) return NULL;
    if (prev) prev->next = r->next; else srv_head = r->next;
    if (srv_tail == r) srv_tail = prev;
    r->next = NULL;
    r->taken = 1;
    return r;
}

static void srv_free(struct sreq *r) { CloseHandle(r->done); free(r); }

/* the client closed its end while the request was waiting */
static int srv_client_gone(SOCKET s)
{
    fd_set fds; struct timeval tv = {0, 0}; char c;
    FD_ZERO(&fds); FD_SET(s, &fds);
    if (select(0, &fds, NULL, NULL, &tv) <= 0) return 0;
    return recv(s, &c, 1, MSG_PEEK) <= 0;
}

/* wait for the main thread to answer r; returns 1 when it has (r is then ours to send and free), 0 when
 * we gave up. After SRV_WAIT_MS a request that still waits is removed (the client is told it did not run).
 * A command whose client is gone still runs, like any request already running, and the main thread frees
 * it: a client that only fires a command must get it done (Sidra sends /quit and exits within 3 s). Clients that
 * closed their end are looked for only when connections run short: some just half-close and still read. */
static int srv_wait(SOCKET s, struct sreq *r)
{
    DWORD t0 = GetTickCount(); int done, gone = 0, ours = 0;
    while (!(done = WaitForSingleObject(r->done, 100) == WAIT_OBJECT_0))
        if (GetTickCount() - t0 >= SRV_WAIT_MS || (gone = srv_conns > SRV_MAX_CONN / 2 && srv_client_gone(s))) break;
    if (done) return 1;
    EnterCriticalSection(&srv_cs);
    if (r->finished) done = 1;
    else if (r->taken || (gone && strcmp(r->method, "GET"))) r->abandoned = 1;
    else
    {
        struct sreq **p = &srv_head, *prev = NULL;
        while (*p && *p != r) { prev = *p; p = &(*p)->next; }
        if (*p) { *p = r->next; if (srv_tail == r) srv_tail = prev; }
        ours = 1;
    }
    LeaveCriticalSection(&srv_cs);
    if (ours) srv_free(r);
    return done;
}

static void srv_reading_done(struct srv_conn *c)
{
    EnterCriticalSection(&srv_cs);
    c->reading = 0;
    LeaveCriticalSection(&srv_cs);
}

/* queue r. The connection threads run in any order, so requests that arrived together are put back in the
 * order their connections were accepted (a connection that has sent nothing yet holds nobody up for long) */
static void srv_queue(struct srv_conn *c, struct sreq *r)
{
    DWORD t0 = GetTickCount();
    EnterCriticalSection(&srv_cs);
    while (GetTickCount() - t0 < SRV_ORDER_MS)
    {
        int i;
        for (i = 0; i < SRV_MAX_CONN; i++)
            if (srv_slots[i].reading && (int)(srv_slots[i].seq - c->seq) < 0) break;
        if (i == SRV_MAX_CONN) break;
        LeaveCriticalSection(&srv_cs);
        Sleep(1);
        EnterCriticalSection(&srv_cs);
    }
    c->reading = 0;
    if (srv_tail) srv_tail->next = r; else srv_head = r;
    srv_tail = r;
    LeaveCriticalSection(&srv_cs);
    PostThreadMessageW(srv_main_tid, WM_NULL, 0, 0);     /* wake the message loop */
}

static void srv_connection(struct srv_conn *c)
{
    SOCKET s = c->s;
    char buf[4096], *line_end, *sp1, *sp2, *q, *auth;
    int len = 0, n;
    struct sreq *r;
    DWORD t0 = GetTickCount(), tmo;

    tmo = 5000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&tmo, sizeof(tmo));
    for (;;)
    {
        DWORD used = GetTickCount() - t0;
        if (len >= (int)sizeof(buf) - 1 || used >= SRV_HEADER_MS) break;
        tmo = SRV_HEADER_MS - used;                  /* a client trickling bytes still ends at the deadline */
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
        if ((n = recv(s, buf + len, sizeof(buf) - 1 - len, 0)) <= 0) break;
        len += n;
        buf[len] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[len] = 0;
    if (!(line_end = strstr(buf, "\r\n")) || !(sp1 = strchr(buf, ' ')) || !(sp2 = strchr(sp1 + 1, ' ')) || sp2 > line_end)
    { srv_send(s, 400, "{\"error\":\"bad request\"}"); return; }

    /* the token must match exactly */
    auth = strstr(line_end, "\r\nAuthorization: Bearer ");
    if (!auth) auth = strstr(line_end, "\r\nauthorization: Bearer ");
    if (!auth || strncmp(auth + 24, srv_token, 64) || (auth[24 + 64] != '\r' && auth[24 + 64] != ' '))
    { srv_send(s, 401, "{\"error\":\"missing or wrong token\"}"); return; }

    if (!(r = calloc(1, sizeof(*r))) || !(r->done = CreateEventW(NULL, TRUE, FALSE, NULL)))
    { free(r); srv_send(s, 503, "{\"error\":\"out of memory\"}"); return; }
    *sp1 = 0; *sp2 = 0;
    lstrcpynA(r->method, buf, sizeof(r->method));
    if ((q = strchr(sp1 + 1, '?'))) { *q = 0; lstrcpynA(r->query, q + 1, sizeof(r->query)); }
    lstrcpynA(r->path, sp1 + 1, sizeof(r->path));

    srv_queue(c, r);
    if (srv_wait(s, r))
    {
        srv_send(s, r->status, r->body);
        srv_free(r);
    }
    else srv_send(s, 503, "{\"error\":\"timed out waiting for the player thread\"}");   /* or the client is gone */
}

static DWORD WINAPI srv_conn_thread(void *arg)
{
    struct srv_conn *c = arg;
    SOCKET s = c->s;
    srv_connection(c);
    srv_reading_done(c);           /* a request that was refused before it was queued */
    shutdown(s, SD_SEND);
    closesocket(s);
    EnterCriticalSection(&srv_cs);
    c->used = 0;
    LeaveCriticalSection(&srv_cs);
    InterlockedDecrement(&srv_conns);
    return 0;
}

/* a free connection slot, or NULL */
static struct srv_conn *srv_slot(void)
{
    struct srv_conn *c = NULL;
    EnterCriticalSection(&srv_cs);
    for (int i = 0; i < SRV_MAX_CONN && !c; i++) if (!srv_slots[i].used) c = &srv_slots[i];
    LeaveCriticalSection(&srv_cs);
    return c;
}

/* With all slots taken nothing is accepted: clients wait in the listen backlog (refusing one here
 * would reset its connection, as its request has not been read, and lose a /quit, say) */
static DWORD WINAPI srv_thread(void *arg)
{
    SOCKET ls = (SOCKET)(ULONG_PTR)arg;
    for (DWORD seq = 0;; seq++)
    {
        HANDLE t; struct srv_conn *c; SOCKET s;
        while (!(c = srv_slot())) Sleep(10);
        if ((s = accept(ls, NULL, NULL)) == INVALID_SOCKET) { Sleep(50); continue; }
        EnterCriticalSection(&srv_cs);
        c->s = s; c->seq = seq; c->used = c->reading = 1;
        LeaveCriticalSection(&srv_cs);
        InterlockedIncrement(&srv_conns);
        if ((t = CreateThread(NULL, 0, srv_conn_thread, c, 0, NULL))) CloseHandle(t);
        else srv_conn_thread(c);           /* no thread to be had: served here, one at a time */
    }
    return 0;
}

/* The main thread runs the handler of one request at a time. While a /enqueue handler runs (and
 * pumps, which calls this again) queued GET /status and POST /quit requests are answered too, one
 * at a time and never from within another of them; every other request keeps its place in the queue
 * until the handler ends. A /quit only sets api_quit_requested: the /enqueue then stops waiting, and
 * the main loop stops playback once it has returned. During /play and the rest nothing is taken:
 * /status must not show the previous track while a /play is changing it. */
void server_process(void)
{
    for (;;)
    {
        struct sreq *r; int nested = srv_busy;
        if (nested && (!srv_status_ok || srv_nested)) return;
        EnterCriticalSection(&srv_cs);
        r = srv_take_locked(nested);
        LeaveCriticalSection(&srv_cs);
        if (!r) return;
        if (nested) srv_nested = 1;
        else { srv_busy = 1; srv_status_ok = !strcmp(r->method, "POST") && !strcmp(r->path, "/enqueue"); }
        srv_handler(r);
        if (nested) srv_nested = 0;
        else { srv_busy = 0; srv_status_ok = 0; }
        EnterCriticalSection(&srv_cs);
        if (r->abandoned) { LeaveCriticalSection(&srv_cs); srv_free(r); continue; }   /* nobody is waiting any more */
        r->finished = 1;
        SetEvent(r->done);
        LeaveCriticalSection(&srv_cs);
    }
}

int server_start(int port, server_handler handler, WCHAR *token_path, size_t token_path_len)
{
    WSADATA wsa;
    struct sockaddr_in addr = {0};
    SOCKET ls;
    BYTE rnd[32];
    WCHAR dir[MAX_PATH];
    HANDLE f;
    DWORD w;
    int i, yes = 1;

    if (BCryptGenRandom(NULL, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return 0;
    for (i = 0; i < 32; i++) sprintf(srv_token + 2 * i, "%02x", rnd[i]);

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir))) return 0;
    lstrcatW(dir, L"\\AppleMusicWrapper");
    CreateDirectoryW(dir, NULL);
    swprintf(token_path, token_path_len, L"%s\\token", dir);
    f = CreateFileW(token_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WriteFile(f, srv_token, 64, &w, NULL);
    CloseHandle(f);

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 0;
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);        /* loopback only */
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 16)) { closesocket(ls); return 0; }
    srv_main_tid = GetCurrentThreadId();
    srv_handler = handler;
    CloseHandle(CreateThread(NULL, 0, srv_thread, (void *)(ULONG_PTR)ls, 0, NULL));
    return 1;
}
