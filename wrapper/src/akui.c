/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* AuthKit UI provider for the wrapper.
 *
 * AuthKitWin.dll asks an AKUIProvider object (a C++ class it exports; the real UI subclasses it
 * in AMP.UI.Views) for anything interactive during sign-in. AMP.Services installs the UI's
 * provider with SetUIProvider() right before authenticating; in the wrapper there is none, so
 * its SetUIProvider import is redirected to always install ours.
 *
 * vtable (from AuthKitWin's base vtable and AMP.UI.Views' AuthControl::AuthUIProvider):
 *   0 destructor                       4 PresentServerProvidedUI(ctx, url, dict, dict, cb, cbctx)
 *   1 HWND GetParentWindow()           5 DismissServerProvidedUI(ctx, CFError*, CFError**)
 *   2 SetCompletionHandler (base)      6 CFDictionary *Get2FACode(info, CFError **err)  (blocks)
 *   3 ShowAlert(?, ?, title, message, ?) -> NULL   7 Set2FACode(CFDictionary*)
 * Get2FACode returns { AKSecondFactorCodeKey: CFString code }, or sets *err on cancel.
 * (Interface: akui.h.) */
#include "common.h"
#include "akui.h"
#include "iat.h"
#include "prompt.h"

struct ak_provider { const void **vtbl; BYTE base[0x800]; void *code_dict; };
static struct ak_provider ak_provider;
static const void *ak_vtbl[8];

static struct
{
    void *(*CFDictionaryCreateMutable)(void *, INT_PTR, const void *, const void *);
    void (*CFDictionarySetValue)(void *, const void *, const void *);
    void *(*CFStringCreateWithCString)(void *, const char *, UINT32);
    BOOLEAN (*CFStringGetCString)(void *, char *, INT_PTR, UINT32);
    void (*CFRelease)(void *);
    void **kCFAllocatorDefault;
    const void *key_callbacks, *value_callbacks;
    void **AKSecondFactorCodeKey;
    void *(*AKCreateAuthenticationError)(int);
    void (*SetUIProvider)(void *);
} ak;

#define CF_UTF8 0x08000100

static void ak_cfstr(void *s, char *buf, int size)
{
    buf[0] = 0;
    if (s) { __try { ak.CFStringGetCString(s, buf, size, CF_UTF8); } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; } }
}

static HWND ak_GetParentWindow(struct ak_provider *p) { return NULL; }

/* returns a pointer the caller treats as a CF object (the real UI returns NULL) */
static void *ak_ShowAlert(struct ak_provider *p, void *a, void *b, void *title, void *message, void *c)
{
    char t[512], m[1024];
    WCHAR wt[512], wm[1024];
    struct prompt q = {0};
    ak_cfstr(title, t, sizeof(t)); ak_cfstr(message, m, sizeof(m));
    LOG("AuthKit alert: title='%s' message='%s'", t, m);
    MultiByteToWideChar(CP_UTF8, 0, t, -1, wt, ARRAY_SIZE(wt));
    MultiByteToWideChar(CP_UTF8, 0, m, -1, wm, ARRAY_SIZE(wm));
    q.message = wt; q.explanation = wm; q.n_buttons = 1; q.buttons[0] = L"OK";
    prompt_run(&q);
    return NULL;
}

static void *ak_PresentServerProvidedUI(struct ak_provider *p, void *ctx, void *url, void *d1, void *d2,
                                       void (*cb)(int, void *, void *), void *cbctx)
{
    char u[1024];
    ak_cfstr(url, u, sizeof(u));
    /* a web page AuthKit wants shown (needs a WebView): not supported yet. The URL can carry
     * session data, so only its host/path prefix is logged. */
    u[80] = 0;
    LOG("AuthKit wants server-provided web UI (unsupported): %s...", u);
    return NULL;
}

static void *ak_DismissServerProvidedUI(struct ak_provider *p, void *ctx, void *err, void **out_err)
{
    LOG("AuthKit dismisses server-provided UI");
    return NULL;
}

