/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Foundation.Uri on top of urlmon's IUri (Wine's parser/canonicaliser).
 * Replaces Wine's class, which lacks IUriRuntimeClassWithAbsoluteCanonicalUri. */
#include "gen/amshim_gen.h"
#include <urlmon.h>
#include <shlwapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

static void uri_free(struct obj *o)
{
    if (o->priv) IUri_Release((IUri *)o->priv);
}

static HRESULT uri_create(const WCHAR *str, void **result)
{
    IUri *uri;
    HRESULT hr = CreateUri(str, Uri_CREATE_ALLOW_RELATIVE | Uri_CREATE_CANONICALIZE, 0, &uri);
    if (FAILED(hr))
    {
        WARN("invalid uri %s: %#lx\n", debugstr_w(str), hr);
        *result = NULL;
        return E_INVALIDARG;
    }
    if (FAILED(hr = create_object_by_name(L"Windows.Foundation.Uri", (IInspectable **)result)))
    {
        IUri_Release(uri);
        return hr;
    }
    impl_from_inspectable(*result)->priv = uri;
    impl_from_inspectable(*result)->priv_free = uri_free;
    return S_OK;
}

static HRESULT uri_prop(struct obj *o, Uri_PROPERTY prop, HSTRING *result)
{
    BSTR s = NULL;
    HRESULT hr;
    *result = NULL;
    if (!o->priv) return S_OK;   /* an empty Uri (auto-created for an unset property): all parts are "" */
    hr = IUri_GetPropertyBSTR((IUri *)o->priv, prop, &s, 0);
    if (SUCCEEDED(hr) && s) hr = WindowsCreateString(s, SysStringLen(s), result);
    else if (hr == S_FALSE) hr = S_OK;
    SysFreeString(s);
    return hr;
}

HRESULT OV_IUriRuntimeClassFactory_CreateUri(struct obj *o, HSTRING uri, void **result)
{
    return uri_create(WindowsGetStringRawBuffer(uri, NULL), result);
}

HRESULT OV_IUriRuntimeClassFactory_CreateWithRelativeUri(struct obj *o, HSTRING base, HSTRING rel, void **result)
{
    WCHAR out[8192];
    DWORD len = ARRAY_SIZE(out);
    HRESULT hr = UrlCombineW(WindowsGetStringRawBuffer(base, NULL), WindowsGetStringRawBuffer(rel, NULL), out, &len, 0);
    if (FAILED(hr)) return hr;
    return uri_create(out, result);
}

HRESULT OV_IUriRuntimeClass_get_AbsoluteUri(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_ABSOLUTE_URI, r); }
HRESULT OV_IUriRuntimeClass_get_DisplayUri(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_DISPLAY_URI, r); }
HRESULT OV_IUriRuntimeClass_get_Domain(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_DOMAIN, r); }
HRESULT OV_IUriRuntimeClass_get_Extension(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_EXTENSION, r); }
HRESULT OV_IUriRuntimeClass_get_Fragment(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_FRAGMENT, r); }
HRESULT OV_IUriRuntimeClass_get_Host(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_HOST, r); }
HRESULT OV_IUriRuntimeClass_get_Password(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_PASSWORD, r); }
HRESULT OV_IUriRuntimeClass_get_Path(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_PATH, r); }
HRESULT OV_IUriRuntimeClass_get_Query(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_QUERY, r); }
HRESULT OV_IUriRuntimeClass_get_RawUri(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_RAW_URI, r); }
HRESULT OV_IUriRuntimeClass_get_SchemeName(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_SCHEME_NAME, r); }
HRESULT OV_IUriRuntimeClass_get_UserName(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_USER_NAME, r); }
HRESULT OV_IUriRuntimeClassWithAbsoluteCanonicalUri_get_AbsoluteCanonicalUri(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_ABSOLUTE_URI, r); }
HRESULT OV_IUriRuntimeClassWithAbsoluteCanonicalUri_get_DisplayIri(struct obj *o, HSTRING *r) { return uri_prop(o, Uri_PROPERTY_DISPLAY_URI, r); }

HRESULT OV_IUriRuntimeClass_get_Port(struct obj *o, INT32 *result)
{
    DWORD port = 0;
    IUri_GetPort((IUri *)o->priv, &port);
    *result = port;
    return S_OK;
}

