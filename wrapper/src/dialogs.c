/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* IDialogServiceImpl: how AMP.Services asks the UI for dialogs (see dialogs.h).
 *
 * Requests that need an answer (sign-in, message boxes) are queued and shown from the message
 * loop (dialogs_process), never from inside the app's call. Other dialogs are only reported;
 * their operations stay pending ("open") because nobody answers them here. */
#include "dialogs.h"
#include "async.h"
#include "page.h"
#include "prompt.h"
#include "signin.h"

static const GUID IID_IDialogService = {0x4e87acf0,0x63b8,0x51b5,{0xb1,0xa4,0x0b,0xc3,0x92,0xf3,0x65,0x7d}};
static const GUID IID_IDialogServiceImpl = {0xbbdd9570,0xd1a1,0x5767,{0x89,0x22,0xa2,0x27,0x40,0x5d,0xc5,0x9f}};
static const GUID IID_IGDPRWelcomeDialogViewModel = {0xb72fccc1,0x010e,0x5010,{0xbe,0xd4,0x8d,0xe2,0x38,0x37,0x13,0xdd}};

/* ---------------- first-run welcome notice ---------------- */

static void *welcome_vm;   /* GDPRWelcomeDialogViewModel of the open welcome dialog */

/* log the notice's text; the view model is kept so that it can be answered later */
static int report_welcome(IInspectable *obj)
{
    static const char *const names[] = { "Title", "Subtitle", "Explanation", "SeeMoreTitle", NULL, "AcceptTitle" };
    void *vm = NULL;
    if (!obj || FAILED(IInspectable_QueryInterface(obj, &IID_IGDPRWelcomeDialogViewModel, &vm))) return 0;
    for (int i = 0; i < 6; i++)
    {
        HSTRING h = NULL;
        if (!names[i]) continue;
        ((HRESULT (WINAPI *)(void *, HSTRING *))VT(vm)[6 + i])(vm, &h);
        LOG("WELCOME %s: %s", names[i], ws(h));
    }
    welcome_vm = vm;
    return 1;
}

int welcome_pending(void) { return welcome_vm != NULL; }

HRESULT welcome_accept(void)
{
    IInspectable *action = NULL; HSTRING cls = NULL; HRESULT hr;
    if (!welcome_vm) return E_UNEXPECTED;
    hr = CALL1(welcome_vm, 12, &action);    /* AcceptAction */
    if (action) IInspectable_GetRuntimeClassName(action, &cls);
    LOG("AcceptAction -> %08lx %p class='%s'", hr, action, ws(cls));
    if (!action) return FAILED(hr) ? hr : E_FAIL;
    hr = dispatch_action(action);
    say("dispatch AcceptAction", hr);
    return hr;
}

/* ---------------- NotificationCenter subscriber ----------------
 * EventHandler<NotificationArgs>: how AMP.Services asks the UI to do things; the event kind is
 * a four-character code */
