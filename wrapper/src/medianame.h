/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Now-playing name for the mixer (see medianame.c and pulse/ampulse.c). */
#pragma once
#include "common.h"

/* start writing %LOCALAPPDATA%\AppleMusicWrapper\media-name (amd.exe serve only) */
void media_name_init(void);
/* called from pump(): refresh the file at most every 500 ms; no-op before media_name_init() */
void media_name_tick(void);
