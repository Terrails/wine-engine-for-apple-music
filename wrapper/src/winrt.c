/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Small WinRT helpers: strings, factories of AMP.Services classes, waiting for async results. */
#include "common.h"

const GUID IID_IInspectable_ = {0xaf86e2e0,0xb12d,0x4c6a,{0x9c,0x5a,0xd7,0xaa,0x65,0x10,0x1e,0x90}};
const GUID IID_IAgileObject_ = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
const GUID IID_IActivationFactory_ = {0x00000035,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
const GUID IID_IAsyncInfo_ = {0x00000036,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
const GUID IID_IWeakReference_ = {0x00000037,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
const GUID IID_IWeakReferenceSource_ = {0x00000038,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

HSTRING hs(const WCHAR *s)
{
    HSTRING h;
    WindowsCreateString(s, lstrlenW(s), &h);
    return h;
}

const char *ws(HSTRING h)
{
    static char bufs[8][2048]; static LONG idx;
    char *b = bufs[InterlockedIncrement(&idx) & 7];
    b[0] = 0;
    if (h) WideCharToMultiByte(CP_UTF8, 0, WindowsGetStringRawBuffer(h, NULL), -1, b, sizeof(bufs[0]) - 1, NULL, NULL);
    return b;
}

const WCHAR *wz(HSTRING h) { return h ? WindowsGetStringRawBuffer(h, NULL) : L""; }

int is_classic_iid(REFIID iid)
{
    static const BYTE tail[8] = {0xc0,0,0,0,0,0,0,0x46};
    return !iid->Data2 && !iid->Data3 && !memcmp(iid->Data4, tail, 8);
}

HRESULT get_factory(const WCHAR *cls, IActivationFactory **factory)
{
    static HRESULT (WINAPI *dll_get_factory)(HSTRING, IActivationFactory **);
    *factory = NULL;
    if (!dll_get_factory)
    {
        HMODULE m = GetModuleHandleA("AMP.Services.dll");
        if (!m || !(*(FARPROC *)&dll_get_factory = GetProcAddress(m, "DllGetActivationFactory"))) return E_FAIL;
    }
    {
        HSTRING h = hs(cls);
        HRESULT hr = dll_get_factory(h, factory);
        WindowsDeleteString(h);
        return hr;
    }
}

void *get_statics(const WCHAR *cls, REFIID iid)
{
    IActivationFactory *f = NULL; void *statics = NULL;
    if (SUCCEEDED(get_factory(cls, &f)) && f)
    {
        IActivationFactory_QueryInterface(f, iid, &statics);
        IActivationFactory_Release(f);
    }
    return statics;
}

INT32 await_async(void *op, DWORD step_ms, int max_steps, HRESULT *err)
{
    void *info = NULL; INT32 status = 0;
    *err = S_OK;
    if (!op || FAILED(IUnknown_QueryInterface((IUnknown *)op, &IID_IAsyncInfo_, &info))) return 0;
    for (int i = 0; i < max_steps && !status; i++) { pump(step_ms); CALL1(info, 7, &status); }   /* get_Status */
    CALL1(info, 8, err);                                                                         /* get_ErrorCode */
    IUnknown_Release((IUnknown *)info);
    return status;
}

HRESULT WINAPI insp_GetIids(void *This, ULONG *n, IID **iids) { *n = 0; *iids = NULL; return S_OK; }
HRESULT WINAPI insp_GetRuntimeClassName(void *This, HSTRING *name) { *name = NULL; return S_OK; }
HRESULT WINAPI insp_GetTrustLevel(void *This, TrustLevel *level) { *level = BaseTrust; return S_OK; }