static HRESULT WINAPI nh_QI(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IAgileObject_) || !is_classic_iid(iid)) { *out = This; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI static_AddRef(void *This) { return 2; }
static ULONG WINAPI static_Release(void *This) { return 1; }
static HRESULT WINAPI nh_Invoke(void *This, IInspectable *sender, void *args)
{
    UINT32 kind = 0; IInspectable *param = NULL; HSTRING cls = NULL; char cc[5];
    ((HRESULT (WINAPI *)(void *, UINT32 *))VT(args)[6])(args, &kind);
    ((HRESULT (WINAPI *)(void *, IInspectable **))VT(args)[7])(args, &param);
    cc[0] = kind >> 24; cc[1] = kind >> 16; cc[2] = kind >> 8; cc[3] = kind; cc[4] = 0;
    for (int i = 0; i < 4; i++) if (cc[i] < 32 || cc[i] > 126) cc[i] = '?';
    if (param) IInspectable_GetRuntimeClassName(param, &cls);
    LOG("NOTIFICATION '%s' (0x%08x) parameter class='%s'", cc, kind, ws(cls));
    report_welcome(param);
    return S_OK;
}
static const void *nh_vtbl[] = { nh_QI, static_AddRef, static_Release, nh_Invoke };
static const void **notification_handler = nh_vtbl;

void notifications_subscribe(void)
{
    for (int slot = 32; slot <= 33; slot++)    /* NotificationCenter, PageNotificationCenter */
    {
        void *nc = NULL; INT64 token = 0; HRESULT hr;
        hr = CALL1(g_sp, slot, &nc);
        if (nc) hr = ((HRESULT (WINAPI *)(void *, void *, INT64 *))VT(nc)[7])(nc, &notification_handler, &token);  /* add_Notification */
        LOG("subscribe notification center (slot %d) -> %08lx", slot, hr);
    }
}

/* ---------------- request queue (drained on the main thread) ---------------- */

struct uireq
{
    struct uireq *next;
    int kind;                 /* 1 sign-in, 2 message box */
    HSTRING s[6];
    IUnknown *handler;        /* SignInHandler */
    struct aop *op;
    INT32 def;
};
static struct uireq *uiq_head, *uiq_tail;
static CRITICAL_SECTION uiq_cs;
static CRITICAL_SECTION_DEBUG uiq_cs_debug = { 0, 0, &uiq_cs, { &uiq_cs_debug.ProcessLocksList, &uiq_cs_debug.ProcessLocksList }, 0, 0, { 0 } };
static CRITICAL_SECTION uiq_cs = { &uiq_cs_debug, -1, 0, 0, 0, 0 };

static void uiq_push(struct uireq *r)
{
    EnterCriticalSection(&uiq_cs);
    if (uiq_tail) uiq_tail->next = r; else uiq_head = r;
    uiq_tail = r;
    LeaveCriticalSection(&uiq_cs);
}

/* message box: DialogResult { DialogButton (1 Primary, 2 Secondary, 3 Other), checkbox } */
static void handle_message(struct uireq *r)
{
    struct prompt p = {0};
    struct { INT32 button; boolean checkbox; } res = { 0, FALSE };
    INT32 map[3];
    p.message = wz(r->s[0]); p.explanation = wz(r->s[1]);
    for (int i = 0; i < 3; i++)
        if (WindowsGetStringLen(r->s[2 + i]))
        {
            map[p.n_buttons] = i + 1;
            p.buttons[p.n_buttons] = wz(r->s[2 + i]);
            if (r->def == i + 1) p.default_button = p.n_buttons;
            p.n_buttons++;
        }
    if (!p.n_buttons) { p.buttons[0] = L"OK"; map[0] = 1; p.n_buttons = 1; }
    res.button = map[prompt_run(&p)];
    LOG("message dialog answered with DialogButton=%d", res.button);
    aop_complete(r->op, &res, sizeof(res));
}

static void handle_auth(struct uireq *r)
{
    /* AuthResult: 0 Ok, 1 Canceled, 2 CreateAccount */
    INT32 auth_result = signin_dialog(r->handler, r->s[0], r->s[1], r->s[2], r->s[3]);
    aop_complete(r->op, &auth_result, sizeof(auth_result));
    LOG("auth dialog closed with AuthResult=%d", auth_result);
}

void dialogs_process(void)
{
    for (;;)
    {
        struct uireq *r;
        EnterCriticalSection(&uiq_cs);
        if ((r = uiq_head)) { uiq_head = r->next; if (!uiq_head) uiq_tail = NULL; }
        LeaveCriticalSection(&uiq_cs);
        if (!r) return;
        if (r->kind == 1) handle_auth(r); else handle_message(r);
        for (int i = 0; i < 6; i++) WindowsDeleteString(r->s[i]);
        if (r->handler) IUnknown_Release(r->handler);
        free(r);
    }
}

/* ---------------- IDialogServiceImpl ----------------
 * DialogService keeps only a weak reference to its implementation (and assumes it can get one),
 * so the object also answers IWeakReferenceSource. All three are static singletons. */
static HRESULT WINAPI dlg_QI(void *This, REFIID iid, void **out);
static const void **dialog_weak_src, **dialog_weak_ref, **dialog_impl;

static HRESULT WINAPI dlgw_QI(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IWeakReference_) || (This == &dialog_weak_ref && IsEqualGUID(iid, &IID_IUnknown))) { *out = &dialog_weak_ref; return S_OK; }
    return dlg_QI(&dialog_impl, iid, out);
}
static HRESULT WINAPI dlgw_GetWeakReference(void *This, void **ref) { *ref = &dialog_weak_ref; return S_OK; }
static HRESULT WINAPI dlgw_Resolve(void *This, REFIID iid, void **out) { return dlg_QI(&dialog_impl, iid, out); }
static const void *dlg_weak_src_vtbl[] = { dlgw_QI, static_AddRef, static_Release, dlgw_GetWeakReference };
static const void *dlg_weak_ref_vtbl[] = { dlgw_QI, static_AddRef, static_Release, dlgw_Resolve };
static const void **dialog_weak_src = dlg_weak_src_vtbl, **dialog_weak_ref = dlg_weak_ref_vtbl;

