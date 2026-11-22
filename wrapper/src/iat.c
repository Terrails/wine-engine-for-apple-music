/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Import redirection (see iat.h). */
#include <windows.h>
#include <psapi.h>
#include <string.h>
#include "iat.h"

static int name_matches(const char *name, const char *pattern)
{
    size_t n = strlen(pattern);
    if (n && pattern[n - 1] == '*') return !strncmp(name, pattern, n - 1);
    return !strcmp(name, pattern);
}

int iat_patch(HMODULE mod, const char *dll, const char *func, void *fn)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int n = 0;
    if (!dir->VirtualAddress) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++)
    {
        IMAGE_THUNK_DATA64 *names, *iat;
        if (_stricmp((char *)base + imp->Name, dll) || !imp->OriginalFirstThunk) continue;
        names = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk);
        iat = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++)
        {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            if (!name_matches((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, func)) continue;
            VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            iat->u1.Function = (ULONG_PTR)fn;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            n++;
        }
    }
    return n;
}

int iat_patch_all(const char *dll, const char *func, void *fn)
{
    HMODULE mods[1024]; DWORD needed = 0, i; int n = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    for (i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) n += iat_patch(mods[i], dll, func, fn);
    return n;
}
