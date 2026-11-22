/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg, optional (AM_HANDLE_DEBUG=1): report CloseHandle calls that fail, i.e. a module closing a handle
 * it does not own (any more). A double close is harmless on its own but, once the value has been
 * reused, closes or leaves dangling somebody else's handle. */
#include "ampkg.h"

static int handle_debug;

/* Events created by combase (it caches one per RPC channel and waits on it for every outgoing call):
 * remember them, to see whether another module closes one behind its back. */
static HANDLE com_events[1024];
static HMODULE combase_mod;
static HANDLE (WINAPI *real_CreateEventW)(SECURITY_ATTRIBUTES *, BOOL, BOOL, const WCHAR *);

static BOOL from_combase(void *ra)
{
    HMODULE mod = NULL;
    if (!combase_mod) combase_mod = GetModuleHandleW(L"combase.dll");
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ra, &mod)
           && mod == combase_mod;
}

static HANDLE WINAPI my_CreateEventW(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL state, const WCHAR *name)
{
    HANDLE h = real_CreateEventW(sa, manual, state, name);
    if (h && !manual && !name && from_combase(__builtin_return_address(0)))
    {
        unsigned int i;
        static LONG count;
        for (i = 0; i < ARRAY_SIZE(com_events); i++)
            if (!InterlockedCompareExchangePointer(&com_events[i], h, NULL)) break;
        if (!(InterlockedIncrement(&count) % 500)) MESSAGE("ampkg: %ld COM channel events created so far\n", count);
    }
    return h;
}

static void report_foreign(const char *what, HANDLE h, void *ra)
{
    HMODULE mod = NULL;
    WCHAR name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ra, &mod))
        GetModuleFileNameW(mod, name, MAX_PATH);
    MESSAGE("ampkg: %s(%p) on a COM channel event, tid %04lx from %ls+%#Ix\n", what, h,
            GetCurrentThreadId(), wcsrchr(name, '\\') ? wcsrchr(name, '\\') + 1 : name, (ULONG_PTR)ra - (ULONG_PTR)mod);
}

static BOOL WINAPI my_SetEvent(HANDLE h)
{
    unsigned int i;
    for (i = 0; h && i < ARRAY_SIZE(com_events); i++)
        if (com_events[i] == h)
        {
            if (!from_combase(__builtin_return_address(0))) report_foreign("SetEvent", h, __builtin_return_address(0));
            break;
        }
    return !NtSetEvent(h, NULL);
}

static BOOL WINAPI my_CloseHandle(HANDLE h)
{
    NTSTATUS st;
    BOOL ret;
    unsigned int i;
    for (i = 0; h && i < ARRAY_SIZE(com_events); i++)
    {
        if (com_events[i] != h) continue;
        if (from_combase(__builtin_return_address(0))) { InterlockedCompareExchangePointer(&com_events[i], NULL, h); break; }
        {
            void *ra = __builtin_return_address(0);
            HMODULE mod = NULL;
            WCHAR name[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ra, &mod))
                GetModuleFileNameW(mod, name, MAX_PATH);
            MESSAGE("ampkg: CloseHandle(%p) of a COM channel event, tid %04lx from %ls+%#Ix\n", h,
                    GetCurrentThreadId(), wcsrchr(name, '\\') ? wcsrchr(name, '\\') + 1 : name, (ULONG_PTR)ra - (ULONG_PTR)mod);
        }
        break;
    }
    st = NtClose(h);      /* not CloseHandle: our own imports are hooked too */
    ret = !st;
    if (!ret)
    {
        SetLastError(RtlNtStatusToDosError(st));
        void *ra = __builtin_return_address(0);
        HMODULE mod = NULL;
        WCHAR name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, ra, &mod))
            GetModuleFileNameW(mod, name, MAX_PATH);
        MESSAGE("ampkg: CloseHandle(%p) FAILED (status %#lx) tid %04lx from %ls+%#Ix\n", h, st,
                GetCurrentThreadId(), wcsrchr(name, '\\') ? wcsrchr(name, '\\') + 1 : name, (ULONG_PTR)ra - (ULONG_PTR)mod);
    }
    return ret;
}

void handles_init(void)
{
    ANSI_STRING str;
    void *fn = NULL;
    if (!(handle_debug = GetEnvironmentVariableA("AM_HANDLE_DEBUG", NULL, 0) != 0)) return;
    RtlInitAnsiString(&str, "CreateEventW");
    if (!LdrGetProcedureAddress(GetModuleHandleW(L"kernelbase.dll"), &str, 0, &fn)) real_CreateEventW = fn;
}

void *handles_hook(const char *name)
{
    if (!handle_debug) return NULL;
    if (!strcmp(name, "CloseHandle")) return my_CloseHandle;
    if (!strcmp(name, "SetEvent")) return my_SetEvent;
    if (real_CreateEventW && !strcmp(name, "CreateEventW")) return my_CreateEventW;
    return NULL;
}
