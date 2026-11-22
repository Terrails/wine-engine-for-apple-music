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
                       status == 405 ? "Method Not Allowed" : status == 503 ? "Service Unavailable" : "Error";
    int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", status, text, (unsigned)strlen(body));
    send(s, hdr, n, 0);
    send(s, body, strlen(body), 0);
}

static void srv_connection(SOCKET s)
{
    char buf[4096], *line_end, *sp1, *sp2, *q, *auth;
    int len = 0, n;
    struct sreq *r;
    DWORD tmo = 5000;

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&tmo, sizeof(tmo));
    while (len < (int)sizeof(buf) - 1 && (n = recv(s, buf + len, sizeof(buf) - 1 - len, 0)) > 0)
    {
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

    r = calloc(1, sizeof(*r));
    *sp1 = 0; *sp2 = 0;
    lstrcpynA(r->method, buf, sizeof(r->method));
    if ((q = strchr(sp1 + 1, '?'))) { *q = 0; lstrcpynA(r->query, q + 1, sizeof(r->query)); }
    lstrcpynA(r->path, sp1 + 1, sizeof(r->path));
    r->status = 503;
    strcpy(r->body, "{\"error\":\"timed out waiting for the player thread\"}");
    r->done = CreateEventW(NULL, TRUE, FALSE, NULL);

    EnterCriticalSection(&srv_cs);
    if (srv_tail) srv_tail->next = r; else srv_head = r;
    srv_tail = r;
    LeaveCriticalSection(&srv_cs);
    PostThreadMessageW(srv_main_tid, WM_NULL, 0, 0);     /* wake the message loop */

    if (WaitForSingleObject(r->done, 60000) == WAIT_OBJECT_0)
    {
        srv_send(s, r->status, r->body);
        CloseHandle(r->done);
        free(r);
    }
    else srv_send(s, 503, r->body);   /* the request stays queued and is freed by nobody: rare, small */
}

static DWORD WINAPI srv_thread(void *arg)
{
    SOCKET ls = (SOCKET)(ULONG_PTR)arg;
    for (;;)
    {
        SOCKET s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) { Sleep(50); continue; }
        srv_connection(s);
        shutdown(s, SD_SEND);
        closesocket(s);
    }
    return 0;
}

void server_process(void)
{
    if (srv_busy) return;            /* a handler is running and pumping messages itself */
    for (;;)
    {
        struct sreq *r;
        EnterCriticalSection(&srv_cs);
        if ((r = srv_head)) { srv_head = r->next; if (!srv_head) srv_tail = NULL; }
        LeaveCriticalSection(&srv_cs);
        if (!r) return;
        srv_busy = 1;
        srv_handler(r);
        srv_busy = 0;
        SetEvent(r->done);
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
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 8)) { closesocket(ls); return 0; }
    srv_main_tid = GetCurrentThreadId();
    srv_handler = handler;
    CloseHandle(CreateThread(NULL, 0, srv_thread, (void *)(ULONG_PTR)ls, 0, NULL));
    return 1;
}
