/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: Windows.System.Threading.ThreadPoolTimer */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* Delegate Invoke is slot 3 (after IUnknown). */
static HRESULT invoke_delegate1(IUnknown *d, void *arg)
{
    HRESULT (WINAPI *fn)(IUnknown *, void *) = ((void **)*(void ***)d)[3];
    return fn(d, arg);
}

struct timer
{
    PTP_TIMER tp;
    IUnknown *elapsed, *destroyed;
    INT64 period, delay;      /* 100ns units */
    BOOL periodic, cancelled, scheduled;
};

static void CALLBACK timer_cb(PTP_CALLBACK_INSTANCE inst, void *ctx, PTP_TIMER tp)
{
    struct obj *o = ctx;
    struct timer *t = o->priv;
    IInspectable *self = (IInspectable *)&o->ifcs[0];

    if (t->cancelled) return;
    TRACE("timer %p fired\n", o);
    invoke_delegate1(t->elapsed, self);
    if (!t->periodic)
    {
        if (t->destroyed) invoke_delegate1(t->destroyed, self);
        EnterCriticalSection(&o->cs);
        if (t->scheduled) { t->scheduled = FALSE; LeaveCriticalSection(&o->cs); IInspectable_Release(self); }
        else LeaveCriticalSection(&o->cs);
    }
}

static void timer_free(struct obj *o)
{
    struct timer *t = o->priv;
    if (!t) return;
    if (t->tp)
    {
        SetThreadpoolTimer(t->tp, NULL, 0, 0);
        CloseThreadpoolTimer(t->tp);
    }
    if (t->elapsed) IUnknown_Release(t->elapsed);
    if (t->destroyed) IUnknown_Release(t->destroyed);
    free(t);
}

static HRESULT timer_create(void *handler, INT64 span, BOOL periodic, void *destroyed, void **result)
{
    IInspectable *inst;
    struct obj *o;
    struct timer *t;
    FILETIME due;
    LARGE_INTEGER li;
    HRESULT hr;

    if (!handler) return E_INVALIDARG;
    if (FAILED(hr = create_object_by_name(L"Windows.System.Threading.ThreadPoolTimer", &inst))) return hr;
    o = impl_from_inspectable(inst);
    t = calloc(1, sizeof(*t));
    o->priv = t;
    o->priv_free = timer_free;
    t->elapsed = handler; IUnknown_AddRef(t->elapsed);
    if (destroyed) { t->destroyed = destroyed; IUnknown_AddRef(t->destroyed); }
    t->periodic = periodic;
    if (periodic) t->period = span; else t->delay = span;
    prop_put(o, L"Period", PK_STRUCT, &t->period, 8);
    prop_put(o, L"Delay", PK_STRUCT, &t->delay, 8);

    t->tp = CreateThreadpoolTimer(timer_cb, o, NULL);
    if (!t->tp) { IInspectable_Release(inst); return HRESULT_FROM_WIN32(GetLastError()); }

    /* the system holds a reference while the timer is scheduled */
    IInspectable_AddRef(inst);
    t->scheduled = TRUE;
    li.QuadPart = -(span > 0 ? span : 1);
    due.dwLowDateTime = li.LowPart;
    due.dwHighDateTime = li.HighPart;
    SetThreadpoolTimer(t->tp, &due, periodic ? (DWORD)max(span / 10000, 1) : 0, 0);
    TRACE("timer %p span %I64d periodic %d\n", o, span, periodic);
    *result = inst;
    return S_OK;
}

HRESULT OV_IThreadPoolTimerStatics_CreatePeriodicTimer(struct obj *o, void *handler, S_Windows_Foundation_TimeSpan period, void **result)
{
    return timer_create(handler, period.Duration, TRUE, NULL, result);
}

HRESULT OV_IThreadPoolTimerStatics_CreateTimer(struct obj *o, void *handler, S_Windows_Foundation_TimeSpan delay, void **result)
{
    return timer_create(handler, delay.Duration, FALSE, NULL, result);
}

HRESULT OV_IThreadPoolTimerStatics_CreatePeriodicTimer_2(struct obj *o, void *handler, S_Windows_Foundation_TimeSpan period, void *destroyed, void **result)
{
    return timer_create(handler, period.Duration, TRUE, destroyed, result);
}

HRESULT OV_IThreadPoolTimerStatics_CreateTimer_2(struct obj *o, void *handler, S_Windows_Foundation_TimeSpan delay, void *destroyed, void **result)
{
    return timer_create(handler, delay.Duration, FALSE, destroyed, result);
}

HRESULT OV_IThreadPoolTimer_Cancel(struct obj *o)
{
    struct timer *t = o->priv;
    BOOL release = FALSE;

    if (!t) return S_OK;
    TRACE("timer %p\n", o);
    EnterCriticalSection(&o->cs);
    t->cancelled = TRUE;
    if (t->tp) SetThreadpoolTimer(t->tp, NULL, 0, 0);
    if (t->scheduled) { t->scheduled = FALSE; release = TRUE; }
    LeaveCriticalSection(&o->cs);
    if (t->destroyed && release) invoke_delegate1(t->destroyed, &o->ifcs[0]);
    if (release) IInspectable_Release((IInspectable *)&o->ifcs[0]);
    return S_OK;
}
