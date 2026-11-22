/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: DispatcherQueue{,Controller,Timer}, for both Windows.System and
 * Microsoft.UI.Dispatching (same shape; overrides are matched by interface short name, so
 * one implementation serves both; a thread can have one queue of each flavour).
 * Each queue owns a message-only window on its thread; work items and timers
 * are delivered through that window's message loop. */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

#define WM_DQ_INVOKE     (WM_APP + 0x51)
#define WM_DQ_TIMER      (WM_APP + 0x52)
#define WM_DQ_SHUTDOWN   (WM_APP + 0x53)

struct dqueue
{
    DWORD tid;
    HWND hwnd;
    HANDLE thread;
    struct obj *self;
};

struct dtimer
{
    struct obj *queue;     /* DispatcherQueue object (ref held) */
    IUnknown *tick[8];
    INT64 interval;        /* 100ns */
    BOOL repeating, running;
};

/* [0] Windows.System, [1] Microsoft.UI.Dispatching */
static DWORD tls_index[2] = { TLS_OUT_OF_INDEXES, TLS_OUT_OF_INDEXES };
static const WCHAR *const queue_class[2] = { L"Windows.System.DispatcherQueue", L"Microsoft.UI.Dispatching.DispatcherQueue" };
static const WCHAR *const ctrl_class[2] = { L"Windows.System.DispatcherQueueController", L"Microsoft.UI.Dispatching.DispatcherQueueController" };
static const WCHAR *const timer_class[2] = { L"Windows.System.DispatcherQueueTimer", L"Microsoft.UI.Dispatching.DispatcherQueueTimer" };

static int is_ms(struct obj *o) { return o->cls->name[0] == 'M'; }

static HRESULT invoke0(IUnknown *d)
{
    HRESULT (WINAPI *fn)(IUnknown *) = ((void **)*(void ***)d)[3];
    return fn(d);
}

static HRESULT invoke2(IUnknown *d, void *a, void *b)
{
    HRESULT (WINAPI *fn)(IUnknown *, void *, void *) = ((void **)*(void ***)d)[3];
    return fn(d, a, b);
}

static void timer_tick(struct obj *t);

static LRESULT CALLBACK dq_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DQ_INVOKE:
    {
        IUnknown *h = (IUnknown *)lp;
        HRESULT hr = invoke0(h);
        if (FAILED(hr)) WARN("handler %p failed %#lx\n", h, hr);
        IUnknown_Release(h);
        return 0;
    }
    case WM_DQ_TIMER:
    {
        struct obj *t = (struct obj *)lp;
        struct dtimer *dt = t->priv;
        if (wp) SetTimer(hwnd, (UINT_PTR)t, (UINT)max(dt->interval / 10000, 1), NULL);
        else
        {
            KillTimer(hwnd, (UINT_PTR)t);
            IInspectable_Release((IInspectable *)&t->ifcs[0]);   /* running reference */
        }
        IInspectable_Release((IInspectable *)&t->ifcs[0]);       /* message reference */
        return 0;
    }
    case WM_TIMER:
        timer_tick((struct obj *)wp);
        return 0;
    case WM_DQ_SHUTDOWN:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND create_queue_window(void)
{
    static ATOM atom;
    if (!atom)
    {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = dq_wndproc;
        wc.hInstance = GetModuleHandleW(L"amshim.dll");
        wc.lpszClassName = L"amshim_DispatcherQueue";
        atom = RegisterClassW(&wc);
    }
    return CreateWindowW(L"amshim_DispatcherQueue", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                         GetModuleHandleW(L"amshim.dll"), NULL);
}

static void dq_free(struct obj *o)
{
    struct dqueue *q = o->priv;
    if (q->hwnd && q->tid == GetCurrentThreadId()) DestroyWindow(q->hwnd);
    if (q->thread) CloseHandle(q->thread);
    free(q);
}

