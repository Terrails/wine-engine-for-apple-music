/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* amshim runtime: generic WinRT object / factory machinery */
#include <initguid.h>
#include "amshim_rt.h"
#include <weakreference.h>

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

HRESULT hstring_from_w(const WCHAR *s, HSTRING *out)
{
    return WindowsCreateString(s, s ? wcslen(s) : 0, out);
}

struct obj *impl_from_inspectable(IInspectable *p)
{
    return ((struct ifc *)p)->obj;
}

/* ------------------------------------------------------------------ */

/* IWeakReference: holds a strong reference (simple, leak-tolerant) */
struct weakref { IWeakReference IWeakReference_iface; LONG ref; struct obj *obj; };

static HRESULT WINAPI weakref_QueryInterface(IWeakReference *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IWeakReference))
    {
        *out = iface;
        IWeakReference_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI weakref_AddRef(IWeakReference *iface)
{
    return InterlockedIncrement(&CONTAINING_RECORD(iface, struct weakref, IWeakReference_iface)->ref);
}

static ULONG WINAPI weakref_Release(IWeakReference *iface)
{
    struct weakref *w = CONTAINING_RECORD(iface, struct weakref, IWeakReference_iface);
    ULONG ref = InterlockedDecrement(&w->ref);
    if (!ref)
    {
        IInspectable_Release((IInspectable *)&w->obj->ifcs[0]);
        free(w);
    }
    return ref;
}

static HRESULT WINAPI weakref_Resolve(IWeakReference *iface, REFIID iid, IInspectable **out)
{
    struct weakref *w = CONTAINING_RECORD(iface, struct weakref, IWeakReference_iface);
    return obj_query(w->obj, iid, (void **)out);
}

static const IWeakReferenceVtbl weakref_vtbl =
{
    weakref_QueryInterface, weakref_AddRef, weakref_Release, weakref_Resolve,
};

static HRESULT WINAPI weaksrc_QueryInterface(struct ifc *This, REFIID iid, void **out)
{
    return obj_query(This->obj, iid, out);
}

static HRESULT WINAPI weaksrc_GetWeakReference(struct ifc *This, IWeakReference **out)
{
    struct weakref *w = calloc(1, sizeof(*w));
    if (!w) return E_OUTOFMEMORY;
    w->IWeakReference_iface.lpVtbl = &weakref_vtbl;
    w->ref = 1;
    w->obj = This->obj;
    InterlockedIncrement(&This->obj->ref);
    *out = &w->IWeakReference_iface;
    return S_OK;
}

static const void *weaksrc_vtbl[] =
{
    weaksrc_QueryInterface, ifc_AddRef, ifc_Release, weaksrc_GetWeakReference,
};

HRESULT obj_query(struct obj *o, REFIID iid, void **out)
{
    if (o->extra_iid && IsEqualGUID(iid, o->extra_iid))
    {
        *out = &o->extra;
        InterlockedIncrement(&o->ref);
        return S_OK;
    }
    if (IsEqualGUID(iid, &IID_IWeakReferenceSource) && !o->is_factory)
    {
        o->weak_src.vtbl = weaksrc_vtbl;
        o->weak_src.obj = o;
        *out = &o->weak_src;
        InterlockedIncrement(&o->ref);
        return S_OK;
    }

    unsigned int i;

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject))
    {
        *out = &o->ifcs[0];
        InterlockedIncrement(&o->ref);
        return S_OK;
    }
    for (i = 0; i < o->count; i++)
    {
        if (IsEqualGUID(iid, o->entries[i].iid))
        {
            *out = &o->ifcs[i];
            InterlockedIncrement(&o->ref);
            return S_OK;
        }
    }
    *out = NULL;
    return E_NOINTERFACE;
}

HRESULT WINAPI ifc_QueryInterface(struct ifc *This, REFIID iid, void **out)
{
    struct obj *o = This->obj;
    HRESULT hr = obj_query(o, iid, out);
    if (hr == E_NOINTERFACE && !IsEqualGUID(iid, &IID_IMarshal) && !IsEqualGUID(iid, &IID_INoMarshal) &&
        !IsEqualGUID(iid, &IID_IWeakReferenceSource))
        WARN("%s%s: no interface %s\n", debugstr_w(o->cls->name), o->is_factory ? " (factory)" : "",
             debugstr_guid(iid));
    return hr;
}

ULONG WINAPI ifc_AddRef(struct ifc *This)
{
    return InterlockedIncrement(&This->obj->ref);
}