static HRESULT WINAPI dlg_QI(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IWeakReferenceSource_)) { *out = &dialog_weak_src; return S_OK; }
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable_) || IsEqualGUID(iid, &IID_IAgileObject_) ||
        IsEqualGUID(iid, &IID_IDialogServiceImpl)) { *out = &dialog_impl; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static HRESULT WINAPI dlg_ShowInformation(void *This, HSTRING title, HSTRING message)
{
    LOG("DIALOG info: title='%s' message='%s'", ws(title), ws(message));
    return S_OK;
}
static HRESULT WINAPI dlg_ShowDialogAsync(void *This, IInspectable *dialog, void **result)
{
    HSTRING cls = NULL; struct aop *a = aop_new(0, 0, 8);
    if (dialog) IInspectable_GetRuntimeClassName(dialog, &cls);
    LOG("DIALOG object: class='%s'", ws(cls));
    report_welcome(dialog);
    *result = aop_iface(a);
    return S_OK;
}
static HRESULT WINAPI dlg_ShowDialogAsync2(void *This, HSTRING message, HSTRING explanation, HSTRING primary, HSTRING secondary,
                                           HSTRING other, HSTRING checkbox, INT32 def, boolean checkbox_init, void **result)
{
    struct uireq *r = calloc(1, sizeof(*r));
    LOG("DIALOG message='%s' explanation='%s' buttons=['%s','%s','%s'] checkbox='%s' default=%d",
        ws(message), ws(explanation), ws(primary), ws(secondary), ws(other), ws(checkbox), def);
    r->kind = 2; r->def = def; r->op = aop_new(0, 0, 8);
    WindowsDuplicateString(message, &r->s[0]); WindowsDuplicateString(explanation, &r->s[1]);
    WindowsDuplicateString(primary, &r->s[2]); WindowsDuplicateString(secondary, &r->s[3]);
    WindowsDuplicateString(other, &r->s[4]);
    *result = aop_iface(r->op);
    uiq_push(r);
    return S_OK;
}
static HRESULT WINAPI dlg_DismissDialog(void *This, void **result)
{
    LOG("DIALOG dismiss");
    *result = aop_iface(aop_new(1, 1, 0));
    return S_OK;
}
static HRESULT WINAPI dlg_ShowAuthDialogAsync(void *This, void *handler, HSTRING title, HSTRING message, HSTRING explanation,
                                              HSTRING user, HSTRING pass, boolean create, void **result)
{
    struct uireq *r = calloc(1, sizeof(*r));
    /* the initial username/password are deliberately not logged */
    LOG("DIALOG auth: title='%s' message='%s' explanation='%s' showCreate=%d", ws(title), ws(message), ws(explanation), create);
    r->kind = 1; r->op = aop_new(0, 0, 4);
    WindowsDuplicateString(title, &r->s[0]); WindowsDuplicateString(message, &r->s[1]);
    WindowsDuplicateString(explanation, &r->s[2]); WindowsDuplicateString(user, &r->s[3]);
    r->handler = handler;
    if (handler) IUnknown_AddRef((IUnknown *)handler);
    *result = aop_iface(r->op);
    uiq_push(r);
    return S_OK;
}
static HRESULT WINAPI dlg_ShowModalContentAsync(void *This, IInspectable *content, void **result)
{
    HSTRING cls = NULL;
    if (content) IInspectable_GetRuntimeClassName(content, &cls);
    LOG("DIALOG modal content: class='%s'", ws(cls));
    *result = aop_iface(aop_new(0, 0, 8));
    return S_OK;
}
static HRESULT WINAPI dlg_get_ModalContent(void *This, void **result) { *result = NULL; return S_OK; }
static boolean dlg_busy;
static HRESULT WINAPI dlg_get_Busy(void *This, boolean *b) { *b = dlg_busy; return S_OK; }
static HRESULT WINAPI dlg_put_Busy(void *This, boolean b) { dlg_busy = b; LOG("DIALOG busy=%d", b); return S_OK; }

static const void *dlg_vtbl[] = { dlg_QI, static_AddRef, static_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
    dlg_ShowInformation, dlg_ShowDialogAsync, dlg_ShowDialogAsync2, dlg_DismissDialog, dlg_ShowAuthDialogAsync,
    dlg_ShowModalContentAsync, dlg_get_ModalContent, dlg_get_Busy, dlg_put_Busy };
static const void **dialog_impl = dlg_vtbl;

HRESULT dialogs_install(void)
{
    void *svc = NULL, *ids = NULL; HRESULT hr;
    hr = CALL1(g_sp, 28, &svc);                /* ServiceProvider.DialogService */
    say("get_DialogService", hr);
    if (!svc) return FAILED(hr) ? hr : E_FAIL;
    /* the class's default interface is not IDialogService: query for it */
    hr = IUnknown_QueryInterface((IUnknown *)svc, &IID_IDialogService, &ids);
    say("QI IDialogService", hr);
    if (!ids) return hr;
    hr = CALL1(ids, 7, &dialog_impl);          /* put_Implementation */
    say("DialogService.put_Implementation", hr);
    return hr;
}
