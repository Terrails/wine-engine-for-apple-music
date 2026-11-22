/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The engine's play queue (PlayQueue.Current): sections (history, up next, autoplay...) of
 * PlayQueueItem component models. */
#pragma once
#include "common.h"
#include "page.h"

/* call cb for every item of every queue section; page_item.section is the section index */
void queue_walk(page_item_cb cb, void *ctx, int log_sections);
/* PlayQueueItemID of a queue item (0 if it is none) */
UINT64 queue_item_id(IInspectable *item);
/* PlayQueue.PlayPlayQueueItemAsync: jump to that item */
HRESULT queue_play_item(UINT64 id);
/* make the queue's sections available (they stay empty until the app's queue panel is open) */
void queue_open(void);
/* the highest PlayQueueItemID in the queue now (new items get higher ones) */
UINT64 queue_max_id(void);
/* PlayQueueSectionKind */
enum { QUEUE_HISTORY = 1, QUEUE_CURRENT = 2, QUEUE_NEXT_ADDED = 3, QUEUE_NEXT = 4, QUEUE_AUTOPLAY = 5 };
/* PlayQueueItemID of the first item for `adam_id` whose ID is above `after` (0 if none), and
 * the kind of section it is in */
UINT64 queue_find(const WCHAR *adam_id, UINT64 after, INT32 *kind);