/* Create a queue bound to the current thread. */
static HRESULT queue_create_here(int ms, struct obj **out)
{
    IInspectable *inst;
    struct dqueue *q;
    HRESULT hr;

    if (tls_index[ms] == TLS_OUT_OF_INDEXES) tls_index[ms] = TlsAlloc();
    if (FAILED(hr = create_object_by_name(queue_class[ms], &inst))) return hr;
    q = calloc(1, sizeof(*q));
    q->tid = GetCurrentThreadId();
    q->hwnd = create_queue_window();
    q->self = impl_from_inspectable(inst);
    q->self->priv = q;
    q->self->priv_free = dq_free;
    SetWindowLongPtrW(q->hwnd, GWLP_USERDATA, (LONG_PTR)q->self);
    TlsSetValue(tls_index[ms], q->self);
    TRACE("queue %p thread %04lx hwnd %p\n", q->self, q->tid, q->hwnd);
    *out = q->self;
    return S_OK;
}

static HRESULT controller_wrap(struct obj *queue, IInspectable **out)
{
    IInspectable *ctrl, *qi = (IInspectable *)&queue->ifcs[0];
    HRESULT hr = create_object_by_name(ctrl_class[is_ms(queue)], &ctrl);
    if (FAILED(hr)) return hr;
    prop_put(impl_from_inspectable(ctrl), L"DispatcherQueue", PK_OBJECT, &qi, sizeof(qi));
    *out = ctrl;
    return S_OK;
}

struct thread_start { HANDLE ready; struct obj *queue; int apartment; int ms; };

static DWORD WINAPI dedicated_thread(void *arg)
{
    struct thread_start *ts = arg;
    MSG msg;

    if (ts->apartment) CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    queue_create_here(ts->ms, &ts->queue);
    SetEvent(ts->ready);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    TRACE("dedicated queue thread exiting\n");
    if (ts->apartment) CoUninitialize();
    free(ts);
    return 0;
}

static HRESULT create_dedicated(int apartment, int ms, IInspectable **controller)
{
    struct thread_start *ts = calloc(1, sizeof(*ts));
    struct dqueue *q;
    HANDLE thread;

    ts->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    ts->apartment = apartment;
    ts->ms = ms;
    thread = CreateThread(NULL, 0, dedicated_thread, ts, 0, NULL);
    WaitForSingleObject(ts->ready, INFINITE);
    CloseHandle(ts->ready);
    q = ts->queue->priv;
    q->thread = thread;
    /* the controller keeps the queue alive */
    return controller_wrap(ts->queue, controller);
}

/* ---- DispatcherQueue ------------------------------------------------ */

HRESULT OV_IDispatcherQueueStatics_GetForCurrentThread(struct obj *o, void **result)
{
    int ms = is_ms(o);
    struct obj *q = tls_index[ms] != TLS_OUT_OF_INDEXES ? TlsGetValue(tls_index[ms]) : NULL;
    *result = NULL;
    if (q)
    {
        *result = &q->ifcs[0];
        IInspectable_AddRef((IInspectable *)*result);
    }
    TRACE("-> %p\n", *result);
    return S_OK;
}

HRESULT OV_IDispatcherQueue_TryEnqueue(struct obj *o, void *callback, boolean *result)
{
    struct dqueue *q = o->priv;
    IUnknown_AddRef((IUnknown *)callback);
    *result = PostMessageW(q->hwnd, WM_DQ_INVOKE, 0, (LPARAM)callback);
    if (!*result) IUnknown_Release((IUnknown *)callback);
    return S_OK;
}

HRESULT OV_IDispatcherQueue_TryEnqueue_2(struct obj *o, INT32 priority, void *callback, boolean *result)
{
    return OV_IDispatcherQueue_TryEnqueue(o, callback, result);
}

