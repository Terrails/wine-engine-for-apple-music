/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Completed IAsyncOperation / IAsyncAction objects */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

extern const GUID IID_Windows_Foundation_IAsyncInfo;
extern const struct iface_entry ASYNC_INFO_ENTRY;

struct async_class
{
    struct class_info ci;
    struct iface_entry ents[3];
    const struct iface_entry *op;
};

struct async_priv
{
    enum async_kind kind;
    BOOL pending;
    HRESULT status;
    IUnknown *completed;
    IUnknown *progress;
};

static struct async_class *async_classes[64];
static SRWLOCK async_lock = SRWLOCK_INIT;

static HRESULT invoke_completed(IUnknown *handler, void *op, INT32 status)
{
    HRESULT (WINAPI *fn)(IUnknown *, void *, INT32) = ((void **)*(void ***)handler)[3];
    return fn(handler, op, status);
}

static void async_free(struct obj *o)
{
    struct async_priv *p = o->priv;
    if (p->completed) IUnknown_Release(p->completed);
    if (p->progress) IUnknown_Release(p->progress);
    free(p);
}

static BOOL async_dispatch(struct obj *o, const GUID *iid, unsigned int idx, HRESULT *hr, void **args)
{
    struct async_priv *p = o->priv;
    INT32 status = p->pending ? 0 /* Started */ : SUCCEEDED(p->status) ? 1 /* Completed */ : 3 /* Error */;

    if (IsEqualGUID(iid, &IID_Windows_Foundation_IAsyncInfo))
    {
        switch (idx)
        {
        case 0: **(UINT32 **)args[0] = 1; break;                 /* get_Id */
        case 1: **(INT32 **)args[0] = status; break;             /* get_Status */
        case 2: **(HRESULT **)args[0] = p->status; break;        /* get_ErrorCode */
        default: break;                                          /* Cancel, Close */
        }
        *hr = S_OK;
        return TRUE;
    }

    /* progress variants have put_Progress/get_Progress first */
    if (p->kind == ASYNC_OP_PROGRESS || p->kind == ASYNC_ACTION_PROGRESS)
    {
        if (idx == 0)
        {
            IUnknown *h = *(IUnknown **)args[0];
            if (p->progress) IUnknown_Release(p->progress);
            if ((p->progress = h)) IUnknown_AddRef(h);
            *hr = S_OK;
            return TRUE;
        }
        if (idx == 1)
        {
            *(IUnknown **)*(void **)args[0] = p->progress;
            if (p->progress) IUnknown_AddRef(p->progress);
            *hr = S_OK;
            return TRUE;
        }
        idx -= 2;
    }

    switch (idx)
    {
    case 0: /* put_Completed */
    {
        IUnknown *h = *(IUnknown **)args[0];
        BOOL fire;
        EnterCriticalSection(&o->cs);
        if (p->completed) { LeaveCriticalSection(&o->cs); *hr = E_ILLEGAL_DELEGATE_ASSIGNMENT; return TRUE; }
        if ((p->completed = h)) IUnknown_AddRef(h);
        fire = h && !p->pending;
        LeaveCriticalSection(&o->cs);
        if (fire) invoke_completed(h, &o->ifcs[0], status);
        *hr = S_OK;
        return TRUE;
    }
    case 1: /* get_Completed */
        *(IUnknown **)*(void **)args[0] = p->completed;
        if (p->completed) IUnknown_AddRef(p->completed);
        *hr = S_OK;
        return TRUE;
    case 2: /* GetResults */
        if (p->pending) { *hr = E_ILLEGAL_METHOD_CALL; return TRUE; }
        if (FAILED(p->status)) { *hr = p->status; return TRUE; }
        if (p->kind == ASYNC_OP || p->kind == ASYNC_OP_PROGRESS)
        {
            struct prop *pr = prop_find(o, L"Result");
            *hr = prop_get(o, L"Result", pr ? pr->kind : PK_OBJECT, *(void **)args[0],
                           pr ? pr->size : sizeof(void *), NULL);
        }
        else *hr = S_OK;
        return TRUE;
    }
    return FALSE;
}

