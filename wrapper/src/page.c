/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The app's page model (see page.h). */
#include "page.h"

static const GUID IID_IContentPageViewModel = {0x23eb3799,0x0c8c,0x58d7,{0xa3,0xbe,0x21,0x36,0xad,0xca,0x90,0xb1}};
static const GUID IID_IComponentModelContainer = {0xe4d9e42b,0x143e,0x55a0,{0xb1,0x49,0x34,0x6a,0x4a,0xa1,0xd6,0xb4}};
static const GUID IID_IVector_IComponentModel = {0x2c559b4a,0x4165,0x5e91,{0x86,0xf1,0xcd,0x3e,0xc0,0x88,0x44,0x19}};
static const GUID IID_ITitledComponent = {0x29691940,0x53dc,0x5c20,{0x94,0x2f,0x29,0xeb,0x78,0xf3,0x8f,0x1f}};
static const GUID IID_IPlayActionDriven = {0xc09c6d38,0xf0c9,0x5415,{0xa6,0x85,0x40,0xd7,0x5b,0xa4,0x2a,0xa9}};
static const GUID IID_IContentDescriptorDriven = {0x0db3eb30,0xb156,0x5201,{0xac,0x4a,0x13,0x26,0x64,0x8c,0x30,0x7b}};
static const GUID IID_INotificationArgsFactory = {0x9b1e00ef,0x237a,0x5724,{0xb2,0x0b,0x56,0x5f,0x84,0x24,0x3a,0x61}};

#define EVENT_DISPATCH 0x64697370   /* 'disp' */

void *current_content_page(void)
{
    void *nav = NULL, *stack = NULL, *page = NULL; IUnknown *vm = NULL;
    CALL1(g_sp, 31, &nav);                  /* ServiceProvider.Navigation */
    if (nav) { CALL1(nav, 8, &stack); IUnknown_Release((IUnknown *)nav); }      /* RootStack */
    if (stack) { CALL1(stack, 6, &vm); IUnknown_Release((IUnknown *)stack); }   /* CurrentViewModel */
    if (vm) { IUnknown_QueryInterface(vm, &IID_IContentPageViewModel, &page); IUnknown_Release(vm); }
    return page;
}

void page_state(void *cpage, boolean *loading, boolean *error, void **page)
{
    *page = NULL;
    CALL1(cpage, 14, loading);
    CALL1(cpage, 9, error);
    CALL1(cpage, 18, page);
}

static int items_walk_in(IUnknown *items, IUnknown *container, UINT32 si, page_item_cb cb, void *ctx);

int container_walk(IUnknown *section, UINT32 si, page_item_cb cb, void *ctx, int log_sections)
{
    IUnknown *items = NULL; void *cont = NULL; HSTRING sid = NULL; int r;
    if (log_sections) CALL1(section, 6, &sid);     /* IComponentModel.Id */
    if (SUCCEEDED(IUnknown_QueryInterface(section, &IID_IComponentModelContainer, &cont)))
    {
        CALL1(cont, 6, &items);
        IUnknown_Release((IUnknown *)cont);
    }
    if (log_sections) LOG("section %u id='%s'", si, ws(sid));
    WindowsDeleteString(sid);
    r = items ? items_walk_in(items, section, si, cb, ctx) : 0;
    if (items) IUnknown_Release(items);
    return r;
}

int items_walk(IUnknown *items, UINT32 si, page_item_cb cb, void *ctx)
{
    return items_walk_in(items, NULL, si, cb, ctx);
}

/* everything the walk got for one item is released after the callback (which AddRefs what it keeps) */
static void page_item_free(struct page_item *it)
{
    WindowsDeleteString(it->cls); WindowsDeleteString(it->title); WindowsDeleteString(it->adam);
    if (it->action) IInspectable_Release(it->action);
    if (it->cd) IUnknown_Release((IUnknown *)it->cd);
    IInspectable_Release(it->item);
}

