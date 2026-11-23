/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* lab.exe: one-shot experiments against the app's services (dev/lab.sh runs it; `make -C wrapper lab`).
 * Same start-up as amd.exe, then whatever the environment asks for:
 *
 *   AM_EXPLORE=1        current view model of the navigation stack and the interfaces it answers to
 *   AM_SEARCH=<query>   what the sidebar search does (Sidebar.SearchQuerySubmitted); lists the page.
 *                       AM_PLAY_TITLE picks an item (title or class part, default SquareLockup),
 *                       AM_PLAY=1 plays it for 35 s (makes sound)
 *   AM_DEEPLINK=<url>   open a deep link (e.g. musics://music.apple.com/us/album/<id>) and list the page
 *   AM_CD_TEST=a|b|..   round-trip ContentDescriptor JSON candidates
 *   AM_CD_DUMP=1        with AM_DEEPLINK/AM_SEARCH: serialize the first catalogue item's descriptor
 *   AM_CLOUD=1          report the cloud library state; AM_CLOUD_ON=1 also turns syncing on
 *   AM_DUMP=1           with AM_DEEPLINK: every item of the page with its play action's class
 *   AM_PLAY_ITEM=s:i    with AM_DEEPLINK: run the play action of item i of section s, then
 *                       AM_QUEUE=1 dumps the play queue and AM_JUMP=<adamId> jumps to that item
 *   AM_NOWPLAYING=1     with AM_PLAY_ITEM: every field of the now-playing item
 *   AM_SILENT=1         engine volume 0 for the run, AM_RESTORE_VOLUME (default 1) afterwards
 *   AM_WAIT=<ms>        how long to let the app settle first (default 8000)
 *   AM_POLL=<n>         report the ServiceProvider life-cycle/agent state for n s at the end (default 5;
 *                       above 5 also starts a read-only agent call each second)
 * At the end it reports the ServiceProvider life-cycle state (AM_POLL). */
#include "../../wrapper/src/common.h"
#include "../../wrapper/src/app.h"
#include "../../wrapper/src/page.h"
#include "../../wrapper/src/player.h"
#include "../../wrapper/src/queue.h"

/* iids_gen.h (every interface IID in the app's metadata, with its name) is generated into
 * wrapper/build by `make lab` from your own copy of the app (dev/lab/iids.sh,
 * tools/geniids.py); it is not part of the repository. */
#if __has_include("iids_gen.h")
#include "iids_gen.h"

/* list which known interfaces an (opaque) object answers to */
static void probe_iids(void *obj, const char *label)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(known_iids); i++)
    {
        IUnknown *u = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)obj, &known_iids[i].iid, (void **)&u)) && u)
        {
            LOG("%s implements %s", label, known_iids[i].name);
            IUnknown_Release(u);
        }
    }
}
#else
static void probe_iids(void *obj, const char *label)
{
    LOG("%s: probe_iids disabled: lab.exe was built without iids_gen.h (no app metadata found "
        "by `make lab`; set WINMD_DIR to the app directory and rebuild)", label);
}
#endif

static void explore(void)
{
    void *nav = NULL, *stack = NULL; IInspectable *vm = NULL; HSTRING cls = NULL; HRESULT hr;
    hr = CALL1(g_sp, 31, &nav);
    if (nav) hr = CALL1(nav, 8, &stack);
    if (stack) hr = CALL1(stack, 6, &vm);
    if (vm) IInspectable_GetRuntimeClassName(vm, &cls);
    LOG("EXPLORE nav=%p stack=%p currentViewModel=%p class='%s' (%08lx)", nav, stack, vm, ws(cls), hr);
    if (vm) probe_iids(vm, "currentViewModel");
}

/* ---- ContentDescriptor JSON ---- */
static const GUID IID_ICDStatics = {0xb747953e,0x2ef9,0x5270,{0x84,0x48,0x42,0xdc,0x51,0xdd,0x06,0x09}};
static const GUID IID_IIterable_ContentDescriptor = {0x272ddcf0,0x4c0b,0x54d4,{0x97,0x43,0xf9,0xd9,0xe9,0x03,0xd3,0x29}};

