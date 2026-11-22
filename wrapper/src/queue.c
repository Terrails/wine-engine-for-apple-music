/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The engine's play queue (see queue.h). */
#include "queue.h"

static const GUID IID_IPlayQueueStatics = {0x1cf4c1e2,0x8da2,0x5642,{0x91,0x1b,0x6f,0x68,0xd6,0x9d,0x9f,0x68}};
static const GUID IID_IPlayQueue = {0x5da4729b,0x8546,0x5210,{0xb8,0xa2,0x06,0x6b,0x11,0xd6,0x8e,0x8d}};
static const GUID IID_IPlayQueueItem = {0x72fe9b2f,0x3de1,0x5517,{0x8f,0x3d,0xeb,0xaa,0xfa,0xfd,0x25,0x52}};
static const GUID IID_IPlayQueueSection = {0xd05888fd,0x5cd9,0x5db6,{0x98,0xde,0xd9,0xc1,0x4f,0x17,0x52,0x9b}};
static const GUID IID_IVector_IComponentModel = {0x2c559b4a,0x4165,0x5e91,{0x86,0xf1,0xcd,0x3e,0xc0,0x88,0x44,0x19}};

/* IPlayQueue of PlayQueue.Current, or NULL */
static void *play_queue(void)
{
    void *statics = get_statics(L"AMP.Services.PlayQueue", &IID_IPlayQueueStatics), *q = NULL;
    IUnknown *cur = NULL;
    if (statics) CALL1(statics, 6, &cur);            /* Current */
    if (cur)
    {
        IUnknown_QueryInterface(cur, &IID_IPlayQueue, &q);
        IUnknown_Release(cur);
    }
    return q;
}

void queue_walk(page_item_cb cb, void *ctx, int log_sections)
{
    void *q = play_queue(), *vec = NULL; IUnknown *sections = NULL; UINT32 n = 0;
    if (!q) { LOG("QUEUE: no play queue"); return; }
    CALL1(q, 9, &sections);                          /* Sections: IObservableVector<IComponentModel> */
    if (sections) IUnknown_QueryInterface(sections, &IID_IVector_IComponentModel, &vec);
    if (vec) CALL1(vec, 7, &n);
    if (log_sections) LOG("QUEUE sections=%u", n);
    for (UINT32 si = 0; vec && si < n; si++)
    {
        IUnknown *sec = NULL;
        ((HRESULT (WINAPI *)(void *, UINT32, void **))VT(vec)[6])(vec, si, (void **)&sec);
        if (sec && container_walk(sec, si, cb, ctx, log_sections)) break;
    }
}

UINT64 queue_item_id(IInspectable *item)
{
    void *qi = NULL; UINT64 id = 0;
    if (item && SUCCEEDED(IInspectable_QueryInterface(item, &IID_IPlayQueueItem, &qi)))
    {
        CALL1(qi, 6, &id);                           /* PlayQueueItemID */
        IUnknown_Release((IUnknown *)qi);
    }
    return id;
}

HRESULT queue_play_item(UINT64 id)
{
    void *q = play_queue(), *act = NULL;
    if (!q) return E_FAIL;
    return ((HRESULT (WINAPI *)(void *, UINT64, void **))VT(q)[13])(q, id, &act);   /* PlayPlayQueueItemAsync */
}

void queue_open(void)
{
    static IInspectable *vm;   /* kept for the life of the process */
    IActivationFactory *f = NULL;
    if (vm || !g_main_vm) return;
    /* what showing the "Up Next" panel does: MainPageViewModel.IsPlayQueueOpen = true and a
     * PlayQueueViewModel for the panel; without them PlayQueue.Sections stays empty */
    ((HRESULT (WINAPI *)(void *, boolean))VT(g_main_vm)[16])(g_main_vm, TRUE);
    if (SUCCEEDED(get_factory(L"AMP.Services.CommonModels.PlayQueueViewModel", &f)) && f)
    {
        IActivationFactory_ActivateInstance(f, &vm);
        IActivationFactory_Release(f);
    }
    LOG("play queue view model %p", vm);
}

struct qfind { const WCHAR *adam; UINT64 after, id, max; INT32 kind; };

static INT32 section_kind(IUnknown *section)
{
    void *s = NULL; INT32 kind = 0;
    if (section && SUCCEEDED(IUnknown_QueryInterface(section, &IID_IPlayQueueSection, &s)))
    {
        CALL1(s, 6, &kind);                          /* SectionKind */
        IUnknown_Release((IUnknown *)s);
    }
    return kind;
}

static int qfind_cb(const struct page_item *it, void *ctx)
{
    struct qfind *f = ctx;
    UINT64 id = queue_item_id(it->item);
    if (id > f->max) f->max = id;
    if (f->adam && !f->id && id > f->after && !wcscmp(wz(it->adam), f->adam))
    {
        f->id = id;
        f->kind = section_kind(it->container);
    }
    return 0;
}

UINT64 queue_max_id(void)
{
    struct qfind f = { NULL, 0, 0, 0, 0 };
    queue_walk(qfind_cb, &f, 0);
    return f.max;
}

UINT64 queue_find(const WCHAR *adam_id, UINT64 after, INT32 *kind)
{
    struct qfind f = { adam_id, after, 0, 0, 0 };
    queue_walk(qfind_cb, &f, 0);
    *kind = f.kind;
    return f.id;
}
