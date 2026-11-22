/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Web.Http: the network part, on WinHTTP (overview: amshim_http.h). */
#include "amshim_http.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- the network part -------------------------------------------------- */

static HINTERNET session;

/* ---- gzip: Wine's WinHTTP leaves some "Content-Encoding: gzip" bodies compressed even with
 * WINHTTP_OPTION_DECOMPRESSION, while Windows.Web.Http always hands out decompressed content.
 * Inflate those here with the zlib1.dll that ships with the app. ---- */
struct zstream   /* z_stream for 64-bit Windows (uLong is 32 bits) */
{
    const BYTE *next_in; UINT avail_in; ULONG total_in;
    BYTE *next_out; UINT avail_out; ULONG total_out;
    char *msg; void *state; void *zalloc, *zfree, *opaque;
    int data_type; ULONG adler, reserved;
};

static BOOL gunzip(BYTE **data, DWORD *len)
{
    static int (*p_inflateInit2_)(struct zstream *, int, const char *, int);
    static int (*p_inflate)(struct zstream *, int);
    static int (*p_inflateEnd)(struct zstream *);
    static const char *(*p_zlibVersion)(void);
    struct zstream z = {0};
    BYTE *out = NULL;
    DWORD cap = *len * 4 + 4096;
    int ret;

    if (!p_inflate)
    {
        HMODULE zl = LoadLibraryW(L"zlib1.dll");
        if (!zl) { WARN("zlib1.dll not found\n"); return FALSE; }
        p_inflateInit2_ = (void *)GetProcAddress(zl, "inflateInit2_");
        p_inflateEnd = (void *)GetProcAddress(zl, "inflateEnd");
        p_zlibVersion = (void *)GetProcAddress(zl, "zlibVersion");
        p_inflate = (void *)GetProcAddress(zl, "inflate");
        if (!p_inflateInit2_ || !p_inflateEnd || !p_zlibVersion || !p_inflate) { p_inflate = NULL; return FALSE; }
    }
    z.next_in = *data; z.avail_in = *len;
    if (p_inflateInit2_(&z, 15 + 32 /* zlib or gzip header */, p_zlibVersion(), sizeof(z))) return FALSE;
    out = malloc(cap);
    for (;;)
    {
        z.next_out = out + z.total_out; z.avail_out = cap - z.total_out;
        ret = p_inflate(&z, 0 /* Z_NO_FLUSH */);
        if (ret == 1 /* Z_STREAM_END */) break;
        if (ret != 0 || (!z.avail_in && z.avail_out)) { p_inflateEnd(&z); free(out); return FALSE; }
        if (!z.avail_out) { cap *= 2; out = realloc(out, cap); }
    }
    p_inflateEnd(&z);
    TRACE("inflated %lu -> %lu bytes\n", *len, z.total_out);
    free(*data);
    *data = out; *len = z.total_out;
    return TRUE;
}

static HINTERNET get_session(void)
{
    if (!session)
    {
        HINTERNET s = WinHttpOpen(L"AppleMusic", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        DWORD opt = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(s, WINHTTP_OPTION_DECOMPRESSION, &opt, sizeof(opt));
        if (InterlockedCompareExchangePointer(&session, s, NULL)) WinHttpCloseHandle(s);
    }
    return session;
}

static void add_header_lines(struct obj *headers, WCHAR **buf, size_t *len, size_t *cap, BOOL skip_length)
{
    UINT32 i;
    if (!headers) return;
    for (i = 0; i < smap_count(headers); i++)
    {
        const WCHAR *k, *v;
        size_t need;
        smap_at(headers, i, &k, &v);
        if (skip_length && !wcsicmp(k, L"Content-Length")) continue;
        need = wcslen(k) + wcslen(v) + 5;
        if (*len + need >= *cap) { *cap = (*cap + need) * 2; *buf = realloc(*buf, *cap * sizeof(WCHAR)); }
        *len += swprintf(*buf + *len, *cap - *len, L"%s: %s\r\n", k, v);
    }
}

static BOOL is_content_header(const WCHAR *name)
{
    static const WCHAR *names[] = { L"Content-Type", L"Content-Length", L"Content-Encoding", L"Content-Language",
        L"Content-Location", L"Content-MD5", L"Content-Range", L"Content-Disposition", L"Expires", L"Last-Modified",
        L"Allow" };
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(names); i++) if (!wcsicmp(name, names[i])) return TRUE;
    return FALSE;
}