/* a one-element IVectorView<ContentDescriptor> (also IIterable) to hand a single object to Serialize */
struct onevec { const void **view_vtbl, **iterable_vtbl, **iter_vtbl; IUnknown *item; int pos; };
#define OV_ITERABLE(p) ((struct onevec *)((char *)(p) - sizeof(void *)))
#define OV_ITER(p) ((struct onevec *)((char *)(p) - 2 * sizeof(void *)))
static HRESULT WINAPI ov_QI(void *p, REFIID iid, void **out)
{
    struct onevec *v = p;
    if (IsEqualGUID(iid, &IID_IIterable_ContentDescriptor)) *out = &v->iterable_vtbl;
    else if (IsEqualGUID(iid, &IID_IUnknown) || !is_classic_iid(iid)) *out = &v->view_vtbl;
    else { *out = NULL; return E_NOINTERFACE; }
    return S_OK;
}
static ULONG WINAPI ov_AddRef(void *p) { return 2; }
static ULONG WINAPI ov_Release(void *p) { return 1; }
static HRESULT WINAPI ov_GetAt(struct onevec *v, UINT32 i, void **out)
{
    if (i) { *out = NULL; return E_BOUNDS; }
    *out = v->item; IUnknown_AddRef(v->item);
    return S_OK;
}
static HRESULT WINAPI ov_get_Size(void *p, UINT32 *n) { *n = 1; return S_OK; }
static HRESULT WINAPI ov_IndexOf(struct onevec *v, void *item, UINT32 *i, boolean *found) { *i = 0; *found = item == v->item; return S_OK; }
static HRESULT WINAPI ov_GetMany(struct onevec *v, UINT32 start, UINT32 cap, void **items, UINT32 *n)
{
    *n = 0;
    if (!start && cap) { items[0] = v->item; IUnknown_AddRef(v->item); *n = 1; }
    return S_OK;
}
static HRESULT WINAPI ovi_QI(void *p, REFIID iid, void **out) { return ov_QI(OV_ITERABLE(p), iid, out); }
static HRESULT WINAPI ovi_First(void *p, void **out) { struct onevec *v = OV_ITERABLE(p); v->pos = 0; *out = &v->iter_vtbl; return S_OK; }
static HRESULT WINAPI ovt_QI(void *p, REFIID iid, void **out) { *out = p; return S_OK; }
static HRESULT WINAPI ovt_get_Current(void *p, void **out)
{
    struct onevec *v = OV_ITER(p);
    if (v->pos) { *out = NULL; return E_BOUNDS; }
    *out = v->item; IUnknown_AddRef(v->item);
    return S_OK;
}
static HRESULT WINAPI ovt_get_HasCurrent(void *p, boolean *has) { *has = !OV_ITER(p)->pos; return S_OK; }
static HRESULT WINAPI ovt_MoveNext(void *p, boolean *has) { OV_ITER(p)->pos = 1; *has = FALSE; return S_OK; }
static HRESULT WINAPI ovt_GetMany(void *p, UINT32 cap, void **items, UINT32 *n)
{
    struct onevec *v = OV_ITER(p);
    *n = 0;
    if (!v->pos && cap) { items[0] = v->item; IUnknown_AddRef(v->item); *n = 1; v->pos = 1; }
    return S_OK;
}
static const void *ov_view_vtbl[] = { ov_QI, ov_AddRef, ov_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                      ov_GetAt, ov_get_Size, ov_IndexOf, ov_GetMany };
static const void *ov_iterable_vtbl[] = { ovi_QI, ov_AddRef, ov_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel, ovi_First };
static const void *ov_iter_vtbl[] = { ovt_QI, ov_AddRef, ov_Release, insp_GetIids, insp_GetRuntimeClassName, insp_GetTrustLevel,
                                      ovt_get_Current, ovt_get_HasCurrent, ovt_MoveNext, ovt_GetMany };
static void *onevec_new(void *item)
{
    struct onevec *v = calloc(1, sizeof(*v));
    v->view_vtbl = ov_view_vtbl; v->iterable_vtbl = ov_iterable_vtbl; v->iter_vtbl = ov_iter_vtbl; v->item = item;
    return v;
}

/* learn the ContentDescriptor JSON form from a real descriptor (first catalogue item) */
static int cd_dump_cb(const struct page_item *it, void *ctx)
{
    void *cst; HSTRING ser = NULL, ident = NULL; HRESULT hr;
    if (!it->cd || !WindowsGetStringLen(it->adam) || wz(it->adam)[0] < '0' || wz(it->adam)[0] > '9') return 0;
    if (!(cst = get_statics(L"AMP.Services.CommonModels.ContentDescriptor", &IID_ICDStatics))) return 1;
    hr = ((HRESULT (WINAPI *)(void *, void *, HSTRING *))VT(cst)[6])(cst, onevec_new(it->cd), &ser);   /* Serialize */
    CALL1(it->cd, 17, &ident);
    LOG("CD-DUMP (%08lx) identifier='%s' json=%s", hr, ws(ident), ws(ser));
    return 1;
}

