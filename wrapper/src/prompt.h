/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A small native prompt window for requests the app sends to its UI (sign-in, message boxes):
 * message + explanation, up to two edit fields and up to three buttons. Modal: runs its own
 * message loop on the calling thread. Text typed here goes only to the caller.
 * AM_DIALOG_AUTO=1 (tests): press the last button after 1.5 s. */
#pragma once
#include <windows.h>

struct prompt
{
    const WCHAR *title, *message, *explanation, *error;
    int n_edits;
    const WCHAR *edit_label[2];
    int edit_password[2];
    WCHAR edit_text[2][256];         /* in: initial text, out: what was typed */
    int n_buttons;
    const WCHAR *buttons[3];         /* the last button doubles as "close/cancel" */
    int default_button;
    int result;                      /* out: index of the button pressed */
    /* internal */
    HWND edits[2];
    int done;
};

/* returns the index of the button pressed */
int prompt_run(struct prompt *p);
