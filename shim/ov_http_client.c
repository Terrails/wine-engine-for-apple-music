/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Web.Http: HttpClient (overview: amshim_http.h). */
#include "amshim_http.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- HttpClient ------------------------------------------------------- */

HRESULT INIT_HttpClient(struct obj *o)
{
    IInspectable *h;
    if (SUCCEEDED(http_new_obj(L"Windows.Web.Http.Headers.HttpRequestHeaderCollection", &h)))
    {
        http_put_o(o, L"DefaultRequestHeaders", h);
        IInspectable_Release(h);
    }
    return S_OK;
}

static HRESULT client_send(struct obj *o, IInspectable *request, void **result)
{
    IInspectable *filter = http_get_o(o, L"Filter"), *dh = http_get_o(o, L"DefaultRequestHeaders");
    struct obj *req = http_ours((IUnknown *)request, L"Windows.Web.Http.HttpRequestMessage");
    IUnknown *f;
    HRESULT hr;

    /* merge default headers */
    if (req && dh)
    {
        struct prop *hp = prop_find(req, L"Headers");
        struct obj *rh = hp && hp->v.unk ? impl_from_inspectable((IInspectable *)hp->v.unk) : NULL;
        struct obj *d = impl_from_inspectable(dh);
        UINT32 i;
        for (i = 0; rh && i < smap_count(d); i++)
        {
            const WCHAR *k, *v;
            smap_at(d, i, &k, &v);
            if (!smap_get(rh, k)) smap_set(rh, k, v);
        }
    }
    if (dh) IInspectable_Release(dh);
    if (!filter)
    {
        if (FAILED(hr = http_new_obj(L"Windows.Web.Http.Filters.HttpBaseProtocolFilter", &filter))) return hr;
        http_put_o(o, L"Filter", filter);
    }
    hr = IInspectable_QueryInterface(filter, &IID_Windows_Web_Http_Filters_IHttpFilter, (void **)&f);
    IInspectable_Release(filter);
    if (FAILED(hr)) return hr;
    hr = VCALL(f, 6, HRESULT (WINAPI *)(IUnknown *, IInspectable *, void **))(f, request, result);
    IUnknown_Release(f);
    return hr;
}

static HRESULT make_request(const WCHAR *method, void *uri, void *content, IInspectable **out)
{
    IInspectable *m;
    HRESULT hr = http_method_create(method, (void **)&m);
    if (FAILED(hr)) return hr;
    hr = http_new_obj(L"Windows.Web.Http.HttpRequestMessage", out);
    if (SUCCEEDED(hr))
    {
        http_put_o(impl_from_inspectable(*out), L"Method", m);
        http_put_o(impl_from_inspectable(*out), L"RequestUri", uri);
        if (content) http_put_o(impl_from_inspectable(*out), L"Content", content);
    }
    IInspectable_Release(m);
    return hr;
}

static HRESULT client_simple(struct obj *o, const WCHAR *method, void *uri, void *content, void **result)
{
    IInspectable *req;
    HRESULT hr = make_request(method, uri, content, &req);
    if (FAILED(hr)) return hr;
    hr = client_send(o, req, result);
    IInspectable_Release(req);
    return hr;
}

HRESULT OV_IHttpClient_SendRequestAsync(struct obj *o, void *request, void **result) { return client_send(o, request, result); }
HRESULT OV_IHttpClient_SendRequestAsync_2(struct obj *o, void *request, INT32 opt, void **result) { return client_send(o, request, result); }
HRESULT OV_IHttpClient_GetAsync(struct obj *o, void *uri, void **result) { return client_simple(o, L"GET", uri, NULL, result); }
HRESULT OV_IHttpClient_GetAsync_2(struct obj *o, void *uri, INT32 opt, void **result) { return client_simple(o, L"GET", uri, NULL, result); }
HRESULT OV_IHttpClient_DeleteAsync(struct obj *o, void *uri, void **result) { return client_simple(o, L"DELETE", uri, NULL, result); }
HRESULT OV_IHttpClient_PostAsync(struct obj *o, void *uri, void *content, void **result) { return client_simple(o, L"POST", uri, content, result); }
HRESULT OV_IHttpClient_PutAsync(struct obj *o, void *uri, void *content, void **result) { return client_simple(o, L"PUT", uri, content, result); }

