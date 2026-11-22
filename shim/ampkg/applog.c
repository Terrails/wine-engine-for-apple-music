/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg, optional (AM_AGENT_OSLOG=1): show the process's own os_log messages (AMP.Utils.dll) in the
 * Wine log as "applog[type]: ...". Arguments are private data unless marked {public}, so they
 * are only expanded when AM_OSLOG_ARGS is also set (debugging runs). */
#include "ampkg.h"

struct oslog_sv { const char *ptr; size_t len; };   /* std::string_view, by reference on x64 */
static int oslog_enabled, oslog_args;

static void oslog_emit(int type, struct oslog_sv *fmt, va_list ap, int formatted)
{
    char f[1024], msg[3000];
    size_t i, n = 0;
    for (i = 0; i < fmt->len && n < sizeof(f) - 1; i++)
    {
        f[n++] = fmt->ptr[i];
        if (fmt->ptr[i] == '%' && i + 1 < fmt->len && fmt->ptr[i + 1] == '{')
            while (i + 1 < fmt->len && fmt->ptr[i] != '}') i++;      /* "%{public}s" -> "%s" */
    }
    f[n] = 0;
    if (formatted && oslog_args && !strstr(f, "%@"))
    {
        __try { vsnprintf(msg, sizeof(msg), f, ap); }
        __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(msg, sizeof(msg), "%s (unformatted)", f); }
        msg[sizeof(msg) - 1] = 0;
    }
    else snprintf(msg, sizeof(msg), "%s", f);
    MESSAGE("applog[%d]: %s\n", type, msg);
}

static void my_os_log_impl(int log, int type, struct oslog_sv *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    oslog_emit(type, fmt, ap, 1);
    va_end(ap);
}

static void my_os_log_impl_noformat(int log, int type, struct oslog_sv *fmt)
{
    va_list ap;
    memset(&ap, 0, sizeof(ap));
    oslog_emit(type, fmt, ap, 0);
}

void applog_init(void)
{
    oslog_enabled = GetEnvironmentVariableA("AM_AGENT_OSLOG", NULL, 0) != 0;
    oslog_args = GetEnvironmentVariableA("AM_OSLOG_ARGS", NULL, 0) != 0;
}

void *applog_hook(const char *name)
{
    if (!oslog_enabled || strncmp(name, "?os_log_with_type_impl", 22)) return NULL;
    return !strncmp(name, "?os_log_with_type_impl_noformat@@", 33) ? (void *)my_os_log_impl_noformat : (void *)my_os_log_impl;
}
