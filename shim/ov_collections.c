/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: string->object maps (ApplicationDataContainerSettings).
 * A map bound to a registry key (omap_bind_registry: the ApplicationData settings containers)
 * stores its values there (amshim_settings.c); values of types that cannot be stored stay in
 * memory. Unbound maps (e.g. property bags of HTTP content) are in memory only. */
#include "gen/amshim_gen.h"
#include <winreg.h>

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

struct map_entry { HSTRING key; IInspectable *value; };
struct map { struct map_entry *e; UINT32 count, cap; HKEY reg; };

static void map_clear(struct map *m)
{
    UINT32 i;
    for (i = 0; i < m->count; i++)
    {
        WindowsDeleteString(m->e[i].key);
        if (m->e[i].value) IInspectable_Release(m->e[i].value);
    }
    free(m->e);
    m->e = NULL;
    m->count = m->cap = 0;
}

static void map_free(struct obj *o)
{
    struct map *m = o->priv;
    map_clear(m);
    if (m->reg) RegCloseKey(m->reg);
    free(m);
}

struct map *get_map(struct obj *o)
{
    if (!o->priv)
    {
        o->priv = calloc(1, sizeof(struct map));
        o->priv_free = map_free;
    }
    return o->priv;
}

int map_find(struct map *m, HSTRING key)
{
    UINT32 i;
    INT32 r;
    for (i = 0; i < m->count; i++)
        if (SUCCEEDED(WindowsCompareStringOrdinal(m->e[i].key, key, &r)) && !r) return i;
    return -1;
}

static void map_remove_at(struct map *m, int i)
{
    WindowsDeleteString(m->e[i].key);
    if (m->e[i].value) IInspectable_Release(m->e[i].value);
    m->e[i] = m->e[--m->count];
}

void omap_bind_registry(struct obj *o, HKEY key)
{
    struct map *m;
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    if (m->reg) RegCloseKey(m->reg);
    m->reg = key;
    LeaveCriticalSection(&o->cs);
}

HKEY omap_registry(struct obj *o)
{
    HKEY key;
    EnterCriticalSection(&o->cs);
    key = get_map(o)->reg;
    LeaveCriticalSection(&o->cs);
    return key;
}

HRESULT OV_IMap_String_Object__Lookup(struct obj *o, HSTRING key, IInspectable **result)
{
    struct map *m;
    int i;
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    i = map_find(m, key);
    *result = i >= 0 ? m->e[i].value : NULL;
    if (*result) IInspectable_AddRef((IInspectable *)*result);
    else if (m->reg) settings_read(m->reg, key, result);
    LeaveCriticalSection(&o->cs);
    TRACE("%s -> %p\n", debugstr_hstring(key), *result);
    return S_OK; /* ApplicationDataContainerSettings returns null for missing keys */
}

HRESULT OV_IMap_String_Object__get_Size(struct obj *o, UINT32 *result)
{
    struct map *m;
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    *result = m->count + (m->reg ? settings_count(m->reg) : 0);
    LeaveCriticalSection(&o->cs);
    return S_OK;
}

HRESULT OV_IMap_String_Object__HasKey(struct obj *o, HSTRING key, boolean *result)
{
    struct map *m;
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    *result = map_find(m, key) >= 0 || (m->reg && settings_has(m->reg, key));
    LeaveCriticalSection(&o->cs);
    return S_OK;
}

HRESULT OV_IMap_String_Object__Insert(struct obj *o, HSTRING key, IInspectable *value, boolean *result)
{
    struct map *m;
    int i;
    TRACE("%s %p\n", debugstr_hstring(key), value);
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    i = map_find(m, key);
    *result = i >= 0 || (m->reg && settings_has(m->reg, key));
    if (m->reg && settings_write(m->reg, key, value) == S_OK)
    {
        if (i >= 0) map_remove_at(m, i);
        LeaveCriticalSection(&o->cs);
        return S_OK;
    }
    if (m->reg) settings_delete(m->reg, key);   /* not storable: the in-memory value replaces it */
    if (value) IInspectable_AddRef((IInspectable *)value);
    if (i >= 0)
    {
        if (m->e[i].value) IInspectable_Release(m->e[i].value);
        m->e[i].value = value;
    }
    else
    {
        if (m->count == m->cap)
        {
            m->cap = m->cap ? m->cap * 2 : 16;
            m->e = realloc(m->e, m->cap * sizeof(*m->e));
        }
        WindowsDuplicateString(key, &m->e[m->count].key);
        m->e[m->count++].value = value;
    }
    LeaveCriticalSection(&o->cs);
    return S_OK;
}

HRESULT OV_IMap_String_Object__Remove(struct obj *o, HSTRING key)
{
    struct map *m;
    int i;
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    if ((i = map_find(m, key)) >= 0) map_remove_at(m, i);
    if (m->reg) settings_delete(m->reg, key);
    LeaveCriticalSection(&o->cs);
    return S_OK;
}

HRESULT OV_IMap_String_Object__Clear(struct obj *o)
{
    struct map *m;
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    map_clear(m);
    if (m->reg) settings_clear(m->reg);
    LeaveCriticalSection(&o->cs);
    return S_OK;
}

/* ---- generic helpers for other string->object maps ---- */