static void *ak_Get2FACode(struct ak_provider *p, void *info, void **err)
{
    struct prompt q = {0};
    char code[64];
    void *dict, *str;

    LOG("AuthKit asks for the two-factor verification code");
    q.title = L"Apple Music";
    q.message = L"Two-Factor Authentication";
    q.explanation = L"A message with a verification code has been sent to your devices. Enter the code to continue.";
    q.n_edits = 1; q.edit_label[0] = L"Verification code";
    q.n_buttons = 2; q.buttons[0] = L"Continue"; q.buttons[1] = L"Cancel";
    if (prompt_run(&q) != 0 || !q.edit_text[0][0])
    {
        LOG("two-factor prompt: cancelled");
        if (err && ak.AKCreateAuthenticationError) *err = ak.AKCreateAuthenticationError(-7003);   /* what the real UI reports on cancel */
        return NULL;
    }
    WideCharToMultiByte(CP_UTF8, 0, q.edit_text[0], -1, code, sizeof(code), NULL, NULL);
    SecureZeroMemory(q.edit_text, sizeof(q.edit_text));
    dict = ak.CFDictionaryCreateMutable(*ak.kCFAllocatorDefault, 0, ak.key_callbacks, ak.value_callbacks);
    str = ak.CFStringCreateWithCString(*ak.kCFAllocatorDefault, code, CF_UTF8);
    SecureZeroMemory(code, sizeof(code));
    ak.CFDictionarySetValue(dict, *ak.AKSecondFactorCodeKey, str);
    ak.CFRelease(str);
    p->code_dict = dict;
    LOG("two-factor prompt: code submitted");
    return dict;
}

static void ak_Set2FACode(struct ak_provider *p, void *dict) { p->code_dict = dict; }

void akui_log_enable(void)
{
    WCHAR exe[MAX_PATH], *name, *dot;
    HKEY key;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    name = wcsrchr(exe, '\\') ? wcsrchr(exe, '\\') + 1 : exe;
    if ((dot = wcsrchr(name, '.'))) *dot = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Apple Inc.\\Internet Services\\Logging", 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL)) return;
    if (RegQueryValueExW(key, name, NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND)
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)L"error", sizeof(L"error"));
    RegCloseKey(key);
}

/* AMP.Services' SetUIProvider(x): always install ours (it passes the UI's provider, which we
 * do not have, and NULL afterwards) */
static void ak_set_ui_provider_hook(void *ignored)
{
    ak.SetUIProvider(&ak_provider);
}

int akui_install(void)
{
    HMODULE akm = GetModuleHandleA("AuthKitWin.dll"), cf = GetModuleHandleA("CoreFoundation.dll");
    void (*ctor)(void *);
    const void **base_vtbl;
    if (!akm || !cf) return 0;
#define CFSYM(n) (*(void **)&ak.n = (void *)GetProcAddress(cf, #n))
    CFSYM(CFDictionaryCreateMutable); CFSYM(CFDictionarySetValue); CFSYM(CFStringCreateWithCString);
    CFSYM(CFStringGetCString); CFSYM(CFRelease); CFSYM(kCFAllocatorDefault);
#undef CFSYM
    ak.key_callbacks = (void *)GetProcAddress(cf, "kCFTypeDictionaryKeyCallBacks");
    ak.value_callbacks = (void *)GetProcAddress(cf, "kCFTypeDictionaryValueCallBacks");
    ak.AKSecondFactorCodeKey = (void *)GetProcAddress(akm, "AKSecondFactorCodeKey");
    *(void **)&ak.AKCreateAuthenticationError = (void *)GetProcAddress(akm, "?AKCreateAuthenticationError@@YAPEAU__CFError@@W4AKAppleIDAuthenticationError@@@Z");
    *(void **)&ak.SetUIProvider = (void *)GetProcAddress(akm, "?SetUIProvider@@YAXPEAVAKUIProvider@@@Z");
    ctor = (void *)GetProcAddress(akm, "??0AKUIProvider@@QEAA@XZ");
    if (!ctor || !ak.SetUIProvider || !ak.AKSecondFactorCodeKey || !ak.CFDictionaryCreateMutable || !ak.CFDictionarySetValue ||
        !ak.CFStringCreateWithCString || !ak.CFStringGetCString || !ak.CFRelease || !ak.kCFAllocatorDefault ||
        !ak.key_callbacks || !ak.value_callbacks) return 0;
    ctor(&ak_provider);                       /* base-class constructor; then swap in our vtable */
    base_vtbl = ak_provider.vtbl;
    ak_vtbl[0] = base_vtbl[0];
    ak_vtbl[1] = ak_GetParentWindow;
    ak_vtbl[2] = base_vtbl[2];
    ak_vtbl[3] = ak_ShowAlert;
    ak_vtbl[4] = ak_PresentServerProvidedUI;
    ak_vtbl[5] = ak_DismissServerProvidedUI;
    ak_vtbl[6] = ak_Get2FACode;
    ak_vtbl[7] = ak_Set2FACode;
    ak_provider.vtbl = ak_vtbl;
    ak.SetUIProvider(&ak_provider);
    return iat_patch(GetModuleHandleA("AMP.Services.dll"), "AuthKitWin.dll", "?SetUIProvider@@YAXPEAVAKUIProvider@@@Z", ak_set_ui_provider_hook);
}