static void obj_free(struct obj *o)
{
    struct prop *p, *n;
    if (o->priv_free) o->priv_free(o);
    for (p = o->props; p; p = n)
    {
        n = p->next;
        if (p->kind == PK_STRING) WindowsDeleteString(p->v.str);
        else if (p->kind == PK_OBJECT && p->v.unk) IUnknown_Release(p->v.unk);
        free(p);
    }
    o->cs.DebugInfo = NULL;
    DeleteCriticalSection(&o->cs);
    free(o->ifcs);
    free(o);
}

ULONG WINAPI ifc_Release(struct ifc *This)
{
    struct obj *o = This->obj;
    ULONG ref = InterlockedDecrement(&o->ref);
    if (!ref && !o->is_factory) obj_free(o);
    return ref;
}

HRESULT WINAPI ifc_GetIids(struct ifc *This, ULONG *count, IID **iids)
{
    struct obj *o = This->obj;
    unsigned int i;
    *iids = CoTaskMemAlloc(sizeof(IID) * (o->count ? o->count : 1));
    if (!*iids) return E_OUTOFMEMORY;
    for (i = 0; i < o->count; i++) (*iids)[i] = *o->entries[i].iid;
    *count = o->count;
    return S_OK;
}

HRESULT WINAPI ifc_GetRuntimeClassName(struct ifc *This, HSTRING *name)
{
    return hstring_from_w(This->obj->cls->name, name);
}

HRESULT WINAPI ifc_GetTrustLevel(struct ifc *This, TrustLevel *level)
{
    *level = BaseTrust;
    return S_OK;
}

/* ------------------------------------------------------------------ */

static struct obj *obj_alloc(const struct class_info *cls, const struct iface_entry *entries,
                             const struct iface_entry *extra)
{
    struct obj *o = calloc(1, sizeof(*o));
    unsigned int n = 0, i;
    const struct iface_entry *e;

    for (e = entries; e && e->iid; e++) n++;
    if (extra) n++;
    o->cls = cls;
    o->ref = 1;
    o->count = n;
    o->ifcs = calloc(n ? n : 1, sizeof(*o->ifcs));
    if (extra)
    {
        struct iface_entry *all = calloc(n + 1, sizeof(*all));
        all[0] = *extra;
        for (i = 1; i < n; i++) all[i] = entries[i - 1];
        o->entries = all;
    }
    else o->entries = entries;
    for (i = 0; i < n; i++)
    {
        o->ifcs[i].vtbl = o->entries[i].vtbl;
        o->ifcs[i].obj = o;
    }
    InitializeCriticalSection(&o->cs);
    return o;
}

const struct class_info *find_class(const WCHAR *name)
{
    struct class_info **c;
    for (c = all_classes; *c; c++)
        if (!wcscmp((*c)->name, name)) return *c;
    return NULL;
}

HRESULT create_object(const struct class_info *cls, IInspectable **out)
{
    struct obj *o;
    HRESULT hr;

    if (!cls->ifaces || !cls->ifaces[0].iid)
    {
        FIXME("%s has no instance interfaces\n", debugstr_w(cls->name));
        return E_NOTIMPL;
    }
    o = obj_alloc(cls, cls->ifaces, NULL);
    if (cls->init && FAILED(hr = cls->init(o)))
    {
        obj_free(o);
        return hr;
    }
    TRACE("created %s %p\n", debugstr_w(cls->name), o);
    *out = (IInspectable *)&o->ifcs[0];
    return S_OK;
}

HRESULT create_object_by_name(const WCHAR *name, IInspectable **out)
{
    const struct class_info *cls = find_class(name);
    if (!cls)
    {
        FIXME("class %s not in shim\n", debugstr_w(name));
        *out = NULL;
        return E_NOTIMPL;
    }
    return create_object(cls, out);
}

/* ------------------------------------------------------------------ */
/* property storage                                                    */

struct prop *prop_find(struct obj *o, const WCHAR *name)
{
    struct prop *p;
    for (p = o->props; p; p = p->next)
        if (!wcsicmp(p->name, name)) return p;
    return NULL;
}

