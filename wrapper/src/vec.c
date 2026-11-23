/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A small IVector<T> of 8-byte elements (see vec.h). */
#include "vec.h"

/* pinterface IIDs (WinRT's SHA-1 of the type signature) of IVector, IVectorView, IIterable,
 * IIterator for UINT64 and IInspectable */
static const GUID vec_iids[2][4] = {
    { {0xcf4a637f,0x456a,0x5a47,{0xa9,0x27,0xbc,0xf0,0x7f,0xc6,0x99,0x01}}, {0x23d156c7,0x7ef9,0x5096,{0xaa,0xba,0x1e,0x6c,0x9a,0xb5,0xce,0xb4}},
      {0x4b3a3229,0x7995,0x5f3c,{0xb2,0x48,0x6c,0x1f,0x7e,0x66,0x4f,0x01}}, {0xc473ed96,0x76e3,0x5ff2,{0x94,0x35,0x47,0xfe,0xeb,0xfe,0x95,0x39}} },
    { {0xb32bdca4,0x5e52,0x5b27,{0xbc,0x5d,0xd6,0x6a,0x1a,0x26,0x8c,0x2a}}, {0xa6487363,0xb074,0x5c60,{0xab,0x16,0x86,0x6d,0xce,0x4e,0xe5,0x4d}},
      {0x092b849b,0x60b1,0x52be,{0xa4,0x4a,0x6f,0xe8,0xe9,0x33,0xcb,0xe4}}, {0x44a94f2d,0x04f8,0x5091,{0xb3,0x36,0xbe,0x78,0x92,0xdd,0x10,0xbe}} },
};

struct vec { const void **vector_vtbl, **view_vtbl, **iterable_vtbl; LONG ref; int type; UINT32 n; UINT64 v[VEC_MAX]; };
struct vit { const void **vtbl; LONG ref; struct vec *vec; UINT32 pos; };

#define FROM_VIEW(p) ((struct vec *)((char *)(p) - sizeof(void *)))
#define FROM_ITERABLE(p) ((struct vec *)((char *)(p) - 2 * sizeof(void *)))

static void vec_hold(struct vec *v, UINT64 x) { if (v->type != VEC_U64 && x) IUnknown_AddRef((IUnknown *)(ULONG_PTR)x); }
static void vec_drop(struct vec *v, UINT64 x) { if (v->type != VEC_U64 && x) IUnknown_Release((IUnknown *)(ULONG_PTR)x); }
static UINT64 vec_out(struct vec *v, UINT32 i) { vec_hold(v, v->v[i]); return v->v[i]; }

static HRESULT WINAPI vec_QI(struct vec *v, REFIID iid, void **out)
{
    const GUID *g = vec_iids[v->type];
    if (IsEqualGUID(iid, &g[0]) || IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable_) ||
        IsEqualGUID(iid, &IID_IAgileObject_)) *out = &v->vector_vtbl;
    else if (IsEqualGUID(iid, &g[1])) *out = &v->view_vtbl;
    else if (IsEqualGUID(iid, &g[2])) *out = &v->iterable_vtbl;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&v->ref);
    return S_OK;
}
static ULONG WINAPI vec_AddRef(struct vec *v) { return InterlockedIncrement(&v->ref); }
static ULONG WINAPI vec_Release(struct vec *v)
{
    ULONG r = InterlockedDecrement(&v->ref);
    if (!r)
    {
        for (UINT32 i = 0; i < v->n; i++) vec_drop(v, v->v[i]);
        free(v);
    }
    return r;
}
static HRESULT WINAPI vec_GetAt(struct vec *v, UINT32 i, UINT64 *out)
{
    if (i >= v->n) { *out = 0; return E_BOUNDS; }
    *out = vec_out(v, i);
    return S_OK;
}
static HRESULT WINAPI vec_get_Size(struct vec *v, UINT32 *n) { *n = v->n; return S_OK; }
static HRESULT WINAPI vec_GetView(struct vec *v, void **out) { InterlockedIncrement(&v->ref); *out = &v->view_vtbl; return S_OK; }
static HRESULT WINAPI vec_IndexOf(struct vec *v, UINT64 x, UINT32 *i, boolean *found)
{
    for (*i = 0; *i < v->n; (*i)++) if (v->v[*i] == x) { *found = TRUE; return S_OK; }
    *i = 0; *found = FALSE;
    return S_OK;
}
static HRESULT WINAPI vec_SetAt(struct vec *v, UINT32 i, UINT64 x)
{
    if (i >= v->n) return E_BOUNDS;
    vec_hold(v, x); vec_drop(v, v->v[i]); v->v[i] = x;
    return S_OK;
}
static HRESULT WINAPI vec_InsertAt(struct vec *v, UINT32 i, UINT64 x)
{
    if (i > v->n) return E_BOUNDS;
    if (v->n == VEC_MAX) return E_OUTOFMEMORY;
    memmove(v->v + i + 1, v->v + i, (v->n - i) * sizeof(UINT64));
    vec_hold(v, x); v->v[i] = x; v->n++;
    return S_OK;
}
static HRESULT WINAPI vec_RemoveAt(struct vec *v, UINT32 i)
{
    if (i >= v->n) return E_BOUNDS;
    vec_drop(v, v->v[i]);
    memmove(v->v + i, v->v + i + 1, (v->n - i - 1) * sizeof(UINT64)); v->n--;
    return S_OK;
}
static HRESULT WINAPI vec_Append(struct vec *v, UINT64 x) { return vec_InsertAt(v, v->n, x); }
static HRESULT WINAPI vec_RemoveAtEnd(struct vec *v) { return v->n ? vec_RemoveAt(v, v->n - 1) : E_BOUNDS; }
static HRESULT WINAPI vec_Clear(struct vec *v) { while (v->n) vec_RemoveAt(v, v->n - 1); return S_OK; }
static HRESULT WINAPI vec_GetMany(struct vec *v, UINT32 start, UINT32 cap, UINT64 *items, UINT32 *n)
{
    for (*n = 0; start + *n < v->n && *n < cap; (*n)++) items[*n] = vec_out(v, start + *n);
    return S_OK;
}
static HRESULT WINAPI vec_ReplaceAll(struct vec *v, UINT32 n, UINT64 *items)
{
    vec_Clear(v);
    for (UINT32 i = 0; i < n; i++) vec_Append(v, items[i]);
    return S_OK;
}

