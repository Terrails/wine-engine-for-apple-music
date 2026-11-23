/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* JavaScriptCoreV8.dll (the app's JavaScriptCore API on top of V8) does not delete the wrappers of
 * values V8 never collects (numbers, booleans, null, ...) when they are released: it queues them
 * in chunks of 1500 and deletes one chunk per V8 garbage collection. An album page queues about 15
 * chunks and V8 collects 3-4 times meanwhile, so the queue only grows. Each of those small blocks
 * also keeps a group of 31 in Wine's heap from being reused, which makes it about 15 MB per page.
 *
 * So the library's own collection callback is run until only KEEP_CHUNKS are left. The newest stay:
 * the delay is what keeps a value that was just released usable for a moment. */
#include "app.h"
#include "iat.h"
#include "jsvalues.h"

#define KEEP_CHUNKS  64        /* about 4 album pages */
#define MAX_CHUNKS   100000    /* more than this is not a chunk count */
#define COUNT_OFFSET 0x78      /* context group: number of queued chunks (app version 1.1792.39941.0) */

typedef void (*gc_callback)(void *isolate, int type, int flags, void *group);
static const char add_callback_name[] =
    "?AddGCEpilogueCallback@Isolate@v8@@QEAAXP6AXPEAV12@W4GCType@2@W4GCCallbackFlags@2@PEAX@Z31@Z";
static void (*add_callback)(void *isolate, gc_callback cb, void *group, int filter);
static gc_callback app_callback;
static LONG disabled;

/* called by V8 with the isolate locked, like the library's callback */
static void on_gc(void *isolate, int type, int flags, void *group)
{
    volatile SIZE_T *count = (SIZE_T *)((char *)group + COUNT_OFFSET);
    SIZE_T before;
    do
    {
        before = disabled || !group ? 0 : *count;
        app_callback(isolate, type, flags, group);
        if (!before) return;
        /* it deletes exactly one chunk; anything else and this is not the count */
        if (before > MAX_CHUNKS || *count != before - 1)
        {
            if (!InterlockedExchange(&disabled, 1)) LOG("jsvalues: unexpected chunk count %Iu -> %Iu, left alone", before, *count);
            return;
        }
    } while (*count > KEEP_CHUNKS);
}

static void on_add_callback(void *isolate, gc_callback cb, void *group, int filter)
{
    if (app_callback && app_callback != cb) { add_callback(isolate, cb, group, filter); return; }
    app_callback = cb;
    add_callback(isolate, on_gc, group, filter);
}

void jsvalues_install(void)
{
    HMODULE js = GetModuleHandleA("JavaScriptCoreV8.dll"), v8 = GetModuleHandleA("v8.dll");
    int n = 0;
    if (js && v8 && (add_callback = (void *)GetProcAddress(v8, add_callback_name)))
        n = iat_patch(js, "v8.dll", add_callback_name, on_add_callback);
    LOG("jsvalues: collection callback hooked: %d", n);
}
