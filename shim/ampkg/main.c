/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ampkg: patching machinery and DllMain (overview: ampkg.h). */
#include "ampkg.h"

WINE_DEFAULT_DEBUG_CHANNEL(ampkg);

static const struct hook *const hook_tables[] = { identity_hooks, compat_hooks, audio_hooks, crypto_hooks };

static void *find_hook(const char *name)
{
    const struct hook *h;
    unsigned int i;
    void *fn;
    if ((ULONG_PTR)name <= 0xffff) return NULL;
    if ((fn = applog_hook(name)) || (fn = handles_hook(name))) return fn;
    for (i = 0; i < ARRAY_SIZE(hook_tables); i++)
        for (h = hook_tables[i]; h->name; h++)
            if (!strcmp(h->name, name)) return h->fn;
    return NULL;
}

static HMODULE self;

static void patch_thunks(BYTE *base, IMAGE_THUNK_DATA64 *names, IMAGE_THUNK_DATA64 *iat)
{
    for (; names->u1.AddressOfData; names++, iat++)
    {
        IMAGE_IMPORT_BY_NAME *ibn;
        void *fn;
        DWORD old;
        if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
        ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
        if (!(fn = find_hook((const char *)ibn->Name))) continue;
        if (iat->u1.Function == (ULONG_PTR)fn) continue;
        VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
        iat->u1.Function = (ULONG_PTR)fn;
        VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
    }
}

static void patch_module(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY *dir;

    if (mod == self) return;
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;

    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir->VirtualAddress && dir->Size)
    {
        IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
        for (; imp->Name; imp++)
        {
            if (!imp->FirstThunk) continue;
            patch_thunks(base, (IMAGE_THUNK_DATA64 *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk)),
                         (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk));
        }
    }
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (dir->VirtualAddress && dir->Size)
    {
        IMAGE_DELAYLOAD_DESCRIPTOR *d = (IMAGE_DELAYLOAD_DESCRIPTOR *)(base + dir->VirtualAddress);
        for (; d->DllNameRVA; d++)
        {
            if (!d->ImportNameTableRVA || !d->ImportAddressTableRVA) continue;
            patch_thunks(base, (IMAGE_THUNK_DATA64 *)(base + d->ImportNameTableRVA),
                         (IMAGE_THUNK_DATA64 *)(base + d->ImportAddressTableRVA));
        }
    }
}

static void patch_all_modules(void)
{
    LIST_ENTRY *head = &NtCurrentTeb()->Peb->LdrData->InLoadOrderModuleList, *e;
    for (e = head->Flink; e != head; e = e->Flink)
    {
        LDR_DATA_TABLE_ENTRY *m = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
        patch_module(m->DllBase);
    }
}

/* LdrRegisterDllNotification */
static void CALLBACK dll_notify(ULONG reason, LDR_DLL_NOTIFICATION_DATA *data, void *ctx)
{
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED) return;
    TRACE("loaded %s\n", debugstr_wn(data->Loaded.BaseDllName->Buffer, data->Loaded.BaseDllName->Length / 2));
    patch_module(data->Loaded.DllBase);
}

/* GetProcAddress hot-patch ------------------------------------------ */

static FARPROC WINAPI my_GetProcAddress(HMODULE mod, const char *name)
{
    void *fn = find_hook(name);
    ANSI_STRING str;
    void *ret;
    NTSTATUS status;

    if (fn) { TRACE("hooked %s\n", name); return fn; }
    if ((ULONG_PTR)name >> 16)
    {
        RtlInitAnsiString(&str, name);
        status = LdrGetProcedureAddress(mod, &str, 0, &ret);
    }
    else status = LdrGetProcedureAddress(mod, NULL, LOWORD(name), &ret);
    if (status)
    {
        SetLastError(RtlNtStatusToDosError(status));
        return NULL;
    }
    /* keep patching modules that resolve things dynamically after load */
    return ret;
}

static void hotpatch(void *target, void *dest)
{
    BYTE code[12] = { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 }; /* mov rax, imm64; jmp rax */
    DWORD old;
    ULONG64 img;
    RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry((ULONG64)target, &img, NULL);

    if (rf && rf->EndAddress - rf->BeginAddress < sizeof(code))
    {
        ERR("function %p too small to hot-patch\n", target);
        return;
    }
    memcpy(code + 2, &dest, 8);
    VirtualProtect(target, sizeof(code), PAGE_EXECUTE_READWRITE, &old);
    memcpy(target, code, sizeof(code));
    VirtualProtect(target, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(code));
}

static void init(HMODULE mod)
{
    void *cookie;
    unsigned int i;
    static const WCHAR *dlls[] = { L"kernelbase.dll", L"kernel32.dll" };

    self = mod;
    identity_init();

    /* hot-patch existing implementations so every caller sees the package */
    for (i = 0; i < ARRAY_SIZE(dlls); i++)
    {
        HMODULE h = GetModuleHandleW(dlls[i]);
        unsigned int j;
        if (!h) continue;
        for (j = 0; j < ARRAY_SIZE(hook_tables); j++)
        {
            const struct hook *hk;
            for (hk = hook_tables[j]; hk->name; hk++)
            {
                ANSI_STRING str;
                void *fn;
                RtlInitAnsiString(&str, hk->name);
                if (!LdrGetProcedureAddress(h, &str, 0, &fn) && i == 0) hotpatch(fn, hk->fn);
            }
        }
    }
    {
        ANSI_STRING str;
        void *fn, *prev = NULL;
        RtlInitAnsiString(&str, "GetProcAddress");
        for (i = 0; i < ARRAY_SIZE(dlls); i++)
        {
            HMODULE h = GetModuleHandleW(dlls[i]);
            if (h && !LdrGetProcedureAddress(h, &str, 0, &fn) && fn != prev && fn != my_GetProcAddress)
                hotpatch(fn, my_GetProcAddress);
            prev = fn;
        }
    }
    patch_all_modules();
    LdrRegisterDllNotification(0, dll_notify, NULL, &cookie);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        applog_init();
        handles_init();
        init(inst);
    }
    return TRUE;
}

/* exported so it can be referenced from an import table */
void WINAPI ampkg_init(void) {}
