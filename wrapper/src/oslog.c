/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Debug aid: show the app's own log messages (AMP.Utils.dll os_log_with_type_impl*, ASL.dll
 * asl_log) by redirecting those imports in every loaded module (see oslog.h). */
#include <stdarg.h>
#include "common.h"
#include "iat.h"
#include "oslog.h"

struct oslog_sv { const char *ptr; size_t len; };   /* std::string_view, passed by reference on x64 */

static int oslog_contains(const char *s, const char *word)   /* ASCII case-insensitive strstr */
{
    size_t n = strlen(word);
    for (; *s; s++) if (!_strnicmp(s, word, n)) return 1;
    return 0;
}

/* Log arguments are private by default (they can carry account data). They are expanded when
 * AM_OSLOG_ARGS is set (debugging runs), and otherwise only for failure/status messages, with
 * anything that looks like an e-mail address or a long number masked. */
static void oslog_format(char *msg, size_t size, const char *f, va_list ap, int formatted)
{
    int all = GetEnvironmentVariableA("AM_OSLOG_ARGS", NULL, 0) != 0;
    int diag = oslog_contains(f, "fail") || oslog_contains(f, "error") || oslog_contains(f, "status") || oslog_contains(f, "ADIOTP");
    /* assertions: "%{public}s: (%{public}s)" with "Assertion failure" and the source expression */
    if (formatted && !diag && !strcmp(f, "%s: (%s)"))
    {
        va_list aq;
        const char *first;
        va_copy(aq, ap);
        first = va_arg(aq, const char *);
        va_end(aq);
        __try { diag = first && !strcmp(first, "Assertion failure"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { diag = 0; }
    }
    if (formatted && (all || diag) && !strstr(f, "%@") && !oslog_contains(f, "assword"))
    {
        __try { vsnprintf(msg, size, f, ap); }
        __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(msg, size, "%s (unformatted)", f); }
        msg[size - 1] = 0;
        if (!all)
        {
            char *p, *q;
            for (p = msg; (p = strchr(p, '@')); p++)
            {
                for (q = p; q > msg && !strchr(" \t'\"<>(),;:=", q[-1]); q--) ;
                for (; *q && !strchr(" \t'\"<>(),;:", *q); q++) *q = '#';
                p = q - 1;
                if (!*q) break;
            }
            for (p = msg; *p; p++)
            {
                for (q = p; *q >= '0' && *q <= '9'; q++) ;
                if (q - p >= 9) while (p < q) *p++ = '#';
                if (q > p) p = q - 1;
            }
        }
    }
    else snprintf(msg, size, "%s", f);
}

/* Apple System Log (ASL.dll asl_log), used by AuthKitWin and CFNetwork; nothing collects it
 * in the wrapper process otherwise */
static int oslog_asl_log(void *client, void *aslmsg, int level, const char *format, ...)
{
    char msg[3000];
    va_list ap;
    va_start(ap, format);
    oslog_format(msg, sizeof(msg), format ? format : "", ap, 1);
    va_end(ap);
    out("asl[%d]: %s", level, msg);
    return 0;
}

volatile LONG oslog_agent_lost;

static void oslog_emit(int type, struct oslog_sv *fmt, va_list ap, int formatted)
{
    char f[1024], msg[3000];
    size_t i, n = 0;
    /* Apple annotations: "%{public}s" -> "%s" */
    for (i = 0; i < fmt->len && n < sizeof(f) - 1; i++)
    {
        f[n++] = fmt->ptr[i];
        if (fmt->ptr[i] == '%' && i + 1 < fmt->len && fmt->ptr[i + 1] == '{')
            while (i + 1 < fmt->len && fmt->ptr[i] != '}') i++;
    }
    f[n] = 0;
    oslog_format(msg, sizeof(msg), f, ap, formatted);
    out("oslog[%d]: %s", type, msg);
    /* the app does not relaunch its agent; the daemon exits on this so that it gets restarted */
    if (strstr(f, "exited unexpectedly") || strstr(f, "Agent failure") ||
        (strstr(msg, "create_instance(") && strstr(msg, "IAMPLibrary") && strstr(msg, "failed")))
        oslog_agent_lost = 1;
}

static void oslog_impl(int log, int type, struct oslog_sv *fmt, ...)
{
    va_list ap; va_start(ap, fmt); oslog_emit(type, fmt, ap, 1); va_end(ap);
}
static void oslog_impl_noformat(int log, int type, struct oslog_sv *fmt)
{
    va_list ap; memset(&ap, 0, sizeof(ap)); oslog_emit(type, fmt, ap, 0);
}

void oslog_hook_all(void)
{
    iat_patch_all("ASL.dll", "asl_log", oslog_asl_log);
    iat_patch_all("AMP.Utils.dll", "?os_log_with_type_impl_noformat@@*", oslog_impl_noformat);
    iat_patch_all("AMP.Utils.dll", "?os_log_with_type_impl@@*", oslog_impl);
}
