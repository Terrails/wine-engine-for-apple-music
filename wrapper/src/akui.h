/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* AuthKit UI provider: native prompts for the interactive steps of sign-in (2FA code, alerts). */
#pragma once

/* install it; call after AMP.Services.dll (and with it AuthKitWin.dll, CoreFoundation.dll) is
 * loaded. Returns the number of SetUIProvider imports redirected (0 = not installed). */
int akui_install(void);

/* Turn on AuthKitWin's own log at error level unless already configured (call before
 * AuthKitWin.dll loads). AuthKitWin reads HKCU\Software\Apple Inc.\Internet Services\Logging,
 * value <exe name without .exe> = none|error|warn|info|debug|verbose, and writes
 * %LOCALAPPDATA%\Packages\AppleInc.AppleMusicWin_nzyj5cx40ttqa\LocalCache\Local\Logs\
 * AuthKitWin.<date>.log. "error" records why a sign-in failed (e.g. the HTTP error behind
 * "error connecting to the Apple Account server") without account data; info and debug can log
 * the Apple Account name. */
void akui_log_enable(void);