HRESULT OV_IDispatcherQueue2_get_HasThreadAccess(struct obj *o, boolean *result)
{
    struct dqueue *q = o->priv;
    *result = q->tid == GetCurrentThreadId();
    return S_OK;
}

HRESULT OV_IDispatcherQueue_CreateTimer(struct obj *o, void **result)
{
    IInspectable *inst;
    struct dtimer *t;
    HRESULT hr = create_object_by_name(timer_class[is_ms(o)], &inst);
    if (FAILED(hr)) return hr;
    t = calloc(1, sizeof(*t));
    t->queue = o;
    t->repeating = TRUE;
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);
    impl_from_inspectable(inst)->priv = t;
    *result = inst;
    return S_OK;
}

/* ---- DispatcherQueueController --------------------------------------- */

HRESULT OV_IDispatcherQueueControllerStatics_CreateOnDedicatedThread(struct obj *o, void **result)
{
    return create_dedicated(1, is_ms(o), (IInspectable **)result);
}

/* Microsoft.UI.Dispatching only: what WinUI's Application.Start does for the UI thread */
HRESULT OV_IDispatcherQueueControllerStatics_CreateOnCurrentThread(struct obj *o, void **result)
{
    struct obj *q;
    HRESULT hr = queue_create_here(is_ms(o), &q);
    if (FAILED(hr)) return hr;
    hr = controller_wrap(q, (IInspectable **)result);
    IInspectable_Release((IInspectable *)&q->ifcs[0]);
    return hr;
}

HRESULT OV_IDispatcherQueueController_ShutdownQueueAsync(struct obj *o, void **result)
{
    IInspectable *qi;
    prop_get(o, L"DispatcherQueue", PK_OBJECT, &qi, sizeof(qi), NULL);
    if (qi)
    {
        struct dqueue *q = impl_from_inspectable(qi)->priv;
        if (q->thread) PostMessageW(q->hwnd, WM_DQ_SHUTDOWN, 0, 0);
        IInspectable_Release(qi);
    }
    return async_create(&ASYNC_IAsyncAction, ASYNC_ACTION, S_OK, PK_PRIM, NULL, 0, result);
}

/* DISPATCHERQUEUE_THREAD_TYPE: 1 dedicated, 2 current; apartment: 0 none, 1 ASTA, 2 STA */
typedef struct { DWORD dwSize; DWORD threadType; DWORD apartmentType; } DQ_OPTIONS;

HRESULT WINAPI CreateDispatcherQueueController(DQ_OPTIONS options, void **controller)
{
    IInspectable *ctrl;
    struct obj *q;
    HRESULT hr;

    TRACE("type %lu apartment %lu\n", options.threadType, options.apartmentType);
    if (options.threadType == 1) hr = create_dedicated(options.apartmentType, 0, &ctrl);
    else
    {
        if (FAILED(hr = queue_create_here(0, &q))) return hr;
        hr = controller_wrap(q, &ctrl);
        IInspectable_Release((IInspectable *)&q->ifcs[0]);
    }
    if (FAILED(hr)) return hr;
    hr = IInspectable_QueryInterface(ctrl, &IID_Windows_System_IDispatcherQueueController, controller);
    IInspectable_Release(ctrl);
    return hr;
}

/* ---- DispatcherQueueTimer -------------------------------------------- */

static void timer_tick(struct obj *t)
{
    struct dtimer *dt = t->priv;
    IUnknown *handlers[8];
    unsigned int i, n = 0;

    EnterCriticalSection(&t->cs);
    for (i = 0; i < ARRAY_SIZE(dt->tick); i++)
        if (dt->tick[i]) { handlers[n] = dt->tick[i]; IUnknown_AddRef(handlers[n]); n++; }
    if (!dt->repeating)
    {
        struct dqueue *q = dt->queue->priv;
        dt->running = FALSE;
        KillTimer(q->hwnd, (UINT_PTR)t);
    }
    LeaveCriticalSection(&t->cs);
    IInspectable_AddRef((IInspectable *)&t->ifcs[0]);
    for (i = 0; i < n; i++)
    {
        invoke2(handlers[i], &t->ifcs[0], NULL);
        IUnknown_Release(handlers[i]);
    }
    if (!dt->repeating) IInspectable_Release((IInspectable *)&t->ifcs[0]); /* running reference */
    IInspectable_Release((IInspectable *)&t->ifcs[0]);
}

