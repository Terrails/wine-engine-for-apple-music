/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bringing up the app's services layer (AMP.Services) without its UI. */
#pragma once
#include "common.h"

/* load AMP.Services and start it the way AppleMusic.exe does, up to the main page (which makes
 * the app launch AMPLibraryAgent). settle_ms: extra time to let the app work after the
 * asynchronous steps (0 for the service; experiments use a few seconds). Returns 0 on success. */
int app_start(DWORD settle_ms);
/* wait until the agent-backed playback service has started (up to timeout_ms); 0 if it did not
 * or the agent went away */
int app_wait_playback(DWORD timeout_ms);
