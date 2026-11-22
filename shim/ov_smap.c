/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* String->string maps (HTTP header collections, IMapView<String,String>) with
 * key/value pair iteration. Keys compare case-insensitively (HTTP semantics). */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

struct smap { HSTRING *keys, *vals; UINT32 count, cap; };

static void smap_free(struct obj *o)
{
    struct smap *m = o->priv;
    UINT32 i;
    for (i = 0; i < m->count; i++) { WindowsDeleteString(m->keys[i]); WindowsDeleteString(m->vals[i]); }
    free(m->keys); free(m->vals); free(m);
}

static struct smap *smap_of(struct obj *o)
{
    if (!o->priv)
    {
        o->priv = calloc(1, sizeof(struct smap));
        o->priv_free = smap_free;
    }
    return o->priv;
}

static int smap_find(struct smap *m, const WCHAR *key)
{
    UINT32 i;
    for (i = 0; i < m->count; i++)
        if (!wcsicmp(WindowsGetStringRawBuffer(m->keys[i], NULL), key)) return i;
    return -1;
}

const WCHAR *smap_get(struct obj *o, const WCHAR *key)
{
    struct smap *m = smap_of(o);
    int i = smap_find(m, key);
    return i >= 0 ? WindowsGetStringRawBuffer(m->vals[i], NULL) : NULL;
}

void smap_set(struct obj *o, const WCHAR *key, const WCHAR *value)
{
    struct smap *m;
    int i;
    EnterCriticalSection(&o->cs);
    m = smap_of(o);
    if ((i = smap_find(m, key)) >= 0)
    {
        WindowsDeleteString(m->vals[i]);
        hstring_from_w(value, &m->vals[i]);
    }
    else
    {
        if (m->count == m->cap)
        {
            m->cap = m->cap ? m->cap * 2 : 16;
            m->keys = realloc(m->keys, m->cap * sizeof(HSTRING));
            m->vals = realloc(m->vals, m->cap * sizeof(HSTRING));
        }
        hstring_from_w(key, &m->keys[m->count]);
        hstring_from_w(value, &m->vals[m->count]);
        m->count++;
    }
    LeaveCriticalSection(&o->cs);
}

void smap_append(struct obj *o, const WCHAR *key, const WCHAR *value)
{
    const WCHAR *old = smap_get(o, key);
    if (old && *old)
    {
        size_t len = wcslen(old) + wcslen(value) + 3;
        WCHAR *buf = malloc(len * sizeof(WCHAR));
        swprintf(buf, len, L"%s, %s", old, value);
        smap_set(o, key, buf);
        free(buf);
    }
    else smap_set(o, key, value);
}

void smap_remove(struct obj *o, const WCHAR *key)
{
    struct smap *m;
    int i;
    EnterCriticalSection(&o->cs);
    m = smap_of(o);
    if ((i = smap_find(m, key)) >= 0)
    {
        WindowsDeleteString(m->keys[i]); WindowsDeleteString(m->vals[i]);
        m->keys[i] = m->keys[m->count - 1];
        m->vals[i] = m->vals[m->count - 1];
        m->count--;
    }
    LeaveCriticalSection(&o->cs);
}

UINT32 smap_count(struct obj *o) { return smap_of(o)->count; }

void smap_at(struct obj *o, UINT32 i, const WCHAR **key, const WCHAR **value)
{
    struct smap *m = smap_of(o);
    *key = WindowsGetStringRawBuffer(m->keys[i], NULL);
    *value = WindowsGetStringRawBuffer(m->vals[i], NULL);
}

void smap_copy(struct obj *dst, struct obj *src)
{
    UINT32 i;
    EnterCriticalSection(&src->cs);
    for (i = 0; i < smap_count(src); i++)
    {
        const WCHAR *k, *v;
        smap_at(src, i, &k, &v);
        smap_set(dst, k, v);
    }
    LeaveCriticalSection(&src->cs);
}

/* ---- IMap<String,String> ---------------------------------------------- */

HRESULT OV_IMap_String_String__Lookup(struct obj *o, HSTRING key, HSTRING *result)
{
    const WCHAR *v = smap_get(o, WindowsGetStringRawBuffer(key, NULL));
    if (!v) { *result = NULL; return E_BOUNDS; }
    return hstring_from_w(v, result);
}

HRESULT OV_IMap_String_String__get_Size(struct obj *o, UINT32 *result) { *result = smap_count(o); return S_OK; }

HRESULT OV_IMap_String_String__HasKey(struct obj *o, HSTRING key, boolean *result)
{
    *result = smap_get(o, WindowsGetStringRawBuffer(key, NULL)) != NULL;
    return S_OK;
}

HRESULT OV_IMap_String_String__Insert(struct obj *o, HSTRING key, HSTRING value, boolean *result)
{
    const WCHAR *k = WindowsGetStringRawBuffer(key, NULL);
    *result = smap_get(o, k) != NULL;
    smap_set(o, k, WindowsGetStringRawBuffer(value, NULL));
    return S_OK;
}

