/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Minimal bcp47mrm.dll: BCP-47 tag syntax check used by MRT Core. */
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>

/* tag = subtag *("-" subtag), subtag = 1*8 alphanumerics, first subtag alphabetic */
BOOL WINAPI IsWellFormedTag(const WCHAR *tag)
{
    int len = 0, first = 1;
    const WCHAR *p;
    if (!tag || !*tag) return FALSE;
    for (p = tag; ; p++)
    {
        if (!*p || *p == '-' || *p == '_')
        {
            if (len < 1 || len > 8) return FALSE;
            if (!*p) return TRUE;
            len = 0;
            first = 0;
            continue;
        }
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (first == 0 && *p >= '0' && *p <= '9')))
            return FALSE;
        len++;
    }
}