static int items_walk_in(IUnknown *items, IUnknown *container, UINT32 si, page_item_cb cb, void *ctx)
{
    void *vec = NULL; UINT32 n = 0; int r = 0;
    if (items) IUnknown_QueryInterface(items, &IID_IVector_IComponentModel, &vec);
    if (vec) CALL1(vec, 7, &n);
    for (UINT32 i = 0; vec && i < n && i < 500 && !r; i++)
    {
        struct page_item it = { si, i, container };
        void *tc = NULL, *pa = NULL, *cdd = NULL;
        ((HRESULT (WINAPI *)(void *, UINT32, void **))VT(vec)[6])(vec, i, (void **)&it.item);
        if (!it.item) continue;
        IInspectable_GetRuntimeClassName(it.item, &it.cls);
        if (SUCCEEDED(IInspectable_QueryInterface(it.item, &IID_ITitledComponent, &tc))) { CALL1(tc, 6, &it.title); IUnknown_Release((IUnknown *)tc); }
        if (SUCCEEDED(IInspectable_QueryInterface(it.item, &IID_IPlayActionDriven, &pa))) { CALL1(pa, 6, &it.action); IUnknown_Release((IUnknown *)pa); }
        if (SUCCEEDED(IInspectable_QueryInterface(it.item, &IID_IContentDescriptorDriven, &cdd))) { CALL1(cdd, 6, &it.cd); IUnknown_Release((IUnknown *)cdd); }
        if (it.cd) CALL1(it.cd, 21, &it.adam);
        r = cb(&it, ctx);
        page_item_free(&it);
    }
    if (vec) IUnknown_Release((IUnknown *)vec);
    return r;
}

void page_walk(void *cpage, page_item_cb cb, void *ctx, int log_sections)
{
    void *page = NULL; int r = 0;
    CALL1(cpage, 18, &page);
    if (!page) { LOG("PAGE has no page object"); return; }
    for (UINT32 si = 0; si < 40 && !r; si++)
    {
        boolean has = 0; IUnknown *sec = NULL;
        ((HRESULT (WINAPI *)(void *, UINT32, boolean *))VT(page)[30])(page, si, &has);        /* HasSectionAtIndex */
        if (!has) break;
        ((HRESULT (WINAPI *)(void *, UINT32, void **))VT(page)[31])(page, si, (void **)&sec); /* TryGetSectionAtIndex */
        if (sec) { r = container_walk(sec, si, cb, ctx, log_sections); IUnknown_Release(sec); }
    }
    IUnknown_Release((IUnknown *)page);
}

struct find_ctx { const WCHAR *want; int verbose; IInspectable *found; };

static int find_cb(const struct page_item *it, void *ctx)
{
    struct find_ctx *f = ctx;
    if (f->verbose && it->index < 6)
        LOG("PAGE   [%u] %s title='%s' adamId='%s' playAction=%s", it->index,
            ws(it->cls) + (WindowsGetStringLen(it->cls) > 27 ? 27 : 0), ws(it->title), ws(it->adam), it->action ? "yes" : "no");
    if (f->found || !it->action || !f->want) return 0;
    if (!wcscmp(wz(it->adam), f->want) ||
        (f->verbose && (wcsstr(wz(it->cls), f->want) || wcsstr(wz(it->title), f->want))))
    {
        f->found = it->action;
        IInspectable_AddRef(f->found);
        if (f->verbose) LOG("PAGE   -> chose [%u] title='%s' adamId='%s'", it->index, ws(it->title), ws(it->adam));
        else return 1;
    }
    return 0;
}

IInspectable *find_play_action(void *cpage, const WCHAR *adam_id)
{
    struct find_ctx f = { adam_id, 0, NULL };
    page_walk(cpage, find_cb, &f, 0);
    return f.found;
}

IInspectable *page_list(void *cpage, const WCHAR *want)
{
    struct find_ctx f = { want, 1, NULL };
    page_walk(cpage, find_cb, &f, 1);
    return f.found;
}

HRESULT open_deeplink(const WCHAR *url)
{
    void *act = NULL; HSTRING h; HRESULT hr;
    if (!g_main_vm) return E_UNEXPECTED;
    h = hs(url);   /* the callee keeps its own copy (WinRT convention) */
    hr = ((HRESULT (WINAPI *)(void *, HSTRING, boolean, void **))VT(g_main_vm)[31])(g_main_vm, h, FALSE, &act);
    WindowsDeleteString(h);
    return hr;
}

HRESULT dispatch_action(IInspectable *action)
{
    static void *naf;
    void *nargs = NULL, *nc = NULL, *act = NULL;
    HRESULT hr = E_FAIL;
    if (!naf) naf = get_statics(L"AMP.Services.NotificationArgs", &IID_INotificationArgsFactory);
    if (naf) hr = ((HRESULT (WINAPI *)(void *, UINT32, void *, void **))VT(naf)[6])(naf, EVENT_DISPATCH, action, &nargs);
    if (FAILED(hr) || !nargs) return FAILED(hr) ? hr : E_FAIL;
    CALL1(g_sp, 32, &nc);                    /* ServiceProvider.NotificationCenter */
    if (nc) hr = ((HRESULT (WINAPI *)(void *, void *, void **))VT(nc)[6])(nc, nargs, &act);   /* PostAsync (started, not awaited) */
    else hr = E_FAIL;
    if (act) IUnknown_Release((IUnknown *)act);
    if (nc) IUnknown_Release((IUnknown *)nc);
    IUnknown_Release((IUnknown *)nargs);
    return hr;
}
