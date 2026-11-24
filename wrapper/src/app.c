/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bringing up AMP.Services without its UI (see app.h).
 *
 * Required order (docs/architecture.md):
 *  1. ampkg.dll (package identity), RoInitialize(STA)
 *  2. marshal a dummy object once (starts Wine's apartment remoting)
 *  3. a Microsoft.UI DispatcherQueue on this thread (amshim)
 *  4. OSLogInitialize("AppleMusic")
 *  5. ServiceProvider.InitializeAppContext / Initialize(false, factory) / AutoStartAsync
 *  6. our IDialogServiceImpl
 *  7. MainPageViewModel.TryLoadAsync + NavigatedTo, accept the first-run notice
 *     (only then does the app launch the agent and start its services)
 *  8. pump messages on this thread */
#include "app.h"
#include "akui.h"
#include "cxxdbg.h"
#include "dialogs.h"
#include "iat.h"
#include "jsvalues.h"
#include "medianame.h"
#include "oslog.h"
#include "server.h"

static const GUID IID_IServiceProvider_ = {0x2027b2e9,0x93ea,0x5144,{0xbb,0xd1,0x2c,0x26,0x70,0xff,0x15,0xef}};
static const GUID IID_IServiceProviderStatics = {0x391eacbc,0xb710,0x5521,{0xab,0x5c,0x65,0x1a,0x38,0x73,0xed,0x97}};
static const GUID IID_ICustomPageViewModelFactory = {0x293942f8,0xc075,0x50d6,{0xba,0xa0,0xfe,0x0f,0xf0,0xca,0xdf,0x1d}};
static const GUID IID_IDQCStatics = {0xf18d6145,0x722b,0x593d,{0xbc,0xf2,0xa6,0x1e,0x71,0x3f,0x00,0x37}};
static const GUID IID_IMainPageViewModelStatics = {0x1de2686d,0x0295,0x5a19,{0x80,0xbf,0xed,0x04,0x32,0x2e,0xaf,0xbd}};
static const GUID IID_IService = {0x02bf1b3f,0xdcc7,0x5e21,{0xb2,0x2b,0x79,0x50,0xbf,0xc2,0x6e,0x51}};

void *g_sp, *g_main_vm;

/* AMP::Utils::ClipboardContainsContentDescriptors() asks WinRT's Clipboard, which Wine does not
 * have (class not registered), and that failure aborts building an item's context menu (the
 * menu /enqueue uses). Nothing is ever on the clipboard here. */
static unsigned char no_clipboard_content(void) { return 0; }

void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms; int left; MSG msg;
    while ((int)(end - GetTickCount()) > 0)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        dialogs_process();
        server_process();
        media_name_tick();
        left = end - GetTickCount();
        if (left > 0) MsgWaitForMultipleObjects(0, NULL, FALSE, left < 50 ? left : 50, QS_ALLINPUT);
    }
}

/* ---- ICustomPageViewModelFactory: ServiceProvider.Initialize requires a non-null one (the
 * UI's). We have no XAML pages, so it creates nothing. ---- */
