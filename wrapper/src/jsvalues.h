/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Frees the JavaScript value wrappers the app leaves queued (see jsvalues.c). */
#pragma once

/* call after AMP.Services.dll is loaded and before the ServiceProvider starts (which creates the
 * JavaScript contexts) */
void jsvalues_install(void);
