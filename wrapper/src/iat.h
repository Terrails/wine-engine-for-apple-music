/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Import redirection in already loaded modules. */
#pragma once
#include <windows.h>

/* point `mod`'s imports of `func` from `dll` at `fn`; a trailing '*' in `func` matches any name
 * starting with the rest. Returns the number of entries patched. */
int iat_patch(HMODULE mod, const char *dll, const char *func, void *fn);
/* the same in every module loaded in the process */
int iat_patch_all(const char *dll, const char *func, void *fn);
