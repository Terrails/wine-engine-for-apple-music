/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Shared definitions for the wrapper: logging, WinRT helpers and raw vtable calls.
 *
 * AMP.Services' interfaces have no headers, so methods are called by vtable slot
 * (WinRT slot = 6 + method index; slots are listed in docs/architecture.md and by `tools/q`). */
#pragma once
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* raw vtable access: VT(o)[slot] is the slot's function pointer */
#define VT(o) (*(void ***)(o))
#define CALL0(o, slot) ((HRESULT (WINAPI *)(void *))VT(o)[slot])(o)
#define CALL1(o, slot, a) ((HRESULT (WINAPI *)(void *, void *))VT(o)[slot])(o, (void *)(a))

/* ---- log.c ---- */
/* one line on stderr (written in one piece, so it does not interleave with Wine's output) */
void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* wrapper log lines carry this prefix; grep for it in $AM_LOG_DIR/amd.log */
#define LOG(fmt, ...) out("wrapper: " fmt, ##__VA_ARGS__)
/* "<what> -> <hresult>" */
void say(const char *what, HRESULT hr);

/* ---- winrt.c ---- */
HSTRING hs(const WCHAR *s);               /* new HSTRING; the caller deletes it (one-shot code may not) */
const char *ws(HSTRING h);                /* HSTRING -> UTF-8, valid until 8 more calls */
const WCHAR *wz(HSTRING h);               /* HSTRING -> WCHAR*, "" for NULL */
int is_classic_iid(REFIID iid);           /* {xxxxxxxx-0000-0000-C000-000000000046} */
/* activation factory of an AMP.Services class (DllGetActivationFactory of AMP.Services.dll) */
HRESULT get_factory(const WCHAR *cls, IActivationFactory **factory);
/* factory of an AMP.Services class, queried for its statics/factory interface; NULL on failure */
void *get_statics(const WCHAR *cls, REFIID iid);
/* wait for an IAsyncOperation/IAsyncAction while pumping messages; returns the AsyncStatus
 * (0 still running, 1 completed, 2 canceled, 3 error) and the error code in *err */
INT32 await_async(void *op, DWORD step_ms, int max_steps, HRESULT *err);

/* IInspectable methods for objects that do not describe themselves */
HRESULT WINAPI insp_GetIids(void *This, ULONG *n, IID **iids);
HRESULT WINAPI insp_GetRuntimeClassName(void *This, HSTRING *name);
HRESULT WINAPI insp_GetTrustLevel(void *This, TrustLevel *level);

extern const GUID IID_IInspectable_, IID_IAgileObject_, IID_IAsyncInfo_, IID_IWeakReference_,
                  IID_IWeakReferenceSource_, IID_IActivationFactory_;

/* ---- app.c ---- */
extern void *g_sp;        /* IServiceProvider */
extern void *g_main_vm;   /* IMainPageViewModel (NULL until loaded) */
/* run the message loop for `ms`, also serving queued UI and API requests */
void pump(DWORD ms);
