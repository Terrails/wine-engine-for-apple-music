/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The app's page model: navigation by deep link, walking a page's items, running actions. */
#pragma once
#include "common.h"

struct page_item
{
    UINT32 section, index;
    IUnknown *container;       /* the section the item is in */
    IInspectable *item;        /* the component model */
    HSTRING cls, title, adam;  /* runtime class, ITitledComponent.Title, ContentDescriptor.AdamId */
    IInspectable *action;      /* IPlayActionDriven.PlayAction (or NULL) */
    void *cd;                  /* IContentDescriptor (or NULL) */
};
/* return nonzero to stop the walk */
typedef int (*page_item_cb)(const struct page_item *it, void *ctx);

/* current page of the root navigation stack as IContentPageViewModel (or NULL) */
void *current_content_page(void);
/* IContentPageViewModel state: IsLoading, HasError, Page (IPage, NULL until there is one) */
void page_state(void *cpage, boolean *loading, boolean *error, void **page);
/* call cb for every item of one section (an IComponentModelContainer, e.g. a page section or a
 * play-queue section); returns nonzero when cb stopped the walk */
int container_walk(IUnknown *section, UINT32 si, page_item_cb cb, void *ctx, int log_sections);
/* the same for a vector of component models (IVector<IComponentModel>) */
int items_walk(IUnknown *items, UINT32 si, page_item_cb cb, void *ctx);
/* call cb for every item of every section of the page; log_sections: log a line per section */
void page_walk(void *cpage, page_item_cb cb, void *ctx, int log_sections);
/* play action of the item whose AdamId is `adam_id` (quiet) */
IInspectable *find_play_action(void *cpage, const WCHAR *adam_id);
/* log the sections and their first items; returns the play action of the first item whose
 * AdamId equals `want` or whose class name or title contains it (want may be NULL) */
IInspectable *page_list(void *cpage, const WCHAR *want);

/* MainPageViewModel.ProcessDeeplinkAsync(url): navigate the page model, e.g. to
 * musics://music.apple.com/<cc>/album/<albumId>?i=<songId> */
HRESULT open_deeplink(const WCHAR *url);
/* run an (opaque) action the way the UI does: post a 'disp' notification carrying it */
HRESULT dispatch_action(IInspectable *action);