static void cd_test(WCHAR *tests)
{
    void *cst = get_statics(L"AMP.Services.CommonModels.ContentDescriptor", &IID_ICDStatics);
    WCHAR *t, *next;
    LOG("ContentDescriptor statics %p", cst);
    for (t = tests; cst && t && *t; t = next)
    {
        void *vec = NULL; UINT32 n = 0; HSTRING ser = NULL, in; HRESULT hr;
        if ((next = wcschr(t, '|'))) *next++ = 0;
        in = hs(t);
        hr = ((HRESULT (WINAPI *)(void *, HSTRING, void **))VT(cst)[7])(cst, in, &vec);    /* DeserializeContentDescriptors */
        if (vec) CALL1(vec, 7, &n);
        LOG("CD in=%s -> %08lx count=%u", ws(in), hr, n);
        if (vec && n)
        {
            void *cd = NULL; HSTRING id = NULL, adam = NULL; boolean cat = 0, lib = 0, loc = 0;
            hr = ((HRESULT (WINAPI *)(void *, void *, HSTRING *))VT(cst)[6])(cst, vec, &ser);
            LOG("CD   serialized(%08lx)=%s", hr, ws(ser));
            ((HRESULT (WINAPI *)(void *, UINT32, void **))VT(vec)[6])(vec, 0, &cd);
            if (cd)
            {
                CALL1(cd, 6, &cat); CALL1(cd, 8, &lib); CALL1(cd, 9, &loc); CALL1(cd, 17, &id); CALL1(cd, 21, &adam);
                LOG("CD   catalog=%d cloudLib=%d local=%d adamId='%s' identifier='%s'", cat, lib, loc, ws(adam), ws(id));
            }
        }
    }
}

/* ---- pages ---- */
/* wait for a loaded page other than `previous` (the page shown before navigating) */
static void *wait_page_after(void *previous, int max_half_seconds)
{
    void *cpage = NULL, *page = NULL; boolean loading = 1, err = 0;
    for (int i = 0; i < max_half_seconds; i++)
    {
        pump(500);
        cpage = current_content_page();
        if (cpage) page_state(cpage, &loading, &err, &page);
        if (i % 2 == 0) LOG("wait: %.1f s page=%p (before %p) loading=%d page=%p", i / 2.0, cpage, previous, loading, page);
        if (!cpage || cpage == previous) continue;
        if (!loading && page && i > 6) break;
    }
    LOG("page=%p loading=%d error=%d", cpage, loading, err);
    return cpage;
}

static void *wait_page(int max_half_seconds) { return wait_page_after(NULL, max_half_seconds); }

static void play_for_a_while(IInspectable *action)
{
    struct player_status st; void *act = NULL;
    say("dispatch play action of the chosen item", dispatch_action(action));
    for (int i = 0; i < 7; i++)
    {
        pump(5000);
        player_status(&st);
        LOG("PLAYER [%ds] playing=%d paused=%d title='%s' artist='%s' album='%s'", (i + 1) * 5, st.playing, st.paused,
            ws(st.title), ws(st.artist), ws(st.album));
        player_status_free(&st);
    }
    if (get_player()) { say("Player.StopAsync", CALL1(get_player(), 14, &act)); pump(3000); }   /* no music left playing */
}

static void list_and_maybe_play(void *cpage)
{
    WCHAR want[128] = L"SquareLockup"; IInspectable *play;
    if (!cpage) return;
    if (GetEnvironmentVariableA("AM_CD_DUMP", NULL, 0)) page_walk(cpage, cd_dump_cb, NULL, 0);
    GetEnvironmentVariableW(L"AM_PLAY_TITLE", want, ARRAY_SIZE(want));
    play = page_list(cpage, want);
    if (play && GetEnvironmentVariableA("AM_PLAY", NULL, 0)) play_for_a_while(play);
}