HRESULT OV_IMap_String_String__Remove(struct obj *o, HSTRING key)
{
    smap_remove(o, WindowsGetStringRawBuffer(key, NULL));
    return S_OK;
}

HRESULT OV_IMap_String_String__Clear(struct obj *o)
{
    EnterCriticalSection(&o->cs);
    if (o->priv) { smap_free(o); o->priv = NULL; o->priv_free = NULL; }
    LeaveCriticalSection(&o->cs);
    return S_OK;
}

HRESULT OV_IMap_String_String__GetView(struct obj *o, void **result)
{
    HRESULT hr = create_object(&class_amshim_StringMapView, (IInspectable **)result);
    if (SUCCEEDED(hr)) smap_copy(impl_from_inspectable(*result), o);
    return hr;
}

/* ---- IMapView<String,String> ------------------------------------------ */

HRESULT OV_IMapView_String_String__Lookup(struct obj *o, HSTRING key, HSTRING *result) { return OV_IMap_String_String__Lookup(o, key, result); }
HRESULT OV_IMapView_String_String__get_Size(struct obj *o, UINT32 *result) { return OV_IMap_String_String__get_Size(o, result); }
HRESULT OV_IMapView_String_String__HasKey(struct obj *o, HSTRING key, boolean *result) { return OV_IMap_String_String__HasKey(o, key, result); }
HRESULT OV_IMapView_String_String__Split(struct obj *o, void **first, void **second) { *first = *second = NULL; return S_OK; }

/* ---- iteration: IIterable<IKeyValuePair<String,String>> --------------- */

struct kv_iter { struct obj *snap; UINT32 pos; };

static void kv_iter_free(struct obj *o)
{
    struct kv_iter *it = o->priv;
    IInspectable_Release((IInspectable *)&it->snap->ifcs[0]);
    free(it);
}

HRESULT OV_IIterable_IKeyValuePair_String_String___First(struct obj *o, void **result)
{
    IInspectable *snap, *iter;
    struct kv_iter *it;
    HRESULT hr;

    if (FAILED(hr = create_object(&class_amshim_StringMapView, &snap))) return hr;
    smap_copy(impl_from_inspectable(snap), o);
    if (FAILED(hr = create_object(&class_amshim_StringKVPIterator, &iter)))
    {
        IInspectable_Release(snap);
        return hr;
    }
    it = calloc(1, sizeof(*it));
    it->snap = impl_from_inspectable(snap);
    impl_from_inspectable(iter)->priv = it;
    impl_from_inspectable(iter)->priv_free = kv_iter_free;
    *result = iter;
    return S_OK;
}

HRESULT OV_IIterator_IKeyValuePair_String_String___get_HasCurrent(struct obj *o, boolean *result)
{
    struct kv_iter *it = o->priv;
    *result = it->pos < smap_count(it->snap);
    return S_OK;
}

HRESULT OV_IIterator_IKeyValuePair_String_String___get_Current(struct obj *o, void **result)
{
    struct kv_iter *it = o->priv;
    const WCHAR *k, *v;
    IInspectable *kvp;
    HSTRING hk, hv;
    HRESULT hr;

    if (it->pos >= smap_count(it->snap)) { *result = NULL; return E_BOUNDS; }
    smap_at(it->snap, it->pos, &k, &v);
    if (FAILED(hr = create_object(&class_amshim_StringKVP, &kvp))) return hr;
    hstring_from_w(k, &hk); hstring_from_w(v, &hv);
    prop_put(impl_from_inspectable(kvp), L"Key", PK_STRING, &hk, sizeof(hk));
    prop_put(impl_from_inspectable(kvp), L"Value", PK_STRING, &hv, sizeof(hv));
    WindowsDeleteString(hk); WindowsDeleteString(hv);
    *result = kvp;
    return S_OK;
}

HRESULT OV_IIterator_IKeyValuePair_String_String___MoveNext(struct obj *o, boolean *result)
{
    struct kv_iter *it = o->priv;
    UINT32 n = smap_count(it->snap);
    if (it->pos < n) it->pos++;
    *result = it->pos < n;
    return S_OK;
}

HRESULT OV_IIterator_IKeyValuePair_String_String___GetMany(struct obj *o, UINT32 items_len, void **items, UINT32 *result)
{
    UINT32 i = 0;
    boolean more = TRUE, has;
    while (i < items_len && SUCCEEDED(OV_IIterator_IKeyValuePair_String_String___get_HasCurrent(o, &has)) && has)
    {
        OV_IIterator_IKeyValuePair_String_String___get_Current(o, &items[i++]);
        OV_IIterator_IKeyValuePair_String_String___MoveNext(o, &more);
    }
    *result = i;
    return S_OK;
}
