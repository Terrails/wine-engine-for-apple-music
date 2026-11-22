/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Web.Http: methods, headers, content and messages (overview: amshim_http.h). */
#include "amshim_http.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- HttpMethod ------------------------------------------------------- */

HRESULT http_method_create(const WCHAR *name, void **result)
{
    HRESULT hr = http_new_obj(L"Windows.Web.Http.HttpMethod", (IInspectable **)result);
    if (SUCCEEDED(hr)) http_put_s(impl_from_inspectable(*result), L"Method", name);
    return hr;
}

HRESULT OV_IHttpMethodStatics_get_Delete(struct obj *o, void **r) { return http_method_create(L"DELETE", r); }
HRESULT OV_IHttpMethodStatics_get_Get(struct obj *o, void **r) { return http_method_create(L"GET", r); }
HRESULT OV_IHttpMethodStatics_get_Head(struct obj *o, void **r) { return http_method_create(L"HEAD", r); }
HRESULT OV_IHttpMethodStatics_get_Options(struct obj *o, void **r) { return http_method_create(L"OPTIONS", r); }
HRESULT OV_IHttpMethodStatics_get_Patch(struct obj *o, void **r) { return http_method_create(L"PATCH", r); }
HRESULT OV_IHttpMethodStatics_get_Post(struct obj *o, void **r) { return http_method_create(L"POST", r); }
HRESULT OV_IHttpMethodStatics_get_Put(struct obj *o, void **r) { return http_method_create(L"PUT", r); }

/* ---- header collections ----------------------------------------------- */

HRESULT OV_IHttpRequestHeaderCollection_Append(struct obj *o, HSTRING name, HSTRING value)
{
    smap_append(o, WindowsGetStringRawBuffer(name, NULL), WindowsGetStringRawBuffer(value, NULL));
    return S_OK;
}

HRESULT OV_IHttpRequestHeaderCollection_TryAppendWithoutValidation(struct obj *o, HSTRING name, HSTRING value, boolean *result)
{
    *result = TRUE;
    return OV_IHttpRequestHeaderCollection_Append(o, name, value);
}

HRESULT OV_IHttpContentHeaderCollection_Append(struct obj *o, HSTRING name, HSTRING value)
{
    return OV_IHttpRequestHeaderCollection_Append(o, name, value);
}

HRESULT OV_IHttpContentHeaderCollection_TryAppendWithoutValidation(struct obj *o, HSTRING name, HSTRING value, boolean *result)
{
    *result = TRUE;
    return OV_IHttpRequestHeaderCollection_Append(o, name, value);
}

HRESULT OV_IHttpResponseHeaderCollection_Append(struct obj *o, HSTRING name, HSTRING value)
{
    return OV_IHttpRequestHeaderCollection_Append(o, name, value);
}

HRESULT OV_IHttpResponseHeaderCollection_TryAppendWithoutValidation(struct obj *o, HSTRING name, HSTRING value, boolean *result)
{
    *result = TRUE;
    return OV_IHttpRequestHeaderCollection_Append(o, name, value);
}

/* media type "type/sub; charset=x" <-> HttpMediaTypeHeaderValue */
static HRESULT media_type_parse(const WCHAR *s, void **result)
{
    WCHAR type[256], *p;
    const WCHAR *cs;
    HRESULT hr = http_new_obj(L"Windows.Web.Http.Headers.HttpMediaTypeHeaderValue", (IInspectable **)result);
    if (FAILED(hr)) return hr;
    lstrcpynW(type, s, ARRAY_SIZE(type));
    if ((p = wcschr(type, ';'))) *p = 0;
    while ((p = type + wcslen(type)) > type && p[-1] == ' ') p[-1] = 0;
    http_put_s(impl_from_inspectable(*result), L"MediaType", type);
    if ((cs = wcsstr(s, L"charset=")))
    {
        WCHAR charset[64];
        lstrcpynW(charset, cs + 8, ARRAY_SIZE(charset));
        if ((p = wcschr(charset, ';'))) *p = 0;
        if (charset[0] == '"') { memmove(charset, charset + 1, wcslen(charset) * sizeof(WCHAR)); if ((p = wcschr(charset, '"'))) *p = 0; }
        http_put_s(impl_from_inspectable(*result), L"CharSet", charset);
    }
    return S_OK;
}