HRESULT OV_IDispatcherQueueTimer_get_Interval(struct obj *o, S_Windows_Foundation_TimeSpan *result)
{
    result->Duration = ((struct dtimer *)o->priv)->interval;
    return S_OK;
}

HRESULT OV_IDispatcherQueueTimer_put_Interval(struct obj *o, S_Windows_Foundation_TimeSpan value)
{
    ((struct dtimer *)o->priv)->interval = value.Duration;
    return S_OK;
}

HRESULT OV_IDispatcherQueueTimer_get_IsRunning(struct obj *o, boolean *result)
{
    *result = ((struct dtimer *)o->priv)->running;
    return S_OK;
}

HRESULT OV_IDispatcherQueueTimer_get_IsRepeating(struct obj *o, boolean *result)
{
    *result = ((struct dtimer *)o->priv)->repeating;
    return S_OK;
}

HRESULT OV_IDispatcherQueueTimer_put_IsRepeating(struct obj *o, boolean value)
{
    ((struct dtimer *)o->priv)->repeating = value;
    return S_OK;
}

HRESULT OV_IDispatcherQueueTimer_Start(struct obj *o)
{
    struct dtimer *dt = o->priv;
    struct dqueue *q = dt->queue->priv;
    EnterCriticalSection(&o->cs);
    if (!dt->running)
    {
        dt->running = TRUE;
        IInspectable_AddRef((IInspectable *)&o->ifcs[0]);   /* running reference */
    }
    LeaveCriticalSection(&o->cs);
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);       /* message reference */
    PostMessageW(q->hwnd, WM_DQ_TIMER, 1, (LPARAM)o);
    return S_OK;
}

HRESULT OV_IDispatcherQueueTimer_Stop(struct obj *o)
{
    struct dtimer *dt = o->priv;
    struct dqueue *q = dt->queue->priv;
    BOOL was;
    EnterCriticalSection(&o->cs);
    was = dt->running;
    dt->running = FALSE;
    LeaveCriticalSection(&o->cs);
    if (!was) return S_OK;
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);
    PostMessageW(q->hwnd, WM_DQ_TIMER, 0, (LPARAM)o);
    return S_OK;
}

HRESULT OV_IDispatcherQueueTimer_add_Tick(struct obj *o, void *handler, S_Windows_Foundation_EventRegistrationToken *result)
{
    struct dtimer *dt = o->priv;
    unsigned int i;
    EnterCriticalSection(&o->cs);
    for (i = 0; i < ARRAY_SIZE(dt->tick); i++) if (!dt->tick[i]) break;
    if (i < ARRAY_SIZE(dt->tick))
    {
        dt->tick[i] = handler;
        IUnknown_AddRef((IUnknown *)handler);
    }
    LeaveCriticalSection(&o->cs);
    result->Value = i + 1;
    return i < ARRAY_SIZE(dt->tick) ? S_OK : E_OUTOFMEMORY;
}

HRESULT OV_IDispatcherQueueTimer_remove_Tick(struct obj *o, S_Windows_Foundation_EventRegistrationToken token)
{
    struct dtimer *dt = o->priv;
    INT64 i = token.Value - 1;
    EnterCriticalSection(&o->cs);
    if (i >= 0 && i < ARRAY_SIZE(dt->tick) && dt->tick[i])
    {
        IUnknown_Release(dt->tick[i]);
        dt->tick[i] = NULL;
    }
    LeaveCriticalSection(&o->cs);
    return S_OK;
}
