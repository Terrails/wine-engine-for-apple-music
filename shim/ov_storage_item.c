/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Storage: item properties, rename/move/copy (overview: amshim_storage.h). */
#include "amshim_storage.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- IStorageItem::GetBasicPropertiesAsync: size and modification time from the file ---- */
HRESULT OV_IStorageItem_GetBasicPropertiesAsync(struct obj *o, void **result)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    IInspectable *props = NULL;
    HRESULT hr = S_OK;

    if (!GetFileAttributesExW(path_of(o), GetFileExInfoStandard, &fad)) hr = HRESULT_FROM_WIN32(GetLastError());
    else if (SUCCEEDED(hr = create_object_by_name(L"Windows.Storage.FileProperties.BasicProperties", &props)))
    {
        struct obj *p = impl_from_inspectable(props);
        UINT64 size = ((UINT64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        /* Windows.Foundation.DateTime.UniversalTime uses the FILETIME epoch and unit */
        INT64 mtime = ((INT64)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
        prop_put(p, L"Size", PK_PRIM, &size, sizeof(size));
        prop_put(p, L"DateModified", PK_STRUCT, &mtime, sizeof(mtime));
        prop_put(p, L"ItemDate", PK_STRUCT, &mtime, sizeof(mtime));
        TRACE("%s size %I64u\n", debugstr_w(path_of(o)), size);
    }
    hr = async_create(&ASYNC_IAsyncOperation_BasicProperties_, ASYNC_OP, hr, PK_OBJECT, props ? &props : NULL,
                      props ? sizeof(props) : 0, result);
    if (props) IInspectable_Release(props);
    return hr;
}

/* ---- rename / move / copy (synchronous Win32 calls behind completed async actions) ----
 * NameCollisionOption: 0 GenerateUniqueName, 1 ReplaceExisting, 2 FailIfExists */
static HRESULT file_transfer(struct obj *o, const WCHAR *dir, const WCHAR *name, INT32 option, BOOL copy, void **result)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH], *slash;
    BOOL is_dir = (GetFileAttributesW(path_of(o)) & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesW(path_of(o)) != INVALID_FILE_ATTRIBUTES;
    HRESULT hr = S_OK;
    int n;

    lstrcpynW(src, path_of(o), ARRAY_SIZE(src));
    if (dir) lstrcpynW(dst, dir, ARRAY_SIZE(dst));
    else
    {
        lstrcpynW(dst, src, ARRAY_SIZE(dst));
        if ((slash = wcsrchr(dst, '\\'))) *slash = 0;
    }
    if (!name || !*name) name = wcsrchr(src, '\\') ? wcsrchr(src, '\\') + 1 : src;
    n = wcslen(dst);
    swprintf(dst + n, ARRAY_SIZE(dst) - n, L"\\%s", name);

    if (option == 0 && GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES && wcsicmp(src, dst))
    {
        /* "name (2).ext", "name (3).ext", ... */
        const WCHAR *ext = wcsrchr(name, '.');
        int stem = ext ? (int)(ext - name) : (int)wcslen(name), i;
        for (i = 2; i < 1000; i++)
        {
            swprintf(dst + n, ARRAY_SIZE(dst) - n, L"\\%.*s (%d)%s", stem, name, i, ext ? ext : L"");
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) break;
        }
    }
    if (wcsicmp(src, dst))
    {
        BOOL ok = copy ? CopyFileW(src, dst, option != 1)
                       : MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED | (option == 1 ? MOVEFILE_REPLACE_EXISTING : 0));
        if (!ok) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    TRACE("%s %s -> %s option %d: %#lx\n", copy ? "copy" : "move", debugstr_w(src), debugstr_w(dst), option, hr);
    if (copy)
    {
        IInspectable *file = NULL;
        if (SUCCEEDED(hr)) hr = storage_file_create(dst, &file);
        hr = async_create(&ASYNC_IAsyncOperation_StorageFile_, ASYNC_OP, hr, PK_OBJECT, file ? &file : NULL, file ? sizeof(file) : 0, result);
        if (file) IInspectable_Release(file);
        return hr;
    }
    if (SUCCEEDED(hr)) set_item_props(o, dst, is_dir);
    return async_create(&ASYNC_IAsyncAction, ASYNC_ACTION, hr, PK_PRIM, NULL, 0, result);
}

static const WCHAR *folder_path(void *folder) { return folder ? path_of(impl_from_inspectable(folder)) : NULL; }

HRESULT OV_IStorageItem_RenameAsync(struct obj *o, HSTRING name, void **result)
{ return file_transfer(o, NULL, WindowsGetStringRawBuffer(name, NULL), 2, FALSE, result); }
HRESULT OV_IStorageItem_RenameAsync_2(struct obj *o, HSTRING name, INT32 option, void **result)
{ return file_transfer(o, NULL, WindowsGetStringRawBuffer(name, NULL), option, FALSE, result); }
HRESULT OV_IStorageFile_MoveAsync(struct obj *o, void *folder, void **result)
{ return file_transfer(o, folder_path(folder), NULL, 2, FALSE, result); }
HRESULT OV_IStorageFile_MoveAsync_2(struct obj *o, void *folder, HSTRING name, void **result)
{ return file_transfer(o, folder_path(folder), WindowsGetStringRawBuffer(name, NULL), 2, FALSE, result); }
HRESULT OV_IStorageFile_MoveAsync_3(struct obj *o, void *folder, HSTRING name, INT32 option, void **result)
{ return file_transfer(o, folder_path(folder), WindowsGetStringRawBuffer(name, NULL), option, FALSE, result); }
HRESULT OV_IStorageFile_CopyAsync(struct obj *o, void *folder, void **result)
{ return file_transfer(o, folder_path(folder), NULL, 2, TRUE, result); }
HRESULT OV_IStorageFile_CopyAsync_2(struct obj *o, void *folder, HSTRING name, void **result)
{ return file_transfer(o, folder_path(folder), WindowsGetStringRawBuffer(name, NULL), 2, TRUE, result); }
HRESULT OV_IStorageFile_CopyAsync_3(struct obj *o, void *folder, HSTRING name, INT32 option, void **result)
{ return file_transfer(o, folder_path(folder), WindowsGetStringRawBuffer(name, NULL), option, TRUE, result); }