HRESULT OV_IHttpMediaTypeHeaderValueStatics_Parse(struct obj *o, HSTRING input, void **result)
{
    return media_type_parse(WindowsGetStringRawBuffer(input, NULL), result);
}

HRESULT OV_IHttpMediaTypeHeaderValueStatics_TryParse(struct obj *o, HSTRING input, void **value, boolean *result)
{
    *result = SUCCEEDED(media_type_parse(WindowsGetStringRawBuffer(input, NULL), value));
    return S_OK;
}

static void media_type_string(IUnknown *mt, WCHAR *out, size_t len)
{
    HSTRING type = NULL, cs = NULL;
    IUnknown *m;
    out[0] = 0;
    if (FAILED(IUnknown_QueryInterface(mt, &IID_Windows_Web_Http_Headers_IHttpMediaTypeHeaderValue, (void **)&m))) return;
    VCALL(m, 6, HRESULT (WINAPI *)(IUnknown *, HSTRING *))(m, &cs);    /* get_CharSet */
    VCALL(m, 8, HRESULT (WINAPI *)(IUnknown *, HSTRING *))(m, &type);  /* get_MediaType */
    IUnknown_Release(m);
    if (WindowsGetStringLen(cs))
        swprintf(out, len, L"%s; charset=%s", WindowsGetStringRawBuffer(type, NULL), WindowsGetStringRawBuffer(cs, NULL));
    else
        swprintf(out, len, L"%s", WindowsGetStringRawBuffer(type, NULL));
    WindowsDeleteString(type);
    WindowsDeleteString(cs);
}

HRESULT OV_IHttpContentHeaderCollection_put_ContentType(struct obj *o, void *value)
{
    WCHAR buf[512];
    http_put_o(o, L"ContentType", value);
    if (value)
    {
        media_type_string(value, buf, ARRAY_SIZE(buf));
        smap_set(o, L"Content-Type", buf);
    }
    else smap_remove(o, L"Content-Type");
    return S_OK;
}

HRESULT OV_IHttpContentHeaderCollection_get_ContentType(struct obj *o, void **result)
{
    const WCHAR *s;
    if ((*result = http_get_o(o, L"ContentType"))) return S_OK;
    if (!(s = smap_get(o, L"Content-Type"))) return S_OK;
    return media_type_parse(s, result);
}

HRESULT OV_IHttpContentHeaderCollection_put_ContentLength(struct obj *o, void *value)
{
    return S_OK; /* computed from the body when sending */
}

/* ---- content ------------------------------------------------------------ */

static HRESULT init_headers(struct obj *o, const WCHAR *cls)
{
    IInspectable *h;
    HRESULT hr = http_new_obj(cls, &h);
    if (FAILED(hr)) return hr;
    http_put_o(o, L"Headers", h);
    IInspectable_Release(h);
    return S_OK;
}

struct obj *http_content_headers(struct obj *o)
{
    struct prop *p = prop_find(o, L"Headers");
    return p && p->v.unk ? impl_from_inspectable((IInspectable *)p->v.unk) : NULL;
}

HRESULT INIT_HttpBufferContent(struct obj *o) { return init_headers(o, L"Windows.Web.Http.Headers.HttpContentHeaderCollection"); }
HRESULT INIT_HttpStreamContent(struct obj *o) { return init_headers(o, L"Windows.Web.Http.Headers.HttpContentHeaderCollection"); }
HRESULT INIT_HttpStringContent(struct obj *o)
{
    HRESULT hr = init_headers(o, L"Windows.Web.Http.Headers.HttpContentHeaderCollection");
    if (SUCCEEDED(hr)) smap_set(http_content_headers(o), L"Content-Type", L"text/plain; charset=UTF-8");
    return hr;
}

static HRESULT own_body(struct obj *o, IInspectable **buf);

/* Windows fills in Content-Length when buffer and string content is created; AMP.Services asserts
 * it (`mImpl.Content().Headers().TryLookup(kContentLength)`) after setting a request body. */