/* GetStringAsync / GetBufferAsync / GetInputStreamAsync */
struct get_job { struct obj *client; IInspectable *uri; struct obj *op; int kind; };

static DWORD WINAPI get_worker(void *arg)
{
    struct get_job *j = arg;
    IInspectable *inner = NULL, *resp = NULL, *content = NULL, *res_op = NULL, *value = NULL;
    HSTRING str = NULL;
    HRESULT hr = client_simple(j->client, L"GET", j->uri, NULL, (void **)&inner);

    if (SUCCEEDED(hr)) hr = async_wait((IUnknown *)inner);
    if (SUCCEEDED(hr)) hr = VCALL(inner, 10, HRESULT (WINAPI *)(IInspectable *, IInspectable **))(inner, &resp);
    if (SUCCEEDED(hr))
    {
        struct obj *ro = impl_from_inspectable(resp);
        IInspectable *tmp;
        hr = OV_IHttpResponseMessage_EnsureSuccessStatusCode(ro, (void **)&tmp);
        if (SUCCEEDED(hr)) IInspectable_Release(tmp);
        content = http_get_o(ro, L"Content");
    }
    if (SUCCEEDED(hr) && content)
    {
        struct obj *co = impl_from_inspectable(content);
        if (j->kind == 0) hr = OV_IHttpContent_ReadAsStringAsync(co, (void **)&res_op);
        else if (j->kind == 1) hr = OV_IHttpContent_ReadAsBufferAsync(co, (void **)&res_op);
        else hr = OV_IHttpContent_ReadAsInputStreamAsync(co, (void **)&res_op);
        if (SUCCEEDED(hr))
        {
            struct prop *p = prop_find(impl_from_inspectable(res_op), L"Result");
            if (j->kind == 0) WindowsDuplicateString(p->v.str, &str);
            else { value = (IInspectable *)p->v.unk; IInspectable_AddRef(value); }
        }
    }
    if (j->kind == 0) async_complete(j->op, hr, PK_STRING, SUCCEEDED(hr) ? &str : NULL, sizeof(str));
    else async_complete(j->op, hr, PK_OBJECT, SUCCEEDED(hr) ? &value : NULL, sizeof(value));
    WindowsDeleteString(str);
    if (value) IInspectable_Release(value);
    if (res_op) IInspectable_Release(res_op);
    if (content) IInspectable_Release(content);
    if (resp) IInspectable_Release(resp);
    if (inner) IInspectable_Release(inner);
    IInspectable_Release(j->uri);
    IInspectable_Release((IInspectable *)&j->client->ifcs[0]);
    free(j);
    return 0;
}

static HRESULT client_get(struct obj *o, void *uri, int kind, const struct iface_entry *entry, void **result)
{
    struct get_job *j = calloc(1, sizeof(*j));
    HRESULT hr = async_create_pending(entry, ASYNC_OP_PROGRESS, &j->op, result);
    if (FAILED(hr)) { free(j); return hr; }
    j->client = o;
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);
    j->uri = uri;
    IInspectable_AddRef(j->uri);
    j->kind = kind;
    if (!QueueUserWorkItem(get_worker, j, WT_EXECUTELONGFUNCTION)) get_worker(j);
    return S_OK;
}

HRESULT OV_IHttpClient_GetStringAsync(struct obj *o, void *uri, void **result)
{
    return client_get(o, uri, 0, &ASYNC_IAsyncOperationWithProgress_String_HttpProgress_, result);
}

HRESULT OV_IHttpClient_GetBufferAsync(struct obj *o, void *uri, void **result)
{
    return client_get(o, uri, 1, &ASYNC_IAsyncOperationWithProgress_IBuffer_HttpProgress_, result);
}

HRESULT OV_IHttpClient_GetInputStreamAsync(struct obj *o, void *uri, void **result)
{
    return client_get(o, uri, 2, &ASYNC_IAsyncOperationWithProgress_IInputStream_HttpProgress_, result);
}

/* ---- ApplicationLanguages (Wine's is a stub) --------------------------- */