static void search(const WCHAR *query)
{
    void *sidebar = NULL;
    if (!g_main_vm) return;
    CALL1(g_main_vm, 24, &sidebar);                          /* MainPageViewModel.Sidebar */
    LOG("SEARCH sidebar=%p", sidebar);
    if (sidebar) say("Sidebar.SearchQuerySubmitted", CALL1(sidebar, 30, hs(query)));
    list_and_maybe_play(wait_page(80));
}

/* ---- cloud library ---- */
static void cloud(void)
{
    void *cl = NULL; boolean busy = 0, offline = 0, avail = 0, on = 0;
    CALL1(g_sp, 23, &cl);                                    /* ServiceProvider.CloudLibrary */
    if (!cl) return;
    CALL1(cl, 6, &busy); CALL1(cl, 8, &offline); CALL1(cl, 9, &avail); CALL1(cl, 10, &on);
    LOG("cloud library: busy=%d offline=%d syncAvailable=%d syncEnabled=%d", busy, offline, avail, on);
    if (!on && GetEnvironmentVariableA("AM_CLOUD_ON", NULL, 0))
    {
        /* what the "Sync Library" setting does (the app also tries it itself after sign-in) */
        void *op = NULL; INT32 status, code = -1; HRESULT err;
        say("CloudLibraryService.TurnOnCloudSyncingAsync(resetLibrary=false)",
            ((HRESULT (WINAPI *)(void *, boolean, void **))VT(cl)[15])(cl, FALSE, &op));
        status = await_async(op, 250, 480, &err);
        if (status == 1) CALL1(op, 8, &code);
        LOG("TurnOnCloudSyncing: async status=%d result=%d", status, code);
        CALL1(cl, 6, &busy); CALL1(cl, 8, &offline); CALL1(cl, 10, &on);
        LOG("cloud library after: busy=%d offline=%d syncEnabled=%d", busy, offline, on);
    }
}

/* ---- page items, queue ---- */
static int dump_cb(const struct page_item *it, void *ctx)
{
    HSTRING acls = NULL;
    if (it->action) IInspectable_GetRuntimeClassName(it->action, &acls);
    LOG("  [%u:%u] %s title='%s' adamId='%s' action=%s queueId=%I64u", it->section, it->index,
        ws(it->cls) + (WindowsGetStringLen(it->cls) > 27 ? 27 : 0), ws(it->title), ws(it->adam),
        it->action ? ws(acls) : "-", queue_item_id(it->item));
    return 0;
}

struct pick { UINT32 section, index; IInspectable *action; const WCHAR *adam; UINT64 id; };
static int pick_cb(const struct page_item *it, void *ctx)
{
    struct pick *p = ctx;
    if (p->adam) { if (!wcscmp(wz(it->adam), p->adam)) { p->id = queue_item_id(it->item); return p->id != 0; } return 0; }
    if (it->section == p->section && it->index == p->index) { p->action = it->action; if (p->action) IInspectable_AddRef(p->action); return 1; }
    return 0;
}

/* the queue three ways: PlayQueue.Current, after opening the queue panel, and a PlayQueueViewModel */
static void dump_queue(void)
{
    static const GUID IID_IPlayQueueViewModel = {0xf86806d9,0x0913,0x5978,{0x88,0xc1,0xdd,0x34,0x60,0x8c,0x55,0x6c}};
    static IInspectable *vm_insp;
    IActivationFactory *f = NULL; void *vm = NULL;
    LOG("QUEUE via PlayQueue.Current:");
    queue_walk(dump_cb, NULL, 1);
    if (g_main_vm) say("MainPageViewModel.put_IsPlayQueueOpen(true)", ((HRESULT (WINAPI *)(void *, boolean))VT(g_main_vm)[16])(g_main_vm, TRUE));
    if (!vm_insp && SUCCEEDED(get_factory(L"AMP.Services.CommonModels.PlayQueueViewModel", &f)) && f)
        say("PlayQueueViewModel activate", IActivationFactory_ActivateInstance(f, &vm_insp));
    pump(3000);
    LOG("QUEUE via PlayQueue.Current after opening the panel:");
    queue_walk(dump_cb, NULL, 1);
    if (vm_insp && SUCCEEDED(IInspectable_QueryInterface(vm_insp, &IID_IPlayQueueViewModel, &vm)))
    {
        static const char *const names[] = { "QueuedSections", "QueuedItems", "QueuedItemsOther", "QueuedItemsAutoplay" };
        for (int k = 0; k < 4; k++)
        {
            IUnknown *v = NULL;
            CALL1(vm, 19 + k, &v);
            LOG("QUEUE view model %s %p:", names[k], v);
            if (v) items_walk(v, k, dump_cb, NULL);
        }
    }
}

