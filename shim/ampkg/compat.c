/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg: functions Wine lacks or gets wrong for this app (see ampkg.h). */
#include "ampkg.h"

WINE_DEFAULT_DEBUG_CHANNEL(ampkg);

/* DispatcherQueue: implemented in amshim.dll */
typedef struct { DWORD dwSize; DWORD threadType; DWORD apartmentType; } DQ_OPTIONS;
static HRESULT WINAPI my_CreateDispatcherQueueController(DQ_OPTIONS options, void **controller)
{
    static HRESULT (WINAPI *fn)(DQ_OPTIONS, void **);
    if (!fn)
    {
        HMODULE h = LoadLibraryW(L"amshim.dll");
        ANSI_STRING str;
        RtlInitAnsiString(&str, "CreateDispatcherQueueController");
        if (!h || LdrGetProcedureAddress(h, &str, 0, (void **)&fn)) return E_NOTIMPL;
    }
    return fn(options, controller);
}

/* SHGetKnownFolderPath: Wine rejects the package-redirection flags (KF_FLAG_NO_PACKAGE_REDIRECTION
 * and friends) with E_INVALIDARG and a NULL path, which callers do not expect. There is no
 * package redirection here, so drop them. */
static HRESULT WINAPI my_SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, WCHAR **path)
{
    static HRESULT (WINAPI *fn)(const GUID *, DWORD, HANDLE, WCHAR **);
    if (!fn)
    {
        HMODULE h = LoadLibraryW(L"shell32.dll");
        ANSI_STRING str;
        RtlInitAnsiString(&str, "SHGetKnownFolderPath");
        if (!h || LdrGetProcedureAddress(h, &str, 0, (void **)&fn)) return E_NOTIMPL;
    }
    return fn(id, flags & ~0x00070000, token, path);
}

/* dcomp.dll!DCompositionCreateSurfaceHandle is unimplemented in Wine (calling it aborts the
 * process). The agent calls it when it creates a player, to get a composition surface for the
 * video layer, even for audio-only playback. Hand out a placeholder handle; video presentation
 * to that surface cannot work (no DirectComposition), audio does not need it. */
static HRESULT WINAPI my_DCompositionCreateSurfaceHandle(DWORD access, SECURITY_ATTRIBUTES *sa, HANDLE *handle)
{
    HANDLE h = CreateEventW(sa, TRUE, FALSE, NULL);
    FIXME("access %#lx -> placeholder handle %p\n", access, h);
    if (!handle) return E_INVALIDARG;
    *handle = h;
    return h ? S_OK : E_FAIL;
}

/* combase.dll!RoTransformError is unimplemented in Wine, and calling an unimplemented export
 * aborts the process. WIL calls it on error paths (e.g. when a cross-process call fails), which
 * turned a recoverable error into an agent crash. It only attaches error info; report success. */
static BOOL WINAPI my_RoTransformError(HRESULT old_error, HRESULT new_error, void *message)
{
    FIXME("%#lx -> %#lx: stub\n", old_error, new_error);
    return TRUE;
}

const struct hook compat_hooks[] =
{
    HOOK(CreateDispatcherQueueController),
    HOOK(SHGetKnownFolderPath),
    HOOK(DCompositionCreateSurfaceHandle),
    HOOK(RoTransformError),
    { NULL }
};
