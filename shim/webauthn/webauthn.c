/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Minimal webauthn.dll: no platform authenticator available. */
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>

DWORD WINAPI WebAuthNGetApiVersionNumber(void) { return 1; }

HRESULT WINAPI WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable(BOOL *available)
{
    *available = FALSE;
    return S_OK;
}

HRESULT WINAPI WebAuthNAuthenticatorGetAssertion(HWND hwnd, const WCHAR *rpid, const void *client_data,
                                                 const void *options, void **assertion)
{
    if (assertion) *assertion = NULL;
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

void WINAPI WebAuthNFreeAssertion(void *assertion) {}

const WCHAR * WINAPI WebAuthNGetErrorName(HRESULT hr) { return L"NotSupportedError"; }