IInspectable *omap_get(struct obj *o, HSTRING key)
{
    struct map *m;
    IInspectable *v = NULL;
    int i;
    EnterCriticalSection(&o->cs);
    m = get_map(o);
    if ((i = map_find(m, key)) >= 0 && (v = m->e[i].value)) IInspectable_AddRef(v);
    LeaveCriticalSection(&o->cs);
    return v;
}

void omap_set(struct obj *o, HSTRING key, IInspectable *value)
{
    boolean replaced;
    OV_IMap_String_Object__Insert(o, key, value, &replaced);
}

void omap_remove(struct obj *o, HSTRING key)
{
    OV_IMap_String_Object__Remove(o, key);
}

/* copy all entries of src into dst (snapshot for map views) */
void omap_copy(struct obj *dst, struct obj *src)
{
    struct map *m;
    UINT32 i;
    EnterCriticalSection(&src->cs);
    m = get_map(src);
    for (i = 0; i < m->count; i++) omap_set(dst, m->e[i].key, m->e[i].value);
    LeaveCriticalSection(&src->cs);
}

/* IMapView<String, ApplicationDataContainer> snapshot */
HRESULT OV_IMapView_String_ApplicationDataContainer__Lookup(struct obj *o, HSTRING key, void **result)
{
    *result = omap_get(o, key);
    return *result ? S_OK : E_BOUNDS;
}

HRESULT OV_IMapView_String_ApplicationDataContainer__get_Size(struct obj *o, UINT32 *result)
{
    return OV_IMap_String_Object__get_Size(o, result);
}

HRESULT OV_IMapView_String_ApplicationDataContainer__HasKey(struct obj *o, HSTRING key, boolean *result)
{
    return OV_IMap_String_Object__HasKey(o, key, result);
}

UINT32 omap_count(struct obj *o) { return get_map(o)->count; }

void omap_at(struct obj *o, UINT32 i, HSTRING *key, IInspectable **value)
{
    struct map *m = get_map(o);
    *key = m->e[i].key;
    *value = m->e[i].value;
}

/* generic iteration over an omap: iterator objects keep a snapshot object + index */
struct oiter { struct obj *snap; UINT32 pos; const struct class_info *kvp_cls; };

static void oiter_free(struct obj *o)
{
    struct oiter *it = o->priv;
    IInspectable_Release((IInspectable *)&it->snap->ifcs[0]);
    free(it);
}

HRESULT omap_first(struct obj *o, const struct class_info *view_cls, const struct class_info *iter_cls,
                   const struct class_info *kvp_cls, void **result)
{
    IInspectable *snap, *iter;
    struct oiter *it;
    HRESULT hr;
    if (FAILED(hr = create_object(view_cls, &snap))) return hr;
    omap_copy(impl_from_inspectable(snap), o);
    if (FAILED(hr = create_object(iter_cls, &iter))) { IInspectable_Release(snap); return hr; }
    it = calloc(1, sizeof(*it));
    it->snap = impl_from_inspectable(snap);
    it->kvp_cls = kvp_cls;
    impl_from_inspectable(iter)->priv = it;
    impl_from_inspectable(iter)->priv_free = oiter_free;
    *result = iter;
    return S_OK;
}

HRESULT oiter_current(struct obj *o, void **result)
{
    struct oiter *it = o->priv;
    IInspectable *kvp, *v;
    HSTRING k;
    HRESULT hr;
    if (it->pos >= omap_count(it->snap)) { *result = NULL; return E_BOUNDS; }
    omap_at(it->snap, it->pos, &k, &v);
    if (FAILED(hr = create_object(it->kvp_cls, &kvp))) return hr;
    prop_put(impl_from_inspectable(kvp), L"Key", PK_STRING, &k, sizeof(k));
    prop_put(impl_from_inspectable(kvp), L"Value", PK_OBJECT, &v, sizeof(v));
    *result = kvp;
    return S_OK;
}

HRESULT oiter_has_current(struct obj *o, boolean *result)
{
    struct oiter *it = o->priv;
    *result = it->pos < omap_count(it->snap);
    return S_OK;
}

HRESULT oiter_move_next(struct obj *o, boolean *result)
{
    struct oiter *it = o->priv;
    UINT32 n = omap_count(it->snap);
    if (it->pos < n) it->pos++;
    *result = it->pos < n;
    return S_OK;
}

/* IIterable<IKeyValuePair<String, ResourceMap>> */
HRESULT OV_IIterable_IKeyValuePair_String_ResourceMap___First(struct obj *o, void **result)
{
    return omap_first(o, &class_amshim_ResourceMapMapView, &class_amshim_ResourceMapKVPIterator,
                      &class_amshim_ResourceMapKVP, result);
}
HRESULT OV_IIterator_IKeyValuePair_String_ResourceMap___get_Current(struct obj *o, void **r) { return oiter_current(o, r); }
HRESULT OV_IIterator_IKeyValuePair_String_ResourceMap___get_HasCurrent(struct obj *o, boolean *r) { return oiter_has_current(o, r); }
HRESULT OV_IIterator_IKeyValuePair_String_ResourceMap___MoveNext(struct obj *o, boolean *r) { return oiter_move_next(o, r); }