/* IVectorView and IIterable tear-offs */
static HRESULT WINAPI view_QI(void *p, REFIID iid, void **out) { return vec_QI(FROM_VIEW(p), iid, out); }
static ULONG WINAPI view_AddRef(void *p) { return vec_AddRef(FROM_VIEW(p)); }
static ULONG WINAPI view_Release(void *p) { return vec_Release(FROM_VIEW(p)); }
static HRESULT WINAPI view_GetAt(void *p, UINT32 i, UINT64 *out) { return vec_GetAt(FROM_VIEW(p), i, out); }
static HRESULT WINAPI view_get_Size(void *p, UINT32 *n) { return vec_get_Size(FROM_VIEW(p), n); }
static HRESULT WINAPI view_IndexOf(void *p, UINT64 x, UINT32 *i, boolean *found) { return vec_IndexOf(FROM_VIEW(p), x, i, found); }
static HRESULT WINAPI view_GetMany(void *p, UINT32 start, UINT32 cap, UINT64 *items, UINT32 *n) { return vec_GetMany(FROM_VIEW(p), start, cap, items, n); }

static HRESULT WINAPI vit_QI(struct vit *it, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &vec_iids[it->vec->type][3]) || IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IInspectable_) || IsEqualGUID(iid, &IID_IAgileObject_)) *out = it;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&it->ref);
    return S_OK;
}
static ULONG WINAPI vit_AddRef(struct vit *it) { return InterlockedIncrement(&it->ref); }
static ULONG WINAPI vit_Release(struct vit *it)
{
    ULONG r = InterlockedDecrement(&it->ref);
    if (!r) { vec_Release(it->vec); free(it); }
    return r;
}
static HRESULT WINAPI vit_get_Current(struct vit *it, UINT64 *out) { return vec_GetAt(it->vec, it->pos, out); }
static HRESULT WINAPI vit_get_HasCurrent(struct vit *it, boolean *has) { *has = it->pos < it->vec->n; return S_OK; }
static HRESULT WINAPI vit_MoveNext(struct vit *it, boolean *has)
{
    if (it->pos < it->vec->n) it->pos++;
    *has = it->pos < it->vec->n;
    return S_OK;
}
static HRESULT WINAPI vit_GetMany(struct vit *it, UINT32 cap, UINT64 *items, UINT32 *n)
{
    vec_GetMany(it->vec, it->pos, cap, items, n);
    it->pos += *n;
    return S_OK;
}

static const void *vec_vtbl[] = { vec_QI, vec_AddRef, vec_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                  vec_GetAt, vec_get_Size, vec_GetView, vec_IndexOf, vec_SetAt, vec_InsertAt, vec_RemoveAt,
                                  vec_Append, vec_RemoveAtEnd, vec_Clear, vec_GetMany, vec_ReplaceAll };
static const void *view_vtbl[] = { view_QI, view_AddRef, view_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                   view_GetAt, view_get_Size, view_IndexOf, view_GetMany };
static const void *vit_vtbl[] = { vit_QI, vit_AddRef, vit_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                  vit_get_Current, vit_get_HasCurrent, vit_MoveNext, vit_GetMany };

static HRESULT WINAPI ible_QI(void *p, REFIID iid, void **out) { return vec_QI(FROM_ITERABLE(p), iid, out); }
static ULONG WINAPI ible_AddRef(void *p) { return vec_AddRef(FROM_ITERABLE(p)); }
static ULONG WINAPI ible_Release(void *p) { return vec_Release(FROM_ITERABLE(p)); }
static HRESULT WINAPI ible_First(void *p, void **out)
{
    struct vit *it = calloc(1, sizeof(*it));
    if (!it) return E_OUTOFMEMORY;
    it->vtbl = vit_vtbl; it->ref = 1; it->vec = FROM_ITERABLE(p);
    vec_AddRef(it->vec);
    *out = it;
    return S_OK;
}

static const void *ible_vtbl[] = { ible_QI, ible_AddRef, ible_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                   ible_First };

void *vec_new(int type, const void *items, UINT32 n)
{
    struct vec *v;
    if (type < VEC_U64 || type > VEC_INSPECTABLE || n > VEC_MAX || !(v = calloc(1, sizeof(*v)))) return NULL;
    v->vector_vtbl = vec_vtbl; v->view_vtbl = view_vtbl; v->iterable_vtbl = ible_vtbl;
    v->ref = 1; v->type = type;
    for (UINT32 i = 0; i < n; i++) vec_Append(v, ((const UINT64 *)items)[i]);
    return &v->vector_vtbl;
}
