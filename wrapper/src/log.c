/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Logging: every line goes to stderr in a single WriteFile. */
#include <stdarg.h>
#include "common.h"

void out(const char *fmt, ...)
{
    char buf[4096]; va_list ap; DWORD n; int len;
    buf[0] = '\n';
    va_start(ap, fmt); len = vsnprintf(buf + 1, sizeof(buf) - 2, fmt, ap); va_end(ap);
    if (len < 0 || len > (int)sizeof(buf) - 24) len = sizeof(buf) - 24;
    {   /* AM_TS=1: append milliseconds since the first message, for timing startup */
        static DWORD t0; static int ts = -1;
        if (ts < 0) { ts = GetEnvironmentVariableA("AM_TS", NULL, 0) != 0; t0 = GetTickCount(); }
        if (ts) len += snprintf(buf + 1 + len, 20, "  @%lums", GetTickCount() - t0);
    }
    buf[len + 1] = '\n';
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, len + 2, &n, NULL);
}

void say(const char *what, HRESULT hr) { LOG("%s -> %08lx", what, hr); }