/* perform the request synchronously; returns a new HttpResponseMessage */
HRESULT http_perform(IInspectable *request, IInspectable **response)
{
    struct obj *req = impl_from_inspectable(request);
    IInspectable *uri = http_get_o(req, L"RequestUri"), *method = http_get_o(req, L"Method"), *content = http_get_o(req, L"Content");
    IInspectable *body = NULL, *resp = NULL, *rcontent = NULL, *rbuf = NULL;
    HSTRING huri = NULL, hmethod = NULL;
    URL_COMPONENTS uc = { sizeof(uc) };
    WCHAR host[256], *path = NULL, *hdrs = NULL, *raw = NULL;
    size_t hlen = 0, hcap = 0;
    HINTERNET conn = NULL, hreq = NULL;
    BYTE *data = NULL, *rdata = NULL;
    UINT32 dlen = 0;
    DWORD status = 0, size, avail, read, total = 0;
    HRESULT hr = S_OK;

    if (!uri) { hr = E_INVALIDARG; goto done; }
    if (FAILED(hr = http_uri_string((IUnknown *)uri, &huri))) goto done;
    http_method_string((IUnknown *)method, &hmethod);
    TRACE("%s %s\n", debugstr_hstring(hmethod), debugstr_hstring(huri));

    uc.lpszHostName = host; uc.dwHostNameLength = ARRAY_SIZE(host);
    uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(WindowsGetStringRawBuffer(huri, NULL), 0, 0, &uc)) { hr = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    path = malloc((uc.dwUrlPathLength + uc.dwExtraInfoLength + 2) * sizeof(WCHAR));
    memcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength * sizeof(WCHAR));
    memcpy(path + uc.dwUrlPathLength, uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(WCHAR));
    path[uc.dwUrlPathLength + uc.dwExtraInfoLength] = 0;
    if (!path[0]) wcscpy(path, L"/");

    if (content && SUCCEEDED(http_content_body((IUnknown *)content, &body)) && body)
        buffer_bytes((IUnknown *)body, &data, &dlen, NULL);

    {
        struct prop *hp = prop_find(req, L"Headers");
        add_header_lines(hp && hp->v.unk ? impl_from_inspectable((IInspectable *)hp->v.unk) : NULL, &hdrs, &hlen, &hcap, FALSE);
    }
    if (content)
    {
        struct obj *co = http_ours((IUnknown *)content, L"Windows.Web.Http.HttpBufferContent");
        if (!co) co = http_ours((IUnknown *)content, L"Windows.Web.Http.HttpStringContent");
        if (!co) co = http_ours((IUnknown *)content, L"Windows.Web.Http.HttpStreamContent");
        if (co) add_header_lines(http_content_headers(co), &hdrs, &hlen, &hcap, TRUE);
    }

    if (!(conn = WinHttpConnect(get_session(), host, uc.nPort, 0)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    /* Wine's WinHTTP reuses a cached keep-alive connection without noticing that the server has
     * closed it: the reply is empty and the request fails with ERROR_WINHTTP_INVALID_SERVER_RESPONSE
     * (seen on amp-api.music.apple.com). Windows sends the request again on a new connection; so do
     * we, once (the failed connection is not cached again). */
    for (int attempt = 0; ; attempt++)
    {
        DWORD err;
        if (!(hreq = WinHttpOpenRequest(conn, WindowsGetStringRawBuffer(hmethod, NULL), path, NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto done;
        }
        if (WinHttpSendRequest(hreq, hdrs ? hdrs : WINHTTP_NO_ADDITIONAL_HEADERS, hdrs ? (DWORD)-1 : 0,
                               data, dlen, dlen, 0) &&
            WinHttpReceiveResponse(hreq, NULL))
            break;
        err = GetLastError();
        hr = HRESULT_FROM_WIN32(err);
        WinHttpCloseHandle(hreq);
        hreq = NULL;
        if (attempt || (err != ERROR_WINHTTP_INVALID_SERVER_RESPONSE && err != ERROR_WINHTTP_CONNECTION_ERROR))
        {
            WARN("request failed %#lx\n", hr);
            goto done;
        }
        TRACE("request failed %#lx, retrying on a new connection\n", hr);
        hr = S_OK;
    }

    size = sizeof(status);
    WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL);
    while (WinHttpQueryDataAvailable(hreq, &avail) && avail)
    {
        rdata = realloc(rdata, total + avail);
        if (!WinHttpReadData(hreq, rdata + total, avail, &read) || !read) break;
        total += read;
    }
    TRACE("-> %lu, %lu bytes\n", status, total);
    if (total > 2 && rdata[0] == 0x1f && rdata[1] == 0x8b)
    {
        /* Still gzip. Windows.Web.Http would have delivered it decompressed (automatic
         * decompression is on by default); Wine's WinHTTP sometimes neither inflates the body
         * nor keeps the Content-Encoding header, so go by the gzip signature. */
        WCHAR enc[64] = L"", type[128] = L"";
        size = sizeof(enc);
        WinHttpQueryHeaders(hreq, WINHTTP_QUERY_CONTENT_ENCODING, NULL, enc, &size, NULL);
        size = sizeof(type);
        WinHttpQueryHeaders(hreq, WINHTTP_QUERY_CONTENT_TYPE, NULL, type, &size, NULL);
        TRACE("gzip body, Content-Encoding %s, Content-Type %s\n", debugstr_w(enc), debugstr_w(type));
        gunzip(&rdata, &total);
    }
    {
        /* debugging aid: AMSHIM_HTTP_DUMP=<dir> saves response bodies */
        char dir[MAX_PATH];
        if (GetEnvironmentVariableA("AMSHIM_HTTP_DUMP", dir, sizeof(dir)))
        {
            static LONG seq;
            char name[MAX_PATH + 32];
            HANDLE f;
            DWORD w;
            sprintf(name, "%s\\%03ld.bin", dir, InterlockedIncrement(&seq));
            f = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) { WriteFile(f, rdata, total, &w, NULL); CloseHandle(f); }
            TRACE("dumped %s\n", name);
        }
    }

    if (FAILED(hr = http_new_obj(L"Windows.Web.Http.HttpResponseMessage", &resp))) goto done;
    {
        struct obj *ro = impl_from_inspectable(resp), *rh, *ch;
        INT32 st = status, version = 2 /* Http11 */, source = 2 /* Network */;
        WCHAR text[256] = L"";
        WCHAR *line, *next;
        prop_put(ro, L"StatusCode", PK_PRIM, &st, sizeof(st));
        prop_put(ro, L"Version", PK_PRIM, &version, sizeof(version));
        prop_put(ro, L"Source", PK_PRIM, &source, sizeof(source));
        size = sizeof(text);
        WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_TEXT, NULL, text, &size, NULL);
        http_put_s(ro, L"ReasonPhrase", text);
        http_put_o(ro, L"RequestMessage", request);

        byte_buffer_create(rdata, total, total, &rbuf);
        http_new_obj(L"Windows.Web.Http.HttpBufferContent", &rcontent);
        http_put_o(impl_from_inspectable(rcontent), L"Content", rbuf);
        http_put_o(ro, L"Content", rcontent);
        rh = http_content_headers(ro);
        ch = http_content_headers(impl_from_inspectable(rcontent));

        size = 0;
        WinHttpQueryHeaders(hreq, WINHTTP_QUERY_RAW_HEADERS_CRLF, NULL, NULL, &size, NULL);
        raw = malloc(size + sizeof(WCHAR));
        if (WinHttpQueryHeaders(hreq, WINHTTP_QUERY_RAW_HEADERS_CRLF, NULL, raw, &size, NULL))
        {
            line = wcschr(raw, '\n'); /* skip status line */
            for (line = line ? line + 1 : raw; line && *line; line = next)
            {
                WCHAR *colon, *val, *end;
                if ((next = wcschr(line, '\n'))) *next++ = 0;
                if ((end = wcschr(line, '\r'))) *end = 0;
                if (!(colon = wcschr(line, ':'))) continue;
                *colon = 0;
                for (val = colon + 1; *val == ' '; val++);
                if (is_content_header(line))
                {
                    /* body was decompressed by WinHTTP: drop encoding/length */
                    if (!wcsicmp(line, L"Content-Encoding") || !wcsicmp(line, L"Content-Length")) continue;
                    smap_append(ch, line, val);
                }
                else smap_append(rh, line, val);
            }
        }
        {
            WCHAR len[32];
            swprintf(len, ARRAY_SIZE(len), L"%lu", total);
            smap_set(ch, L"Content-Length", len);
        }
    }
    *response = resp;
    resp = NULL;

done:
    if (hreq) WinHttpCloseHandle(hreq);
    if (conn) WinHttpCloseHandle(conn);
    free(path); free(hdrs); free(raw); free(rdata);
    WindowsDeleteString(huri); WindowsDeleteString(hmethod);
    if (uri) IInspectable_Release(uri);
    if (method) IInspectable_Release(method);
    if (content) IInspectable_Release(content);
    if (body) IInspectable_Release(body);
    if (rbuf) IInspectable_Release(rbuf);
    if (rcontent) IInspectable_Release(rcontent);
    if (resp) IInspectable_Release(resp);
    return hr;
}