HRESULT OV_IUriRuntimeClass_get_Suspicious(struct obj *o, boolean *result) { *result = FALSE; return S_OK; }

HRESULT OV_IUriRuntimeClass_Equals(struct obj *o, void *other, boolean *result)
{
    HSTRING a = NULL, b = NULL;
    INT32 cmp = 1;
    IUnknown *u;
    *result = FALSE;
    if (!other) return S_OK;
    uri_prop(o, Uri_PROPERTY_ABSOLUTE_URI, &a);
    if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)other, &IID_Windows_Foundation_IUriRuntimeClass, (void **)&u)))
    {
        ((HRESULT (WINAPI *)(IUnknown *, HSTRING *))((void **)*(void ***)u)[6])(u, &b);
        IUnknown_Release(u);
    }
    WindowsCompareStringOrdinal(a, b, &cmp);
    *result = !cmp;
    WindowsDeleteString(a); WindowsDeleteString(b);
    return S_OK;
}

HRESULT OV_IUriRuntimeClass_CombineUri(struct obj *o, HSTRING rel, void **result)
{
    HSTRING base;
    HRESULT hr = uri_prop(o, Uri_PROPERTY_ABSOLUTE_URI, &base);
    if (FAILED(hr)) return hr;
    hr = OV_IUriRuntimeClassFactory_CreateWithRelativeUri(o, base, rel, result);
    WindowsDeleteString(base);
    return hr;
}

/* IStringable is shared by several classes */
HRESULT OV_IStringable_ToString(struct obj *o, HSTRING *result)
{
    if (!wcsncmp(o->cls->name, L"Windows.Data.Json.", 18)) return json_to_string(o, result);
    if (!wcscmp(o->cls->name, L"Windows.Foundation.Uri")) return uri_prop(o, Uri_PROPERTY_ABSOLUTE_URI, result);
    FIXME("%s\n", debugstr_w(o->cls->name));
    return hstring_from_w(L"", result);
}

HRESULT OV_IUriEscapeStatics_EscapeComponent(struct obj *o, HSTRING in, HSTRING *result)
{
    const WCHAR *s = WindowsGetStringRawBuffer(in, NULL);
    size_t i, n = 0, len = wcslen(s);
    WCHAR *out = malloc((len * 3 + 1) * sizeof(WCHAR));
    HRESULT hr;
    /* RFC 3986 unreserved characters stay as-is; everything else is UTF-8 percent-encoded */
    for (i = 0; i < len; i++)
    {
        WCHAR c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || wcschr(L"-_.~", c))
            out[n++] = c;
        else
        {
            char u[8];
            int k, m = WideCharToMultiByte(CP_UTF8, 0, &s[i], (IS_HIGH_SURROGATE(c) && i + 1 < len) ? 2 : 1, u, sizeof(u), NULL, NULL);
            if (IS_HIGH_SURROGATE(c) && i + 1 < len) i++;
            out = realloc(out, (len * 3 + n + m * 3 + 1) * sizeof(WCHAR));
            for (k = 0; k < m; k++) n += swprintf(out + n, 4, L"%%%02X", (BYTE)u[k]);
        }
    }
    hr = WindowsCreateString(out, n, result);
    free(out);
    return hr;
}

HRESULT OV_IUriEscapeStatics_UnescapeComponent(struct obj *o, HSTRING in, HSTRING *result)
{
    const WCHAR *s = WindowsGetStringRawBuffer(in, NULL);
    size_t i, n = 0, len = wcslen(s);
    char *bytes = malloc(len * 3 + 1);
    WCHAR *w;
    int wn;
    HRESULT hr;
    /* decode %XX to bytes, re-encode other characters as UTF-8, then decode as UTF-8 */
    for (i = 0; i < len; i++)
    {
        if (s[i] == '%' && i + 2 < len + 0 && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2]))
        {
            WCHAR hex[3] = { s[i + 1], s[i + 2], 0 };
            bytes[n++] = (char)wcstol(hex, NULL, 16);
            i += 2;
        }
        else n += WideCharToMultiByte(CP_UTF8, 0, &s[i], 1, bytes + n, 4, NULL, NULL);
    }
    wn = MultiByteToWideChar(CP_UTF8, 0, bytes, n, NULL, 0);
    w = malloc((wn + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, bytes, n, w, wn);
    hr = WindowsCreateString(w, wn, result);
    free(w); free(bytes);
    return hr;
}
