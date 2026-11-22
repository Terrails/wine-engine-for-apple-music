/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The app's own log (os_log, asl_log) on stderr as "oslog[type]:" / "asl[level]:" lines.
 * Arguments are private by default; AM_OSLOG_ARGS=1 expands all of them (debugging only). */
#pragma once
#include <windows.h>

/* redirect the log imports of every loaded module; call after loading AMP.Services.dll */
void oslog_hook_all(void);
/* set when the app reports that AMPLibraryAgent is gone (it never relaunches it) */
extern volatile LONG oslog_agent_lost;
