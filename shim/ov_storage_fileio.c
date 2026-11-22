/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Storage: FileIO (overview: amshim_storage.h). */
#include "amshim_storage.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- FileIO (statics): synchronous Win32 file I/O behind already-completed async results.
 * `file` must be one of our StorageFile objects. Text is UTF-8. ---- */
static HRESULT fileio_write(void *file, const void *data, DWORD len, BOOL append, void **result)
{
    const WCHAR *path = file ? path_of(impl_from_inspectable(file)) : L"";
    HANDLE h = CreateFileW(path, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HRESULT hr = S_OK;
    DWORD done;
    if (h == INVALID_HANDLE_VALUE) hr = HRESULT_FROM_WIN32(GetLastError());
    else
    {
        if (len && !WriteFile(h, data, len, &done, NULL)) hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(h);
    }
    TRACE("%s %lu bytes -> %#lx\n", debugstr_w(path), len, hr);
    return async_create(&ASYNC_IAsyncAction, ASYNC_ACTION, hr, PK_PRIM, NULL, 0, result);
}

static HRESULT fileio_read(void *file, BYTE **data, DWORD *len)
{
    const WCHAR *path = file ? path_of(impl_from_inspectable(file)) : L"";
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HRESULT hr = S_OK;
    *data = NULL; *len = 0;
    if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    *len = GetFileSize(h, NULL);
    *data = malloc(*len + 1);
    if (*len && !ReadFile(h, *data, *len, len, NULL)) hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(h);
    TRACE("%s %lu bytes -> %#lx\n", debugstr_w(path), *len, hr);
    return hr;
}

static HRESULT fileio_write_text(void *file, HSTRING contents, BOOL append, void **result)
{
    UINT32 wlen;
    const WCHAR *w = WindowsGetStringRawBuffer(contents, &wlen);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    char *buf = malloc(n + 1);
    HRESULT hr;
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, buf, n, NULL, NULL);
    hr = fileio_write(file, buf, n, append, result);
    free(buf);
    return hr;
}

HRESULT OV_IFileIOStatics_WriteBytesAsync(struct obj *o, void *file, UINT32 buffer_len, UINT8 *buffer, void **result)
{
    return fileio_write(file, buffer, buffer_len, FALSE, result);
}

HRESULT OV_IFileIOStatics_WriteBufferAsync(struct obj *o, void *file, void *buffer, void **result)
{
    BYTE *data = NULL;
    UINT32 len = 0, cap = 0;
    if (buffer) buffer_bytes(buffer, &data, &len, &cap);
    return fileio_write(file, data, len, FALSE, result);
}

HRESULT OV_IFileIOStatics_WriteTextAsync(struct obj *o, void *file, HSTRING contents, void **result)
{
    return fileio_write_text(file, contents, FALSE, result);
}
HRESULT OV_IFileIOStatics_WriteTextAsync_2(struct obj *o, void *file, HSTRING contents, INT32 encoding, void **result)
{
    return fileio_write_text(file, contents, FALSE, result);
}
HRESULT OV_IFileIOStatics_AppendTextAsync(struct obj *o, void *file, HSTRING contents, void **result)
{
    return fileio_write_text(file, contents, TRUE, result);
}
HRESULT OV_IFileIOStatics_AppendTextAsync_2(struct obj *o, void *file, HSTRING contents, INT32 encoding, void **result)
{
    return fileio_write_text(file, contents, TRUE, result);
}

HRESULT OV_IFileIOStatics_ReadBufferAsync(struct obj *o, void *file, void **result)
{
    BYTE *data;
    DWORD len;
    IInspectable *buf = NULL;
    HRESULT hr = fileio_read(file, &data, &len);
    if (SUCCEEDED(hr)) hr = byte_buffer_create(data, len, len, &buf);
    free(data);
    hr = async_create(&ASYNC_IAsyncOperation_IBuffer_, ASYNC_OP, hr, PK_OBJECT, buf ? &buf : NULL, buf ? sizeof(buf) : 0, result);
    if (buf) IInspectable_Release(buf);
    return hr;
}

HRESULT OV_IFileIOStatics_ReadTextAsync(struct obj *o, void *file, void **result)
{
    BYTE *data;
    DWORD len;
    HSTRING str = NULL;
    HRESULT hr = fileio_read(file, &data, &len);
    if (SUCCEEDED(hr))
    {
        const char *p = (const char *)data;
        int n;
        WCHAR *w;
        if (len >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) { p += 3; len -= 3; }
        n = MultiByteToWideChar(CP_UTF8, 0, p, len, NULL, 0);
        w = malloc((n + 1) * sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, p, len, w, n);
        hr = WindowsCreateString(w, n, &str);
        free(w);
    }
    free(data);
    hr = async_create(&ASYNC_IAsyncOperation_String_, ASYNC_OP, hr, PK_STRING, &str, sizeof(str), result);
    WindowsDeleteString(str);
    return hr;
}
HRESULT OV_IFileIOStatics_ReadTextAsync_2(struct obj *o, void *file, INT32 encoding, void **result)
{
    return OV_IFileIOStatics_ReadTextAsync(o, file, result);
}
