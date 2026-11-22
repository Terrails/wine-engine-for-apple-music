/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Debug aid: print every MSVC C++ exception thrown in the process (type names, HRESULT or
 * what(), and a short module+offset backtrace), and a backtrace for crashes and breakpoints. */
#include "common.h"
#include "cxxdbg.h"

static void cxxdbg_where(void *addr, char *out, size_t n)
{
    HMODULE mod = NULL; char path[MAX_PATH] = "?"; const char *base;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, addr, &mod) && mod)
        GetModuleFileNameA(mod, path, sizeof(path));
    base = strrchr(path, '\\'); base = base ? base + 1 : path;
    snprintf(out, n, "%s+0x%llx", base, (unsigned long long)((char *)addr - (char *)mod));
}

static LONG CALLBACK cxxdbg_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT || r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {   /* DebugBreak() before abort() (CoreFoundation HALT, asserts) or a crash: say where */
        void *frames[40]; char w[300], msg[3500]; USHORT nf, f; int n;
        n = snprintf(msg, sizeof(msg), "FATAL exception %08lx tid %04lx, backtrace:", r->ExceptionCode, GetCurrentThreadId());
        nf = CaptureStackBackTrace(0, 40, frames, NULL);
        for (f = 0; f < nf && n < 3000; f++)
        {
            cxxdbg_where(frames[f], w, sizeof(w));
            n += snprintf(msg + n, sizeof(msg) - n, " %s", w);
        }
        out("%s", msg);
    }
    if (r->ExceptionCode == 0xE06D7363 && r->NumberParameters >= 4 && r->ExceptionInformation[2])
    {
        char *obj = (char *)r->ExceptionInformation[1];
        char *base = (char *)r->ExceptionInformation[3];
        DWORD *ti = (DWORD *)r->ExceptionInformation[2];
        DWORD *arr = (DWORD *)(base + ti[3]);
        void *frames[24]; char w[300], msg[3500]; USHORT nf, f;
        unsigned int i; int n = 0, webview = 0;
        n += snprintf(msg + n, sizeof(msg) - n, "CXX throw:");
        for (i = 0; i < arr[0] && n < 1000; i++)
        {
            DWORD *ct = (DWORD *)(base + arr[1 + i]);
            const char *name = base + ct[1] + 0x10;
            n += snprintf(msg + n, sizeof(msg) - n, " %s", name);
            if (!i && strstr(name, "hresult_")) n += snprintf(msg + n, sizeof(msg) - n, " [hr=%08x]", *(unsigned int *)(obj + 0xc));
            if (!strcmp(name, ".?AVexception@std@@"))
            {
                const char *(*what)(void *) = (*(void ***)obj)[1];
                n += snprintf(msg + n, sizeof(msg) - n, " [what=%.200s]", what(obj));
            }
        }
        n += snprintf(msg + n, sizeof(msg) - n, "\n   at");
        nf = CaptureStackBackTrace(0, 24, frames, NULL);
        for (f = 0; f < nf && n < 3000; f++)
        {
            cxxdbg_where(frames[f], w, sizeof(w));
            if (strstr(w, "WebView2")) webview = 1;
            if (strncmp(w, "ntdll", 5) && strncmp(w, "kernelbase", 10) && strncmp(w, "VCRUNTIME", 9) && !strstr(w, ".exe+") )
                n += snprintf(msg + n, sizeof(msg) - n, " %s", w);
        }
        /* the WebView2 runtime probe fails thousands of times; its rethrows are dropped too */
        {
            static __thread int last_was_webview;
            if (webview) last_was_webview = 1;
            else if (last_was_webview && strstr(msg, "hr=80070002")) { webview = 1; last_was_webview = 0; }
            else last_was_webview = 0;
        }
        if (!webview) out("%s", msg);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void cxxdbg_install(void) { AddVectoredExceptionHandler(1, cxxdbg_handler); }