HRESULT OV_IApplicationLanguagesStatics_get_Languages(struct obj *o, void **result)
{
    return user_languages_vector((IInspectable **)result);
}

HRESULT OV_IApplicationLanguagesStatics_get_ManifestLanguages(struct obj *o, void **result)
{
    return user_languages_vector((IInspectable **)result);
}

/* ---- HttpClient2: Try* variants returning HttpRequestResult ------------ */

struct try_job { struct obj *client; IInspectable *request; struct obj *op; };

static DWORD WINAPI try_worker(void *arg)
{
    struct try_job *j = arg;
    IInspectable *inner = NULL, *resp = NULL, *result = NULL;
    HRESULT hr = client_send(j->client, j->request, (void **)&inner);
    boolean ok;

    if (SUCCEEDED(hr)) hr = async_wait((IUnknown *)inner);
    if (SUCCEEDED(hr)) hr = VCALL(inner, 10, HRESULT (WINAPI *)(IInspectable *, IInspectable **))(inner, &resp);
    ok = SUCCEEDED(hr);
    if (SUCCEEDED(http_new_obj(L"Windows.Web.Http.HttpRequestResult", &result)))
    {
        struct obj *r = impl_from_inspectable(result);
        prop_put(r, L"Succeeded", PK_PRIM, &ok, sizeof(ok));
        prop_put(r, L"ExtendedError", PK_PRIM, &hr, sizeof(hr));
        http_put_o(r, L"RequestMessage", j->request);
        if (resp) http_put_o(r, L"ResponseMessage", resp);
    }
    TRACE("result %p ok %d hr %#lx\n", result, ok, hr);
    async_complete(j->op, S_OK, PK_OBJECT, &result, sizeof(result));
    if (result) IInspectable_Release(result);
    if (resp) IInspectable_Release(resp);
    if (inner) IInspectable_Release(inner);
    IInspectable_Release(j->request);
    IInspectable_Release((IInspectable *)&j->client->ifcs[0]);
    free(j);
    return 0;
}

static HRESULT client_try(struct obj *o, IInspectable *request, void **result)
{
    struct try_job *j = calloc(1, sizeof(*j));
    HRESULT hr = async_create_pending(&ASYNC_IAsyncOperationWithProgress_HttpRequestResult_HttpProgress_,
                                      ASYNC_OP_PROGRESS, &j->op, result);
    if (FAILED(hr)) { free(j); return hr; }
    j->client = o;
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);
    j->request = request;
    IInspectable_AddRef(request);
    if (!QueueUserWorkItem(try_worker, j, WT_EXECUTELONGFUNCTION)) try_worker(j);
    return S_OK;
}

static HRESULT client_try_simple(struct obj *o, const WCHAR *method, void *uri, void *content, void **result)
{
    IInspectable *req;
    HRESULT hr = make_request(method, uri, content, &req);
    if (FAILED(hr)) return hr;
    hr = client_try(o, req, result);
    IInspectable_Release(req);
    return hr;
}

HRESULT OV_IHttpClient2_TrySendRequestAsync(struct obj *o, void *request, void **result) { return client_try(o, request, result); }
HRESULT OV_IHttpClient2_TrySendRequestAsync_2(struct obj *o, void *request, INT32 opt, void **result) { return client_try(o, request, result); }
HRESULT OV_IHttpClient2_TryGetAsync(struct obj *o, void *uri, void **result) { return client_try_simple(o, L"GET", uri, NULL, result); }
HRESULT OV_IHttpClient2_TryGetAsync_2(struct obj *o, void *uri, INT32 opt, void **result) { return client_try_simple(o, L"GET", uri, NULL, result); }
HRESULT OV_IHttpClient2_TryDeleteAsync(struct obj *o, void *uri, void **result) { return client_try_simple(o, L"DELETE", uri, NULL, result); }
HRESULT OV_IHttpClient2_TryPostAsync(struct obj *o, void *uri, void *c, void **result) { return client_try_simple(o, L"POST", uri, c, result); }
HRESULT OV_IHttpClient2_TryPutAsync(struct obj *o, void *uri, void *c, void **result) { return client_try_simple(o, L"PUT", uri, c, result); }
