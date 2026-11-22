/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Json: JsonArray and iterators (overview: amshim_json.h). */
#include "amshim_json.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

static void jiter_free(struct obj *o)
{
    struct jiter *it = o->priv;
    release(it->owner);
    free(it);
}

static HRESULT jiter_new(const struct class_info *cls, struct obj *owner, void **result)
{
    struct jiter *it;
    HRESULT hr = create_object(cls, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    it = calloc(1, sizeof(*it));
    it->owner = owner;
    addref(owner);
    impl_from_inspectable(*result)->priv = it;
    impl_from_inspectable(*result)->priv_free = jiter_free;
    return S_OK;
}

HRESULT OV_IIterable_IJsonValue__First(struct obj *o, void **result) { return jiter_new(&class_amshim_JsonIterator, o, result); }
HRESULT OV_IIterable_IKeyValuePair_String_IJsonValue___First(struct obj *o, void **result) { return jiter_new(&class_amshim_JsonKVPIterator, o, result); }

HRESULT OV_IIterator_IJsonValue__get_HasCurrent(struct obj *o, boolean *result)
{
    struct jiter *it = o->priv;
    *result = it->pos < node(it->owner)->count;
    return S_OK;
}
HRESULT OV_IIterator_IJsonValue__get_Current(struct obj *o, void **result)
{
    struct jiter *it = o->priv;
    *result = NULL;
    if (it->pos >= node(it->owner)->count) return E_BOUNDS;
    return RET_VALUE(node(it->owner)->vals[it->pos], result);
}
HRESULT OV_IIterator_IJsonValue__MoveNext(struct obj *o, boolean *result)
{
    struct jiter *it = o->priv;
    if (it->pos < node(it->owner)->count) it->pos++;
    *result = it->pos < node(it->owner)->count;
    return S_OK;
}
HRESULT OV_IIterator_IJsonValue__GetMany(struct obj *o, UINT32 items_len, void **items, UINT32 *result)
{
    struct jiter *it = o->priv;
    UINT32 i = 0;
    while (i < items_len && it->pos < node(it->owner)->count)
        RET_VALUE(node(it->owner)->vals[it->pos++], &items[i++]);
    *result = i;
    return S_OK;
}

HRESULT OV_IIterator_IKeyValuePair_String_IJsonValue___get_HasCurrent(struct obj *o, boolean *result)
{
    return OV_IIterator_IJsonValue__get_HasCurrent(o, result);
}
HRESULT OV_IIterator_IKeyValuePair_String_IJsonValue___get_Current(struct obj *o, void **result)
{
    struct jiter *it = o->priv, *kv;
    HRESULT hr;
    *result = NULL;
    if (it->pos >= node(it->owner)->count) return E_BOUNDS;
    /* the pair is an iterator-shaped object frozen at this position */
    if (FAILED(hr = jiter_new(&class_amshim_JsonKVP, it->owner, result))) return hr;
    kv = impl_from_inspectable(*result)->priv;
    kv->pos = it->pos;
    return S_OK;
}
HRESULT OV_IIterator_IKeyValuePair_String_IJsonValue___MoveNext(struct obj *o, boolean *result)
{
    return OV_IIterator_IJsonValue__MoveNext(o, result);
}
HRESULT OV_IIterator_IKeyValuePair_String_IJsonValue___GetMany(struct obj *o, UINT32 items_len, void **items, UINT32 *result)
{
    struct jiter *it = o->priv;
    UINT32 i = 0;
    while (i < items_len && it->pos < node(it->owner)->count)
    {
        OV_IIterator_IKeyValuePair_String_IJsonValue___get_Current(o, &items[i++]);
        it->pos++;
    }
    *result = i;
    return S_OK;
}
HRESULT OV_IKeyValuePair_String_IJsonValue__get_Key(struct obj *o, HSTRING *result)
{
    struct jiter *kv = o->priv;
    *result = NULL;
    if (kv->pos >= node(kv->owner)->count) return E_BOUNDS;
    return WindowsDuplicateString(node(kv->owner)->keys[kv->pos], result);
}
HRESULT OV_IKeyValuePair_String_IJsonValue__get_Value(struct obj *o, void **result)
{
    struct jiter *kv = o->priv;
    *result = NULL;
    if (kv->pos >= node(kv->owner)->count) return E_BOUNDS;
    return RET_VALUE(node(kv->owner)->vals[kv->pos], result);
}

/* ---- JsonArray --------------------------------------------------------- */

static struct obj *jat(struct obj *o, UINT32 index) { return index < node(o)->count ? node(o)->vals[index] : NULL; }

HRESULT OV_IJsonArray_GetObjectAt(struct obj *o, UINT32 index, void **result)
{ struct obj *v = jat(o, index); *result = NULL; return v ? jget_typed(v, JT_OBJECT, result) : E_BOUNDS; }
HRESULT OV_IJsonArray_GetArrayAt(struct obj *o, UINT32 index, void **result)
{ struct obj *v = jat(o, index); *result = NULL; return v ? jget_typed(v, JT_ARRAY, result) : E_BOUNDS; }
HRESULT OV_IJsonArray_GetStringAt(struct obj *o, UINT32 index, HSTRING *result)
{ struct obj *v = jat(o, index); *result = NULL; return v ? jget_string(v, result) : E_BOUNDS; }
HRESULT OV_IJsonArray_GetNumberAt(struct obj *o, UINT32 index, DOUBLE *result)
{ struct obj *v = jat(o, index); *result = 0; return v ? jget_number(v, result) : E_BOUNDS; }
HRESULT OV_IJsonArray_GetBooleanAt(struct obj *o, UINT32 index, boolean *result)
{ struct obj *v = jat(o, index); *result = FALSE; return v ? jget_boolean(v, result) : E_BOUNDS; }

/* IVector<IJsonValue> */
HRESULT OV_IVector_IJsonValue__GetAt(struct obj *o, UINT32 index, void **result)
{ struct obj *v = jat(o, index); *result = NULL; return v ? RET_VALUE(v, result) : E_BOUNDS; }
HRESULT OV_IVector_IJsonValue__get_Size(struct obj *o, UINT32 *result) { *result = node(o)->count; return S_OK; }
HRESULT OV_IVector_IJsonValue__IndexOf(struct obj *o, void *value, UINT32 *index, boolean *result)
{
    struct jnode *n = node(o);
    struct obj *v = value ? impl_from_inspectable(value) : NULL;
    UINT32 i;
    *index = 0; *result = FALSE;
    for (i = 0; i < n->count; i++)
        if (n->vals[i] == v) { *index = i; *result = TRUE; break; }
    return S_OK;
}
HRESULT OV_IVector_IJsonValue__SetAt(struct obj *o, UINT32 index, void *value)
{
    struct obj *v;
    if (index >= node(o)->count) return E_BOUNDS;
    if (!(v = jfrom_iface(value))) return E_INVALIDARG;
    release(node(o)->vals[index]);
    node(o)->vals[index] = v;
    return S_OK;
}
HRESULT OV_IVector_IJsonValue__InsertAt(struct obj *o, UINT32 index, void *value)
{
    struct jnode *n = node(o);
    struct obj *v;
    if (index > n->count) return E_BOUNDS;
    if (!(v = jfrom_iface(value))) return E_INVALIDARG;
    jgrow(n);
    memmove(&n->vals[index + 1], &n->vals[index], (n->count - index) * sizeof(*n->vals));
    n->vals[index] = v;
    n->count++;
    return S_OK;
}
HRESULT OV_IVector_IJsonValue__RemoveAt(struct obj *o, UINT32 index)
{
    struct jnode *n = node(o);
    if (index >= n->count) return E_BOUNDS;
    release(n->vals[index]);
    memmove(&n->vals[index], &n->vals[index + 1], (n->count - index - 1) * sizeof(*n->vals));
    n->count--;
    return S_OK;
}
HRESULT OV_IVector_IJsonValue__Append(struct obj *o, void *value)
{
    struct obj *v = jfrom_iface(value);
    if (!v) return E_INVALIDARG;
    jappend(node(o), v);
    return S_OK;
}
HRESULT OV_IVector_IJsonValue__RemoveAtEnd(struct obj *o)
{
    struct jnode *n = node(o);
    if (!n->count) return E_BOUNDS;
    release(n->vals[--n->count]);
    return S_OK;
}
HRESULT OV_IVector_IJsonValue__Clear(struct obj *o) { jnode_clear(node(o)); return S_OK; }
HRESULT OV_IVector_IJsonValue__GetMany(struct obj *o, UINT32 start, UINT32 items_len, void **items, UINT32 *result)
{
    UINT32 i = 0;
    while (i < items_len && start + i < node(o)->count) { RET_VALUE(node(o)->vals[start + i], &items[i]); i++; }
    *result = i;
    return S_OK;
}
