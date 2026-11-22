/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Minimal async operation/action objects (see async.h). */
#include "async.h"

static HRESULT WINAPI aop_QI(struct aop *a, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IAsyncInfo_)) *out = &a->info_vtbl;
    else if (IsEqualGUID(iid, &IID_IUnknown) || !is_classic_iid(iid)) *out = &a->op_vtbl;  /* any pinterface IID */
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&a->ref);
    return S_OK;
}
static ULONG WINAPI aop_AddRef(struct aop *a) { return InterlockedIncrement(&a->ref); }
static ULONG WINAPI aop_Release(struct aop *a) { return InterlockedDecrement(&a->ref); }   /* never freed */

static void aop_fire(struct aop *a)
{
    IUnknown *h = a->handler;
    if (!h || a->status == 0) return;
    a->handler = NULL;
    /* AsyncOperationCompletedHandler<T>::Invoke(asyncInfo, status) */
    ((HRESULT (WINAPI *)(void *, void *, int))VT(h)[3])(h, &a->op_vtbl, a->status);
    IUnknown_Release(h);
}
static HRESULT WINAPI aop_put_Completed(struct aop *a, IUnknown *h)
{
    if (h) IUnknown_AddRef(h);
    a->handler = h;
    aop_fire(a);
    return S_OK;
}
static HRESULT WINAPI aop_get_Completed(struct aop *a, IUnknown **h) { *h = NULL; return S_OK; }
static HRESULT WINAPI aop_GetResults(struct aop *a, void *res) { if (res && a->rsize) memcpy(res, a->result, a->rsize); return S_OK; }
static HRESULT WINAPI aop_GetResults0(struct aop *a) { return S_OK; }

/* IAsyncInfo tear-off: one pointer after op_vtbl */
#define INFO(p) ((struct aop *)((char *)(p) - sizeof(void *)))
static HRESULT WINAPI ai_QI(void *p, REFIID iid, void **out) { return aop_QI(INFO(p), iid, out); }
static ULONG WINAPI ai_AddRef(void *p) { return aop_AddRef(INFO(p)); }
static ULONG WINAPI ai_Release(void *p) { return aop_Release(INFO(p)); }
static HRESULT WINAPI ai_get_Id(void *p, UINT32 *id) { *id = 1; return S_OK; }
static HRESULT WINAPI ai_get_Status(void *p, INT32 *st) { *st = INFO(p)->status; return S_OK; }
static HRESULT WINAPI ai_get_ErrorCode(void *p, HRESULT *hr) { *hr = S_OK; return S_OK; }
static HRESULT WINAPI ai_Cancel(void *p) { return S_OK; }
static HRESULT WINAPI ai_Close(void *p) { return S_OK; }

static const void *aop_op_vtbl[] = { aop_QI, aop_AddRef, aop_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                     aop_put_Completed, aop_get_Completed, aop_GetResults };
static const void *aop_act_vtbl[] = { aop_QI, aop_AddRef, aop_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                      aop_put_Completed, aop_get_Completed, aop_GetResults0 };
static const void *aop_info_vtbl[] = { ai_QI, ai_AddRef, ai_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                       ai_get_Id, ai_get_Status, ai_get_ErrorCode, ai_Cancel, ai_Close };

struct aop *aop_new(int action, int completed, unsigned int rsize)
{
    struct aop *a = calloc(1, sizeof(*a));
    a->op_vtbl = action ? aop_act_vtbl : aop_op_vtbl;
    a->info_vtbl = aop_info_vtbl;
    a->ref = 1;
    a->status = completed ? 1 : 0;
    a->rsize = rsize;
    return a;
}

void aop_complete(struct aop *a, const void *result, unsigned int size)
{
    if (result && size <= sizeof(a->result)) memcpy(a->result, result, size);
    a->status = 1;
    aop_fire(a);
}