static void set_content_length(struct obj *content)
{
    IInspectable *buf;
    BYTE *d;
    UINT32 len = 0;
    WCHAR num[16];
    if (FAILED(own_body(content, &buf))) return;
    buffer_bytes((IUnknown *)buf, &d, &len, NULL);
    IInspectable_Release(buf);
    swprintf(num, ARRAY_SIZE(num), L"%u", len);
    smap_set(http_content_headers(content), L"Content-Length", num);
}

static HRESULT string_content_create(HSTRING content, IInspectable **result)
{
    HRESULT hr = http_new_obj(L"Windows.Web.Http.HttpStringContent", result);
    if (FAILED(hr)) return hr;
    prop_put(impl_from_inspectable(*result), L"Content", PK_STRING, &content, sizeof(content));
    set_content_length(impl_from_inspectable(*result));
    return S_OK;
}

HRESULT OV_IHttpStringContentFactory_CreateFromString(struct obj *o, HSTRING content, void **result)
{
    return string_content_create(content, (IInspectable **)result);
}

HRESULT OV_IHttpStringContentFactory_CreateFromStringWithEncoding(struct obj *o, HSTRING content, INT32 encoding,
        void **result)
{
    return string_content_create(content, (IInspectable **)result);
}

HRESULT OV_IHttpStringContentFactory_CreateFromStringWithEncodingAndMediaType(struct obj *o, HSTRING content,
        INT32 encoding, HSTRING media, void **result)
{
    HRESULT hr = string_content_create(content, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    if (media)
    {
        WCHAR buf[512];
        swprintf(buf, ARRAY_SIZE(buf), L"%s; charset=UTF-8", WindowsGetStringRawBuffer(media, NULL));
        smap_set(http_content_headers(impl_from_inspectable(*result)), L"Content-Type", buf);
    }
    return S_OK;
}

HRESULT OV_IHttpBufferContentFactory_CreateFromBufferWithOffset(struct obj *o, void *content, UINT32 offset,
        UINT32 count, void **result)
{
    BYTE *data;
    UINT32 len;
    IInspectable *copy;
    HRESULT hr = buffer_bytes(content, &data, &len, NULL);
    if (FAILED(hr)) return hr;
    if (offset > len) offset = len;
    if (count > len - offset) count = len - offset;
    if (FAILED(hr = byte_buffer_create(data + offset, count, count, &copy))) return hr;
    hr = http_new_obj(L"Windows.Web.Http.HttpBufferContent", (IInspectable **)result);
    if (SUCCEEDED(hr))
    {
        http_put_o(impl_from_inspectable(*result), L"Content", copy);
        set_content_length(impl_from_inspectable(*result));
    }
    IInspectable_Release(copy);
    return hr;
}

HRESULT OV_IHttpBufferContentFactory_CreateFromBuffer(struct obj *o, void *content, void **result)
{
    return OV_IHttpBufferContentFactory_CreateFromBufferWithOffset(o, content, 0, ~0u, result);
}

/* Produce the body of one of our content objects as a buffer (ref returned). */
static HRESULT own_body(struct obj *o, IInspectable **buf)
{
    struct prop *p;
    *buf = http_get_o(o, L"Body");
    if (*buf) return S_OK;
    p = prop_find(o, L"Content");
    if (p && p->kind == PK_STRING)
    {
        const WCHAR *s = WindowsGetStringRawBuffer(p->v.str, NULL);
        int n = WideCharToMultiByte(CP_UTF8, 0, s, wcslen(s), NULL, 0, NULL, NULL);
        char *u = malloc(n + 1);
        HRESULT hr;
        WideCharToMultiByte(CP_UTF8, 0, s, wcslen(s), u, n, NULL, NULL);
        hr = byte_buffer_create(u, n, n, buf);
        free(u);
        return hr;
    }
    if (p && p->kind == PK_OBJECT && p->v.unk)
    {
        IUnknown *ib, *is;
        if (SUCCEEDED(IUnknown_QueryInterface(p->v.unk, &IID_Windows_Storage_Streams_IBuffer, (void **)&ib)))
        {
            *buf = (IInspectable *)ib;
            return S_OK;
        }
        if (SUCCEEDED(IUnknown_QueryInterface(p->v.unk, &IID_Windows_Storage_Streams_IInputStream, (void **)&is)))
        {
            /* drain the input stream */
            BYTE *all = NULL;
            UINT32 total = 0;
            HRESULT hr = S_OK;
            for (;;)
            {
                IInspectable *chunk, *op, *res = NULL;
                BYTE *d; UINT32 l;
                if (FAILED(hr = byte_buffer_create(NULL, 0, 65536, &chunk))) break;
                /* IInputStream::ReadAsync (slot 6), options Partial=1 */
                hr = VCALL(is, 6, HRESULT (WINAPI *)(IUnknown *, IInspectable *, UINT32, UINT32, IInspectable **))(is, chunk, 65536, 1, &op);
                if (SUCCEEDED(hr)) hr = async_wait((IUnknown *)op);
                /* IAsyncOperationWithProgress::GetResults (slot 10) */
                if (SUCCEEDED(hr)) hr = VCALL(op, 10, HRESULT (WINAPI *)(IInspectable *, IInspectable **))(op, &res);
                if (op) IInspectable_Release(op);
                l = 0;
                if (SUCCEEDED(hr) && res) buffer_bytes((IUnknown *)res, &d, &l, NULL);
                if (l) { all = realloc(all, total + l); memcpy(all + total, d, l); total += l; }
                if (res) IInspectable_Release(res);
                IInspectable_Release(chunk);
                if (FAILED(hr) || !l) break;
            }
            IUnknown_Release(is);
            if (SUCCEEDED(hr)) hr = byte_buffer_create(all, total, total, buf);
            free(all);
            if (SUCCEEDED(hr)) http_put_o(o, L"Body", *buf);
            return hr;
        }
    }
    return byte_buffer_create(NULL, 0, 0, buf);
}

HRESULT OV_IHttpContent_ReadAsBufferAsync(struct obj *o, void **result)
{
    IInspectable *buf;
    HRESULT hr = own_body(o, &buf);
    if (FAILED(hr)) return hr;
    hr = async_create(&ASYNC_IAsyncOperationWithProgress_IBuffer_UInt64_, ASYNC_OP_PROGRESS, S_OK, PK_OBJECT,
                      &buf, sizeof(buf), result);
    IInspectable_Release(buf);
    return hr;
}

HRESULT OV_IHttpContent_ReadAsStringAsync(struct obj *o, void **result)
{
    IInspectable *buf;
    BYTE *data;
    UINT32 len;
    WCHAR *w;
    HSTRING s;
    int n, skip = 0;
    HRESULT hr = own_body(o, &buf);
    if (FAILED(hr)) return hr;
    buffer_bytes((IUnknown *)buf, &data, &len, NULL);
    if (len >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) skip = 3;
    n = MultiByteToWideChar(CP_UTF8, 0, (char *)data + skip, len - skip, NULL, 0);
    w = malloc((n + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, (char *)data + skip, len - skip, w, n);
    WindowsCreateString(w, n, &s);
    free(w);
    IInspectable_Release(buf);
    hr = async_create(&ASYNC_IAsyncOperationWithProgress_String_UInt64_, ASYNC_OP_PROGRESS, S_OK, PK_STRING,
                      &s, sizeof(s), result);
    WindowsDeleteString(s);
    return hr;
}

HRESULT OV_IHttpContent_ReadAsInputStreamAsync(struct obj *o, void **result)
{
    IInspectable *buf, *stream;
    HRESULT hr = own_body(o, &buf);
    if (FAILED(hr)) return hr;
    hr = buffer_input_stream_create(buf, &stream);
    IInspectable_Release(buf);
    if (FAILED(hr)) return hr;
    hr = async_create(&ASYNC_IAsyncOperationWithProgress_IInputStream_UInt64_, ASYNC_OP_PROGRESS, S_OK, PK_OBJECT,
                      &stream, sizeof(stream), result);
    IInspectable_Release(stream);
    return hr;
}

HRESULT OV_IHttpContent_BufferAllAsync(struct obj *o, void **result)
{
    IInspectable *buf;
    BYTE *d;
    UINT32 len;
    UINT64 len64;
    HRESULT hr = own_body(o, &buf);
    if (FAILED(hr)) return hr;
    buffer_bytes((IUnknown *)buf, &d, &len, NULL);
    IInspectable_Release(buf);
    len64 = len;
    return async_create(&ASYNC_IAsyncOperationWithProgress_UInt64_UInt64_, ASYNC_OP_PROGRESS, S_OK, PK_PRIM,
                        &len64, sizeof(len64), result);
}

HRESULT OV_IHttpContent_TryComputeLength(struct obj *o, UINT64 *length, boolean *result)
{
    IInspectable *buf;
    BYTE *d;
    UINT32 len = 0;
    if (SUCCEEDED(own_body(o, &buf)))
    {
        buffer_bytes((IUnknown *)buf, &d, &len, NULL);
        IInspectable_Release(buf);
    }
    *length = len;
    *result = TRUE;
    return S_OK;
}

/* Body of any IHttpContent (ours or foreign) */
HRESULT http_content_body(IUnknown *content, IInspectable **buf)
{
    IUnknown *c;
    IInspectable *op, *res = NULL;
    HRESULT hr;

    *buf = NULL;
    if (!content) return S_OK;
    if (FAILED(hr = IUnknown_QueryInterface(content, &IID_Windows_Web_Http_IHttpContent, (void **)&c))) return hr;
    hr = VCALL(c, 8, HRESULT (WINAPI *)(IUnknown *, IInspectable **))(c, &op);   /* ReadAsBufferAsync */
    IUnknown_Release(c);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = async_wait((IUnknown *)op)))
        hr = VCALL(op, 10, HRESULT (WINAPI *)(IInspectable *, IInspectable **))(op, &res); /* GetResults */
    IInspectable_Release(op);
    *buf = res;
    return hr;
}