/* every field of the now-playing item that could identify or describe it */
static void dump_nowplaying(void)
{
    static const GUID IID_INowPlayingItem = {0x2dc5f65c,0x28f6,0x58aa,{0x87,0xcf,0xe4,0xa7,0xdf,0x9b,0x9a,0x4b}};
    static const GUID IID_IContentDescriptorDriven = {0x0db3eb30,0xb156,0x5201,{0xac,0x4a,0x13,0x26,0x64,0x8c,0x30,0x7b}};
    static const GUID IID_IArtwork = {0x6f00c1ef,0x8302,0x5354,{0xbd,0x7e,0x8d,0x1a,0x94,0x3b,0x4f,0x0d}};
    static const GUID IID_IUriRuntimeClass = {0x9e365e57,0x48b2,0x4160,{0x95,0x6f,0xc7,0x38,0x51,0x20,0xbb,0xfc}};
    static const struct { int slot; const char *name; } strs[] = {
        {6, "Subtitle"}, {7, "Artist"}, {8, "Album"}, {18, "ISOReleaseDate"}, {19, "Name"}, {21, "Genre"},
        {22, "Description"}, {23, "Accessibility"}, {28, "ShowCanonicalId"}, {41, "HLSMetadata"} };
    IUnknown *item = NULL; void *ini = NULL, *cdd = NULL, *cd = NULL, *art = NULL, *uri = NULL;
    INT32 num = 0, count = 0; UINT64 qid = 0; IUnknown *artwork = NULL, *u = NULL;
    if (!get_player()) return;
    CALL1(get_player(), 6, &item);
    if (!item || FAILED(IUnknown_QueryInterface(item, &IID_INowPlayingItem, &ini))) { LOG("NP: no item"); return; }
    for (int i = 0; i < (int)ARRAY_SIZE(strs); i++)
    {
        HSTRING h = NULL;
        CALL1(ini, strs[i].slot, &h);
        LOG("NP %s='%.300s'", strs[i].name, ws(h));
        WindowsDeleteString(h);
    }
    CALL1(ini, 9, &num); CALL1(ini, 10, &count); CALL1(ini, 45, &qid);
    LOG("NP TrackNumber=%d TrackCount=%d PlayQueueItemID=%I64u", num, count, qid);
    CALL1(ini, 11, &artwork);
    if (artwork && SUCCEEDED(IUnknown_QueryInterface(artwork, &IID_IArtwork, &art)))
    {
        HSTRING fmt = NULL, abs = NULL; double w = 0, h = 0;
        CALL1(art, 10, &fmt); CALL1(art, 12, &h); CALL1(art, 14, &w); CALL1(art, 16, &u);
        if (u && SUCCEEDED(IUnknown_QueryInterface(u, &IID_IUriRuntimeClass, &uri))) CALL1(uri, 6, &abs);
        LOG("NP Artwork format='%s' %.0fx%.0f uri='%s'", ws(fmt), w, h, ws(abs));
    }
    else LOG("NP Artwork %p (no IArtwork)", artwork);
    if (SUCCEEDED(IUnknown_QueryInterface(item, &IID_IContentDescriptorDriven, &cdd))) CALL1(cdd, 6, &cd);
    if (cd)
    {
        HSTRING adam = NULL, ident = NULL, canon = NULL;
        CALL1(cd, 21, &adam); CALL1(cd, 17, &ident); CALL1(cd, 22, &canon);
        LOG("NP ContentDescriptor adamId='%s' identifier='%s' canonicalId='%s'", ws(adam), ws(ident), ws(canon));
    }
    else LOG("NP no ContentDescriptor");
}

static void set_volume(double v)
{
    void *act = NULL;
    if (get_player()) say("SetVolumeAsync", ((HRESULT (WINAPI *)(void *, double, void **))VT(get_player())[27])(get_player(), v, &act));
}

