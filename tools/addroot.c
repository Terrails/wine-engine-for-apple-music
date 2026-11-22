/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Add a DER certificate to the Wine prefix's machine "Root" store.
 * usage (inside the prefix): addroot.exe C:\path\cert.cer */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>

int wmain(int argc, WCHAR **argv)
{
    BYTE buf[8192]; DWORD len = 0; HANDLE f; HCERTSTORE store; BOOL ok;
    if (argc < 2) { printf("usage: addroot <cert.cer>\n"); return 2; }
    f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE || !ReadFile(f, buf, sizeof(buf), &len, NULL)) { printf("cannot read %ls (%lu)\n", argv[1], GetLastError()); return 1; }
    CloseHandle(f);
    store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Root");
    if (!store) { printf("cannot open Root store (%lu)\n", GetLastError()); return 1; }
    ok = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, buf, len, CERT_STORE_ADD_REPLACE_EXISTING, NULL);
    printf("add %lu bytes to LocalMachine\\Root: %s (%lu)\n", len, ok ? "ok" : "FAILED", ok ? 0 : GetLastError());
    CertCloseStore(store, 0);
    return ok ? 0 : 1;
}