/* ---- request / response messages ----------------------------------------- */

HRESULT INIT_HttpRequestMessage(struct obj *o)
{
    IInspectable *props, *map;
    init_headers(o, L"Windows.Web.Http.Headers.HttpRequestHeaderCollection");
    if (SUCCEEDED(http_new_obj(L"Windows.Storage.ApplicationDataContainerSettings", &props)))
    {
        if (SUCCEEDED(IInspectable_QueryInterface(props, &IID_Windows_Foundation_Collections_IMap__String__Object__, (void **)&map)))
        {
            http_put_o(o, L"Properties", map);
            IInspectable_Release(map);
        }
        IInspectable_Release(props);
    }
    return S_OK;
}

HRESULT OV_IHttpRequestMessageFactory_Create(struct obj *o, void *method, void *uri, void **result)
{
    HRESULT hr = http_new_obj(L"Windows.Web.Http.HttpRequestMessage", (IInspectable **)result);
    if (FAILED(hr)) return hr;
    http_put_o(impl_from_inspectable(*result), L"Method", method);
    http_put_o(impl_from_inspectable(*result), L"RequestUri", uri);
    return S_OK;
}

HRESULT INIT_HttpResponseMessage(struct obj *o)
{
    INT32 status = 200;
    prop_put(o, L"StatusCode", PK_PRIM, &status, sizeof(status));
    return init_headers(o, L"Windows.Web.Http.Headers.HttpResponseHeaderCollection");
}

HRESULT OV_IHttpResponseMessage_get_IsSuccessStatusCode(struct obj *o, boolean *result)
{
    INT32 status = 0;
    prop_get(o, L"StatusCode", PK_PRIM, &status, sizeof(status), NULL);
    *result = status >= 200 && status <= 299;
    return S_OK;
}

HRESULT OV_IHttpResponseMessage_EnsureSuccessStatusCode(struct obj *o, void **result)
{
    INT32 status = 0;
    prop_get(o, L"StatusCode", PK_PRIM, &status, sizeof(status), NULL);
    if (status < 200 || status > 299) { *result = NULL; return MAKE_HRESULT(1, 0x19, status); }
    *result = &o->ifcs[0];
    IInspectable_AddRef((IInspectable *)*result);
    return S_OK;
}