struct job { IInspectable *request; struct obj *op; };

static DWORD WINAPI send_worker(void *arg)
{
    struct job *j = arg;
    IInspectable *resp = NULL;
    HRESULT hr = http_perform(j->request, &resp);
    async_complete(j->op, hr, PK_OBJECT, resp ? &resp : NULL, sizeof(resp));
    if (resp) IInspectable_Release(resp);
    IInspectable_Release(j->request);
    free(j);
    return 0;
}

/* HttpBaseProtocolFilter : IHttpFilter */
HRESULT OV_IHttpFilter_SendRequestAsync(struct obj *o, void *request, void **result)
{
    struct job *j;
    HRESULT hr;
    struct obj *req = http_ours(request, L"Windows.Web.Http.HttpRequestMessage");

    if (!req) { FIXME("foreign request message\n"); return E_NOTIMPL; }
    j = calloc(1, sizeof(*j));
    j->request = (IInspectable *)&req->ifcs[0];
    IInspectable_AddRef(j->request);
    hr = async_create_pending(&ASYNC_IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_, ASYNC_OP_PROGRESS,
                              &j->op, result);
    if (FAILED(hr)) { IInspectable_Release(j->request); free(j); return hr; }
    if (!QueueUserWorkItem(send_worker, j, WT_EXECUTELONGFUNCTION)) { send_worker(j); }
    return S_OK;
}