static HRESULT WINAPI pvf_QI(void *This, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IInspectable_) || IsEqualGUID(iid, &IID_IAgileObject_) ||
        IsEqualGUID(iid, &IID_ICustomPageViewModelFactory)) { *out = This; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI pvf_AddRef(void *This) { return 2; }
static ULONG WINAPI pvf_Release(void *This) { return 1; }
static HRESULT WINAPI pvf_CreateViewModel(void *This, INT32 kind, IInspectable *param, IInspectable **result)
{
    LOG("CreateViewModel kind=%d param=%p", kind, param);
    *result = NULL;
    return S_OK;
}
static const void *pvf_vtbl[] = { pvf_QI, pvf_AddRef, pvf_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                  pvf_CreateViewModel };
static const void **page_vm_factory = pvf_vtbl;

/* Wine only starts an apartment's remoting (and its IRundown stub) when the apartment first
 * marshals something. C++/WinRT's "co_await ui_context" reaches this STA through
 * IContextCallback -> IRundown::DoCallback, which fails with RPC_S_SERVER_UNAVAILABLE until then.
 * Marshal a dummy object once to bring the apartment's remoting up. */
static void start_apartment_remoting(void)
{
    IStream *stm = NULL;
    say("start STA remoting", CoMarshalInterThreadInterfaceInStream(&IID_IUnknown, (IUnknown *)&page_vm_factory, &stm));
    if (stm) { CoReleaseMarshalData(stm); IStream_Release(stm); }
}

/* stand-in for WinUI's Application.Start: a Microsoft.UI.Dispatching queue on this thread */
static void create_dispatcher_queue(void)
{
    IActivationFactory *df = NULL; void *st = NULL, *ctrl = NULL; HSTRING cls = hs(L"Microsoft.UI.Dispatching.DispatcherQueueController");
    HRESULT hr = RoGetActivationFactory(cls, &IID_IActivationFactory_, (void **)&df);
    say("DispatcherQueueController factory", hr);
    if (SUCCEEDED(hr)) { hr = IActivationFactory_QueryInterface(df, &IID_IDQCStatics, &st); say("QI DQC statics", hr); }
    if (st) { hr = CALL1(st, 7, &ctrl); say("CreateOnCurrentThread", hr); }
}

static void log_services(void)
{
    static const struct { int slot; const char *name; } svcs[] = {
        {20, "ActivityService"}, {21, "AgentDebugService"}, {22, "ArtworkService"}, {23, "CloudLibraryService"},
        {24, "LibraryService"}, {25, "PlaybackService"}, {26, "VideoPlaybackService"} };
    for (int i = 0; i < (int)ARRAY_SIZE(svcs); i++)
    {
        IUnknown *u = NULL; void *svc = NULL; HSTRING nm = NULL; boolean autostart = 0, started = 0;
        HRESULT hr = CALL1(g_sp, svcs[i].slot, &u);
        if (FAILED(hr) || !u) { LOG("%s: get -> %08lx %p", svcs[i].name, hr, u); continue; }
        if (FAILED(hr = IUnknown_QueryInterface(u, &IID_IService, &svc))) { LOG("%s: no IService %08lx", svcs[i].name, hr); continue; }
        CALL1(svc, 6, &nm); CALL1(svc, 7, &autostart); CALL1(svc, 8, &started);
        LOG("service %s name='%s' autostart=%d started=%d", svcs[i].name, ws(nm), autostart, started);
    }
}

/* steps 5: the ServiceProvider */
static int start_service_provider(DWORD settle_ms)
{
    IActivationFactory *f = NULL; IInspectable *insp = NULL; void *statics = NULL, *act = NULL; HRESULT hr;
    hr = get_factory(L"AMP.Services.ServiceProvider", &f);
    say("factory ServiceProvider", hr); if (FAILED(hr)) return 1;
    hr = IActivationFactory_QueryInterface(f, &IID_IServiceProviderStatics, &statics);
    say("QI statics", hr); if (FAILED(hr)) return 1;
    say("InitializeAppContext", CALL0(statics, 6));
    hr = IActivationFactory_ActivateInstance(f, &insp);
    say("ActivateInstance", hr); if (FAILED(hr)) return 1;
    hr = IInspectable_QueryInterface(insp, &IID_IServiceProvider_, &g_sp);
    say("QI IServiceProvider", hr); if (FAILED(hr)) return 1;
    say("Initialize", ((HRESULT (WINAPI *)(void *, boolean, void *))VT(g_sp)[6])(g_sp, FALSE, &page_vm_factory));
    say("AutoStartAsync", CALL1(g_sp, 11, &act));
    pump(settle_ms ? settle_ms : 100);
    log_services();
    return 0;
}

/* step 7: the main page view model */
static void load_main_page(DWORD settle_ms)
{
    void *mst = get_statics(L"AMP.Services.CommonModels.MainPageViewModel", &IID_IMainPageViewModelStatics), *op = NULL;
    HRESULT hr, err; INT32 status;
    if (!mst) { LOG("no MainPageViewModel statics"); return; }
    hr = ((HRESULT (WINAPI *)(void *, void *, void **))VT(mst)[6])(mst, g_sp, &op);
    say("MainPageViewModel.TryLoadAsync", hr);
    status = await_async(op, 500, 30, &err);
    LOG("TryLoadAsync status=%d error=%08lx", status, err);
    if (status == 1) { hr = CALL1(op, 8, &g_main_vm); LOG("MainPageViewModel %p (%08lx)", g_main_vm, hr); }
    if (!g_main_vm) return;
    say("MainPageViewModel.NavigatedTo", CALL0(g_main_vm, 30));
    pump(settle_ms ? settle_ms : 100);
}

int app_start(DWORD settle_ms)
{
    HMODULE m;
    setvbuf(stdout, NULL, _IONBF, 0);
    LoadLibraryA("ampkg.dll");
    cxxdbg_install();
    say("RoInitialize", RoInitialize(RO_INIT_SINGLETHREADED));
    start_apartment_remoting();
    create_dispatcher_queue();

    akui_log_enable();
    m = LoadLibraryA("AMP.Services.dll");
    LOG("AMP.Services %p err %lu", m, GetLastError());
    if (!m) return 1;
    oslog_hook_all();
    jsvalues_install();
    LOG("AuthKit UI provider installed: %d", akui_install());
    LOG("clipboard check redirected: %d", iat_patch(m, "AMP.Utils.dll", "?ClipboardContainsContentDescriptors@Utils@AMP@winrt@@YA_NXZ", no_clipboard_content));
    {   /* AppleMusic.exe does this first thing in wWinMain */
        void (*oslog_init)(const char *) = (void *)GetProcAddress(GetModuleHandleA("AMP.Utils.dll"), "?OSLogInitialize@@YAXPEBD@Z");
        LOG("OSLogInitialize %p", oslog_init);
        if (oslog_init) oslog_init("AppleMusic");
    }

    if (start_service_provider(settle_ms)) return 1;
    dialogs_install();
    notifications_subscribe();
    load_main_page(settle_ms);
    LOG("welcome dialog pending: %s", welcome_pending() ? "yes" : "no");
    if (welcome_pending() && GetEnvironmentVariableA("AM_ACCEPT_WELCOME", NULL, 0))
    {
        /* explicit opt-in (AM_ACCEPT_WELCOME=1, set by bin/amd and bin/amd-signin): press the
         * notice's accept button */
        welcome_accept();
        pump(200);
    }
    return 0;
}

int app_wait_playback(DWORD timeout_ms)
{
    IUnknown *pb = NULL; void *svc = NULL; boolean started = 0;
    CALL1(g_sp, 25, &pb);                       /* ServiceProvider.PlaybackService */
    if (pb) IUnknown_QueryInterface(pb, &IID_IService, &svc);
    for (DWORD waited = 0; svc && waited < timeout_ms && !started && !oslog_agent_lost; waited += 100)
    {
        pump(100);
        CALL1(svc, 8, &started);                /* IsStarted */
    }
    LOG("playback service started=%d agentLost=%ld", started, oslog_agent_lost);
    return started && !oslog_agent_lost;
}
