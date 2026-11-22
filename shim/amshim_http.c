/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Web.Http: helpers shared by the ov_http_*.c files (see amshim_http.h). */
#include "amshim_http.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

const GUID IID_IUriRuntimeClass = {0x9e365e57, 0x48b2, 0x4160, {0x95, 0x6f, 0xc7, 0x38, 0x51, 0x20, 0xbb, 0xfc}};

/* ---- small helpers ---------------------------------------------------- */

void http_put_s(struct obj *o, const WCHAR *name, const WCHAR *val)
{
    HSTRING s;
    hstring_from_w(val, &s);
    prop_put(o, name, PK_STRING, &s, sizeof(s));
    WindowsDeleteString(s);
}

void http_put_o(struct obj *o, const WCHAR *name, IInspectable *val)
{
    prop_put(o, name, PK_OBJECT, &val, sizeof(val));
}

IInspectable *http_get_o(struct obj *o, const WCHAR *name)
{
    IInspectable *v = NULL;
    struct prop *p = prop_find(o, name);
    if (p && p->kind == PK_OBJECT && (v = (IInspectable *)p->v.unk)) IInspectable_AddRef(v);
    return v;
}

HRESULT http_new_obj(const WCHAR *cls, IInspectable **out)
{
    return create_object_by_name(cls, out);
}

/* is this interface pointer one of our objects of the given class? */
struct obj *http_ours(IUnknown *p, const WCHAR *cls)
{
    struct obj *o;
    const struct class_info *ci = find_class(cls);
    if (!p || !ci) return NULL;
    o = ((struct ifc *)p)->obj;
    /* our tear-offs have a known vtable in slot 0 of QueryInterface */
    if (((void **)*(void ***)p)[0] != (void *)ifc_QueryInterface) return NULL;
    return o->cls == ci ? o : NULL;
}

HRESULT http_uri_string(IUnknown *uri, HSTRING *out)
{
    IUnknown *u;
    HRESULT hr = IUnknown_QueryInterface(uri, &IID_IUriRuntimeClass, (void **)&u);
    if (FAILED(hr)) return hr;
    hr = VCALL(u, 6, HRESULT (WINAPI *)(IUnknown *, HSTRING *))(u, out); /* get_AbsoluteUri */
    IUnknown_Release(u);
    return hr;
}

HRESULT http_method_string(IUnknown *method, HSTRING *out)
{
    IUnknown *m;
    HRESULT hr;
    if (!method) return hstring_from_w(L"GET", out);
    hr = IUnknown_QueryInterface(method, &IID_Windows_Web_Http_IHttpMethod, (void **)&m);
    if (FAILED(hr)) return hr;
    hr = VCALL(m, 6, HRESULT (WINAPI *)(IUnknown *, HSTRING *))(m, out); /* get_Method */
    IUnknown_Release(m);
    return hr;
}