static void play_item(void *cpage, const char *spec)
{
    struct pick p = {0}; struct player_status st; WCHAR jump[32];
    if (sscanf(spec, "%u:%u", &p.section, &p.index) != 2 || !cpage) return;
    page_walk(cpage, pick_cb, &p, 0);
    LOG("PLAY-ITEM %u:%u action=%p", p.section, p.index, p.action);
    if (!p.action) return;
    say("dispatch", dispatch_action(p.action));
    pump(8000);
    player_status(&st);
    LOG("now playing '%s' playing=%d", ws(st.title), st.playing);
    player_status_free(&st);
    if (GetEnvironmentVariableA("AM_NOWPLAYING", NULL, 0)) dump_nowplaying();
    if (GetEnvironmentVariableA("AM_QUEUE", NULL, 0)) dump_queue();
    if (GetEnvironmentVariableW(L"AM_JUMP", jump, ARRAY_SIZE(jump)))
    {
        struct pick q = {0};
        q.adam = jump;
        queue_walk(pick_cb, &q, 0);
        LOG("JUMP to %ls: queue item %I64u", jump, q.id);
        if (q.id) say("PlayPlayQueueItemAsync", queue_play_item(q.id));
        pump(6000);
        player_status(&st);
        LOG("now playing '%s' playing=%d", ws(st.title), st.playing);
        player_status_free(&st);
        if (GetEnvironmentVariableA("AM_QUEUE", NULL, 0)) queue_walk(dump_cb, NULL, 1);
    }
}

int main(void)
{
    WCHAR arg[4096]; char wait[16] = "8000";
    GetEnvironmentVariableA("AM_WAIT", wait, sizeof(wait));
    if (app_start(atoi(wait))) return 1;
    app_wait_playback(40000);
    if (GetEnvironmentVariableA("AM_SILENT", NULL, 0)) set_volume(0);
    if (GetEnvironmentVariableA("AM_EXPLORE", NULL, 0)) explore();
    if (GetEnvironmentVariableW(L"AM_SEARCH", arg, ARRAY_SIZE(arg))) search(arg);
    if (GetEnvironmentVariableW(L"AM_DEEPLINK", arg, ARRAY_SIZE(arg)))
    {
        void *cpage = current_content_page();
        char spec[32];
        say("ProcessDeeplinkAsync", open_deeplink(arg));
        cpage = wait_page_after(cpage, 40);
        if (cpage && GetEnvironmentVariableA("AM_DUMP", NULL, 0)) page_walk(cpage, dump_cb, NULL, 1);
        if (GetEnvironmentVariableA("AM_PLAY_ITEM", spec, sizeof(spec))) play_item(cpage, spec);
        else list_and_maybe_play(cpage);
    }
    if (GetEnvironmentVariableW(L"AM_CD_TEST", arg, ARRAY_SIZE(arg))) cd_test(arg);
    if (GetEnvironmentVariableA("AM_CLOUD", NULL, 0) || GetEnvironmentVariableA("AM_CLOUD_ON", NULL, 0)) cloud();
    if (GetEnvironmentVariableA("AM_SILENT", NULL, 0))
    {
        char v[16] = "1";
        void *act = NULL;
        if (get_player()) { CALL1(get_player(), 14, &act); pump(1500); }      /* Stop */
        GetEnvironmentVariableA("AM_RESTORE_VOLUME", v, sizeof(v));
        set_volume(atof(v));
        pump(1000);
    }
    {   /* where the getters live (RVAs in AMP.Services.dll), for disassembly */
        char *base = (char *)GetModuleHandleA("AMP.Services.dll");
        for (int s = 14; s <= 19; s++) LOG("IServiceProvider[%d] = AMP.Services+%#llx", s, (unsigned long long)((char *)VT(g_sp)[s] - base));
    }
    {
        char n[16] = "5";
        int polls;
        GetEnvironmentVariableA("AM_POLL", n, sizeof(n));
        polls = atoi(n);
        for (int i = 0; i < polls; i++)
        {
            INT32 st = -1, ags = -1; HSTRING ph = NULL; boolean hung = 0;
            pump(1000);
            if (polls > 5 && get_player())          /* GetEQEnabledAsync: a read-only agent call */
            {
                IUnknown *op = NULL;
                if (SUCCEEDED(CALL1(get_player(), 32, &op)) && op) IUnknown_Release(op);
            }
            CALL1(g_sp, 14, &st); CALL1(g_sp, 16, &ph); CALL1(g_sp, 17, &ags); CALL1(g_sp, 18, &hung);
            LOG("[%d] lifecycle=%d agentPhase='%s' agentStatus=%d hung=%d", i, st, ws(ph), ags, hung);
            if (ph) WindowsDeleteString(ph);
        }
    }
    return 0;
}