static const struct class_info *async_class_for(const struct iface_entry *op)
{
    struct async_class *c;
    unsigned int i;

    AcquireSRWLockExclusive(&async_lock);
    for (i = 0; i < ARRAY_SIZE(async_classes) && async_classes[i]; i++)
        if (async_classes[i]->op == op) break;
    if (i < ARRAY_SIZE(async_classes) && !async_classes[i])
    {
        c = calloc(1, sizeof(*c));
        c->op = op;
        c->ents[0] = *op;
        c->ents[1] = ASYNC_INFO_ENTRY;
        c->ci.name = L"Windows.Foundation.IAsyncOperation";
        c->ci.ifaces = c->ents;
        c->ci.dispatch = async_dispatch;
        async_classes[i] = c;
    }
    ReleaseSRWLockExclusive(&async_lock);
    return i < ARRAY_SIZE(async_classes) ? &async_classes[i]->ci : NULL;
}

HRESULT async_create(const struct iface_entry *op, enum async_kind kind, HRESULT status,
                     enum prop_kind rk, const void *value, unsigned int size, void **out)
{
    const struct class_info *cls = async_class_for(op);
    struct async_priv *p;
    IInspectable *inst;
    struct obj *o;
    HRESULT hr;

    if (!cls) return E_OUTOFMEMORY;
    if (FAILED(hr = create_object(cls, &inst))) return hr;
    o = impl_from_inspectable(inst);
    p = calloc(1, sizeof(*p));
    p->kind = kind;
    p->status = status;
    o->priv = p;
    o->priv_free = async_free;
    if (value) prop_put(o, L"Result", rk, value, size);
    TRACE("%s status %#lx\n", op->name, status);
    *out = inst;
    return S_OK;
}

HRESULT async_create_pending(const struct iface_entry *op, enum async_kind kind, struct obj **o, void **out)
{
    HRESULT hr = async_create(op, kind, S_OK, PK_PRIM, NULL, 0, out);
    if (FAILED(hr)) return hr;
    *o = impl_from_inspectable(*out);
    ((struct async_priv *)(*o)->priv)->pending = TRUE;
    IInspectable_AddRef((IInspectable *)*out);   /* reference held until completion */
    return S_OK;
}

void async_complete(struct obj *o, HRESULT status, enum prop_kind rk, const void *value, unsigned int size)
{
    struct async_priv *p = o->priv;
    IUnknown *h;
    if (value) prop_put(o, L"Result", rk, value, size);
    EnterCriticalSection(&o->cs);
    p->status = status;
    p->pending = FALSE;
    h = p->completed;
    if (h) IUnknown_AddRef(h);
    LeaveCriticalSection(&o->cs);
    TRACE("%p status %#lx handler %p\n", o, status, h);
    if (h)
    {
        invoke_completed(h, &o->ifcs[0], SUCCEEDED(status) ? 1 : 3);
        IUnknown_Release(h);
    }
    IInspectable_Release((IInspectable *)&o->ifcs[0]);
}

HRESULT async_wait(IUnknown *op)
{
    IUnknown *info;
    INT32 status = 0;
    HRESULT err = S_OK;
    HRESULT hr = IUnknown_QueryInterface(op, &IID_Windows_Foundation_IAsyncInfo, (void **)&info);
    if (FAILED(hr)) return hr;
    for (;;)
    {
        /* IAsyncInfo: [7] get_Status, [8] get_ErrorCode */
        ((HRESULT (WINAPI *)(IUnknown *, INT32 *))((void **)*(void ***)info)[7])(info, &status);
        if (status) break;
        Sleep(1);
    }
    if (status != 1) ((HRESULT (WINAPI *)(IUnknown *, HRESULT *))((void **)*(void ***)info)[8])(info, &err);
    IUnknown_Release(info);
    return status == 1 ? S_OK : FAILED(err) ? err : E_ABORT;
}
