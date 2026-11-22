/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg.dll internals: shared includes and the pieces each source file contributes.
 *
 * ampkg is loaded as the first import of AMPLibraryAgent.exe (added by tools/addimport.py) and
 * by the wrapper. It patches the import tables of all loaded modules (and of modules loaded
 * later), hot-patches kernelbase and hooks GetProcAddress, so that the functions listed in the
 * hook tables below are replaced everywhere:
 *   identity.c   fake MSIX package identity (appmodel APIs)
 *   compat.c     functions Wine lacks or gets wrong for this app
 *   crypto.c     CryptoAPI fixes for the push service's client certificate (key handle, key storage)
 *   audio.c      IAudioClient lifetime fixes (event handle, leaked session controls)
 *   audio_idle.c the agent's silent start-up stream kept without a real stream
 *   applog.c     optional: the process's os_log on the Wine log (AM_AGENT_OSLOG=1)
 *   handles.c    optional: handle misuse diagnostics (AM_HANDLE_DEBUG=1)
 *   main.c       the patching machinery, DllMain */
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winternl.h>
#include <appmodel.h>
#include <wingdi.h>
#include <mmreg.h>
#include "wine/debug.h"

/* a replaced function: imports and GetProcAddress lookups of `name` get `fn` */
struct hook { const char *name; void *fn; };
#define HOOK(x) { #x, my_##x }

/* tables end with { NULL } */
extern const struct hook identity_hooks[], compat_hooks[], audio_hooks[], crypto_hooks[];

/* identity.c: derive package path and application ID from the executable */
void identity_init(void);

/* applog.c / handles.c: optional hooks, decided by name (NULL = not hooked) */
void applog_init(void);
void *applog_hook(const char *name);
void handles_init(void);
void *handles_hook(const char *name);

/* audio.c <-> audio_idle.c: the real IAudioClient methods, and the idle client's side */
extern ULONG (WINAPI *real_ac_Release)(void *);
extern HRESULT (WINAPI *real_ac_SetEventHandle)(void *, HANDLE);
extern HRESULT (WINAPI *real_ac_GetService)(void *, const GUID *, void **);
/* install the idle client's methods into the IAudioClient vtable (saving the real ones) */
void idle_patch_vtbl(void **vtbl);
/* each returns FALSE when the client is not (or no longer) the quiet one: then call the real method */
BOOL qc_SetEventHandle(void *client, HANDLE event, HRESULT *hr);
BOOL qc_GetService(void *client, const GUID *iid, void **obj, HRESULT *hr);
/* the client is gone (its final Release returned 0) */
void qc_released(void *client);