HRESULT prop_put(struct obj *o, const WCHAR *name, enum prop_kind kind, const void *value, unsigned int size)
{
    struct prop *p;

    EnterCriticalSection(&o->cs);
    if (!(p = prop_find(o, name)))
    {
        p = calloc(1, sizeof(*p));
        p->name = name;
        p->kind = kind;
        p->size = size;
        p->next = o->props;
        o->props = p;
    }
    else if (p->kind == PK_STRING) { WindowsDeleteString(p->v.str); p->v.str = NULL; }
    else if (p->kind == PK_OBJECT && p->v.unk) { IUnknown_Release(p->v.unk); p->v.unk = NULL; }

    switch (kind)
    {
    case PK_STRING:
        WindowsDuplicateString(*(HSTRING *)value, &p->v.str);
        break;
    case PK_OBJECT:
        p->v.unk = *(IUnknown **)value;
        if (p->v.unk) IUnknown_AddRef(p->v.unk);
        break;
    default:
        memcpy(p->v.raw, value, min(size, sizeof(p->v.raw)));
        break;
    }
    LeaveCriticalSection(&o->cs);
    return S_OK;
}

HRESULT prop_get(struct obj *o, const WCHAR *name, enum prop_kind kind, void *result, unsigned int size,
                 const WCHAR *cls)
{
    struct prop *p;
    HRESULT hr = S_OK;

    EnterCriticalSection(&o->cs);
    if (!(p = prop_find(o, name)))
    {
        if (kind == PK_OBJECT && cls && find_class(cls))
        {
            IInspectable *inst;
            if (SUCCEEDED(create_object_by_name(cls, &inst)))
            {
                prop_put(o, name, PK_OBJECT, &inst, sizeof(inst));
                IInspectable_Release(inst);
                p = prop_find(o, name);
            }
        }
    }
    if (!p)
    {
        if (kind == PK_OBJECT)
            FIXME("%s.%s: unset object property, returning NULL\n", debugstr_w(o->cls->name), debugstr_w(name));
        memset(result, 0, size);
    }
    else switch (kind)
    {
    case PK_STRING:
        hr = WindowsDuplicateString(p->v.str, (HSTRING *)result);
        break;
    case PK_OBJECT:
        *(IUnknown **)result = p->v.unk;
        if (p->v.unk) IUnknown_AddRef(p->v.unk);
        break;
    default:
        memcpy(result, p->v.raw, min(size, sizeof(p->v.raw)));
        break;
    }
    LeaveCriticalSection(&o->cs);
    return hr;
}

/* ------------------------------------------------------------------ */
/* factories                                                           */

static HRESULT WINAPI factory_ActivateInstance(struct ifc *This, IInspectable **out)
{
    struct obj *o = This->obj;
    TRACE("%s\n", debugstr_w(o->cls->name));
    if (!o->cls->default_ctor)
    {
        FIXME("%s: no default constructor\n", debugstr_w(o->cls->name));
        return E_NOTIMPL;
    }
    return create_object(o->cls, out);
}

static const void *factory_vtbl[] =
{
    ifc_QueryInterface, ifc_AddRef, ifc_Release, ifc_GetIids, ifc_GetRuntimeClassName, ifc_GetTrustLevel,
    factory_ActivateInstance,
};

static const struct iface_entry factory_entry = { &IID_IActivationFactory, factory_vtbl, "IActivationFactory" };

struct factory_cache { const struct class_info *cls; struct obj *obj; };
static struct factory_cache factories[256];
static CRITICAL_SECTION factory_cs;
static CRITICAL_SECTION_DEBUG factory_cs_debug = { 0, 0, &factory_cs,
    { &factory_cs_debug.ProcessLocksList, &factory_cs_debug.ProcessLocksList }, 0, 0, { 0 } };
static CRITICAL_SECTION factory_cs = { &factory_cs_debug, -1, 0, 0, 0, 0 };

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const WCHAR *name = WindowsGetStringRawBuffer(classid, NULL);
    const struct class_info *cls = find_class(name);
    unsigned int i;

    if (!cls)
    {
        FIXME("class %s not implemented\n", debugstr_w(name));
        *factory = NULL;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    TRACE("%s\n", debugstr_w(name));
    EnterCriticalSection(&factory_cs);
    for (i = 0; i < ARRAY_SIZE(factories) && factories[i].cls; i++)
        if (factories[i].cls == cls) break;
    if (i < ARRAY_SIZE(factories) && !factories[i].cls)
    {
        struct obj *o = obj_alloc(cls, cls->factory_ifaces, &factory_entry);
        o->is_factory = TRUE;
        factory_created(o);
        factories[i].cls = cls;
        factories[i].obj = o;
    }
    LeaveCriticalSection(&factory_cs);
    if (i >= ARRAY_SIZE(factories)) return E_OUTOFMEMORY;
    InterlockedIncrement(&factories[i].obj->ref);
    *factory = (IActivationFactory *)&factories[i].obj->ifcs[0];
    return S_OK;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}
