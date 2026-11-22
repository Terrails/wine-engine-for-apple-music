/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Typed HTTP header collections (CacheControl, UserAgent, Accept, ...):
 * views over a parent header map, bound to one header name. */
#include "gen/amshim_gen.h"
#include <winhttp.h>

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

struct hview { struct obj *parent; const WCHAR *name; };

static void hview_free(struct obj *o)
{
    struct hview *v = o->priv;
    IInspectable_Release((IInspectable *)&v->parent->ifcs[0]);
    free(v);
}

static HRESULT view_create(struct obj *parent, const WCHAR *cls, const WCHAR *name, void **result)
{
    struct hview *v;
    HRESULT hr = create_object_by_name(cls, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    v = calloc(1, sizeof(*v));
    v->parent = parent;
    v->name = name;
    IInspectable_AddRef((IInspectable *)&parent->ifcs[0]);
    impl_from_inspectable(*result)->priv = v;
    impl_from_inspectable(*result)->priv_free = hview_free;
    return S_OK;
}

static HRESULT view_add(struct obj *o, HSTRING input)
{
    struct hview *v = o->priv;
    TRACE("%s += %s\n", debugstr_w(v->name), debugstr_hstring(input));
    smap_append(v->parent, v->name, WindowsGetStringRawBuffer(input, NULL));
    return S_OK;
}

#define NS L"Windows.Web.Http.Headers."

HRESULT OV_IHttpCacheDirectiveHeaderValueCollection_ParseAdd(struct obj *o, HSTRING input) { return view_add(o, input); }
HRESULT OV_IHttpCacheDirectiveHeaderValueCollection_TryParseAdd(struct obj *o, HSTRING input, boolean *result) { *result = TRUE; return view_add(o, input); }
HRESULT OV_IHttpProductInfoHeaderValueCollection_ParseAdd(struct obj *o, HSTRING input) { return view_add(o, input); }
HRESULT OV_IHttpProductInfoHeaderValueCollection_TryParseAdd(struct obj *o, HSTRING input, boolean *result) { *result = TRUE; return view_add(o, input); }
HRESULT OV_IHttpMediaTypeWithQualityHeaderValueCollection_ParseAdd(struct obj *o, HSTRING input) { return view_add(o, input); }
HRESULT OV_IHttpMediaTypeWithQualityHeaderValueCollection_TryParseAdd(struct obj *o, HSTRING input, boolean *result) { *result = TRUE; return view_add(o, input); }
HRESULT OV_IHttpContentCodingWithQualityHeaderValueCollection_ParseAdd(struct obj *o, HSTRING input) { return view_add(o, input); }
HRESULT OV_IHttpContentCodingWithQualityHeaderValueCollection_TryParseAdd(struct obj *o, HSTRING input, boolean *result) { *result = TRUE; return view_add(o, input); }
HRESULT OV_IHttpLanguageRangeWithQualityHeaderValueCollection_ParseAdd(struct obj *o, HSTRING input) { return view_add(o, input); }
HRESULT OV_IHttpLanguageRangeWithQualityHeaderValueCollection_TryParseAdd(struct obj *o, HSTRING input, boolean *result) { *result = TRUE; return view_add(o, input); }
HRESULT OV_IHttpConnectionOptionHeaderValueCollection_ParseAdd(struct obj *o, HSTRING input) { return view_add(o, input); }
HRESULT OV_IHttpConnectionOptionHeaderValueCollection_TryParseAdd(struct obj *o, HSTRING input, boolean *result) { *result = TRUE; return view_add(o, input); }
HRESULT OV_IHttpContentCodingHeaderValueCollection_ParseAdd(struct obj *o, HSTRING input) { return view_add(o, input); }
HRESULT OV_IHttpContentCodingHeaderValueCollection_TryParseAdd(struct obj *o, HSTRING input, boolean *result) { *result = TRUE; return view_add(o, input); }

HRESULT OV_IHttpRequestHeaderCollection_get_Accept(struct obj *o, void **result) { return view_create(o, NS L"HttpMediaTypeWithQualityHeaderValueCollection", L"Accept", result); }
HRESULT OV_IHttpRequestHeaderCollection_get_AcceptEncoding(struct obj *o, void **result) { return view_create(o, NS L"HttpContentCodingWithQualityHeaderValueCollection", L"Accept-Encoding", result); }
HRESULT OV_IHttpRequestHeaderCollection_get_AcceptLanguage(struct obj *o, void **result) { return view_create(o, NS L"HttpLanguageRangeWithQualityHeaderValueCollection", L"Accept-Language", result); }
HRESULT OV_IHttpRequestHeaderCollection_get_CacheControl(struct obj *o, void **result) { return view_create(o, NS L"HttpCacheDirectiveHeaderValueCollection", L"Cache-Control", result); }
HRESULT OV_IHttpRequestHeaderCollection_get_Connection(struct obj *o, void **result) { return view_create(o, NS L"HttpConnectionOptionHeaderValueCollection", L"Connection", result); }
HRESULT OV_IHttpRequestHeaderCollection_get_UserAgent(struct obj *o, void **result) { return view_create(o, NS L"HttpProductInfoHeaderValueCollection", L"User-Agent", result); }
HRESULT OV_IHttpResponseHeaderCollection_get_CacheControl(struct obj *o, void **result) { return view_create(o, NS L"HttpCacheDirectiveHeaderValueCollection", L"Cache-Control", result); }
HRESULT OV_IHttpResponseHeaderCollection_get_Connection(struct obj *o, void **result) { return view_create(o, NS L"HttpConnectionOptionHeaderValueCollection", L"Connection", result); }
HRESULT OV_IHttpContentHeaderCollection_get_ContentEncoding(struct obj *o, void **result) { return view_create(o, NS L"HttpContentCodingHeaderValueCollection", L"Content-Encoding", result); }

/* ---- typed values: Cache-Control max-age, Expires, Last-Modified ----
 * The app derives how long a response stays fresh from these (max-age, else Expires, else a
 * Last-Modified heuristic, else "expires now"). Left unset, its store bag expired the moment it
 * arrived and was fetched again for every request, about twice a second. */

/* minimal IReference<TimeSpan> / IReference<DateTime>: both carry one INT64 */
struct ref64 { void **vtbl; LONG ref; INT64 value; };

static HRESULT WINAPI ref64_QueryInterface(struct ref64 *r, REFIID iid, void **out)
{
    /* handed out as the exact IReference<T> the caller declared; any interface asked of it is that one */
    InterlockedIncrement(&r->ref);
    *out = r;
    return S_OK;
}
static ULONG WINAPI ref64_AddRef(struct ref64 *r) { return InterlockedIncrement(&r->ref); }
static ULONG WINAPI ref64_Release(struct ref64 *r)
{
    ULONG ref = InterlockedDecrement(&r->ref);
    if (!ref) free(r);
    return ref;
}
static HRESULT WINAPI ref64_GetIids(struct ref64 *r, ULONG *count, IID **iids) { *count = 0; *iids = NULL; return S_OK; }
static HRESULT WINAPI ref64_GetRuntimeClassName(struct ref64 *r, HSTRING *name) { *name = NULL; return S_OK; }
static HRESULT WINAPI ref64_GetTrustLevel(struct ref64 *r, TrustLevel *level) { *level = BaseTrust; return S_OK; }
static HRESULT WINAPI ref64_get_Value(struct ref64 *r, INT64 *value) { *value = r->value; return S_OK; }

static void *ref64_vtbl[] = { ref64_QueryInterface, ref64_AddRef, ref64_Release, ref64_GetIids,
                              ref64_GetRuntimeClassName, ref64_GetTrustLevel, ref64_get_Value };

static HRESULT ref64_create(INT64 value, void **result)
{
    struct ref64 *r = calloc(1, sizeof(*r));
    if (!r) return E_OUTOFMEMORY;
    r->vtbl = ref64_vtbl; r->ref = 1; r->value = value;
    *result = r;
    return S_OK;
}

HRESULT OV_IHttpCacheDirectiveHeaderValueCollection_get_MaxAge(struct obj *o, void **result)
{
    struct hview *v = o->priv;
    const WCHAR *p = v ? smap_get(v->parent, v->name) : NULL;
    *result = NULL;
    while (p && *p)
    {
        while (*p == ' ' || *p == ',') p++;                 /* start of a directive ("s-maxage" must not match) */
        if (!wcsnicmp(p, L"max-age=", 8))
        {
            TRACE("max-age %s\n", debugstr_w(p + 8));
            return ref64_create((INT64)wcstoul(p + 8, NULL, 10) * 10000000, result);   /* TimeSpan: 100 ns units */
        }
        while (*p && *p != ',') p++;
    }
    return S_OK;
}

static HRESULT date_header(struct obj *o, const WCHAR *name, void **result)
{
    const WCHAR *s = smap_get(o, name);
    SYSTEMTIME st; FILETIME ft;
    *result = NULL;
    if (!s || !WinHttpTimeToSystemTime(s, &st) || !SystemTimeToFileTime(&st, &ft)) return S_OK;
    return ref64_create(((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime, result);     /* DateTime.UniversalTime */
}

HRESULT OV_IHttpContentHeaderCollection_get_Expires(struct obj *o, void **result) { return date_header(o, L"Expires", result); }
HRESULT OV_IHttpContentHeaderCollection_get_LastModified(struct obj *o, void **result) { return date_header(o, L"Last-Modified", result); }
