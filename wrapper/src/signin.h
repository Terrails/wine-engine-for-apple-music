/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Apple Account sign-in. */
#pragma once
#include "common.h"

/* the app's sign-in dialog: ask for the credentials with a native prompt, hand them to the
 * app's SignInHandler, repeat on failure. Returns the AuthResult (0 Ok, 1 Canceled). */
INT32 signin_dialog(IUnknown *handler, HSTRING title, HSTRING message, HSTRING explanation, HSTRING user);
/* AccountViewModel.SignedIn */
int account_signed_in(void);
/* interactive sign-in (amd.exe signin): start AccountViewModel.SignInAsync and wait for it;
 * when already signed in, only report the account state. Returns 0 when signed in. */
int signin_run(void);
