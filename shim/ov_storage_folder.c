/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Storage: StorageFolder (overview: amshim_storage.h). */
#include "amshim_storage.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ------------------------------------------------------------------ */
/* StorageFolder                                                       */

static void child_path(struct obj *o, HSTRING name, WCHAR *out)
{
    const WCHAR *n = WindowsGetStringRawBuffer(name, NULL);
    WCHAR *p;
    swprintf(out, MAX_PATH, L"%s\\%s", path_of(o), n);
    for (p = out; *p; p++) if (*p == '/') *p = '\\';
}

static HRESULT not_found_async(const struct iface_entry *e, void **result)
{
    return async_create(e, ASYNC_OP, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), PK_OBJECT, NULL, 0, result);
}

static HRESULT obj_async(const struct iface_entry *e, IInspectable *item, void **result)
{
    HRESULT hr = async_create(e, ASYNC_OP, S_OK, PK_OBJECT, &item, sizeof(item), result);
    if (item) IInspectable_Release(item);
    return hr;
}

/* CreationCollisionOption: 0 GenerateUniqueName, 1 ReplaceExisting, 2 FailIfExists, 3 OpenIfExists */
HRESULT OV_IStorageFolder_CreateFolderAsync_2(struct obj *o, HSTRING name, INT32 options, void **result)
{
    WCHAR path[MAX_PATH];
    IInspectable *f;
    DWORD attr;
    child_path(o, name, path);
    TRACE("%s options %d\n", debugstr_w(path), options);
    attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && options == 2)
        return async_create(&ASYNC_IAsyncOperation_StorageFolder_, ASYNC_OP, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                            PK_OBJECT, NULL, 0, result);
    if (FAILED(folder_for(path, &f))) return E_FAIL;
    return obj_async(&ASYNC_IAsyncOperation_StorageFolder_, f, result);
}

HRESULT OV_IStorageFolder_CreateFolderAsync(struct obj *o, HSTRING name, void **result)
{
    return OV_IStorageFolder_CreateFolderAsync_2(o, name, 2, result);
}

HRESULT OV_IStorageFolder_CreateFileAsync_2(struct obj *o, HSTRING name, INT32 options, void **result)
{
    WCHAR path[MAX_PATH];
    IInspectable *f;
    HANDLE h;
    DWORD disp = options == 1 ? CREATE_ALWAYS : options == 3 ? OPEN_ALWAYS : CREATE_NEW;
    child_path(o, name, path);
    TRACE("%s options %d\n", debugstr_w(path), options);
    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return async_create(&ASYNC_IAsyncOperation_StorageFile_, ASYNC_OP, HRESULT_FROM_WIN32(GetLastError()),
                            PK_OBJECT, NULL, 0, result);
    CloseHandle(h);
    if (FAILED(storage_file_create(path, &f))) return E_FAIL;
    return obj_async(&ASYNC_IAsyncOperation_StorageFile_, f, result);
}

HRESULT OV_IStorageFolder_CreateFileAsync(struct obj *o, HSTRING name, void **result)
{
    return OV_IStorageFolder_CreateFileAsync_2(o, name, 2, result);
}

HRESULT OV_IStorageFolder_GetFolderAsync(struct obj *o, HSTRING name, void **result)
{
    WCHAR path[MAX_PATH];
    IInspectable *f;
    DWORD attr;
    child_path(o, name, path);
    attr = GetFileAttributesW(path);
    TRACE("%s attr %#lx\n", debugstr_w(path), attr);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return not_found_async(&ASYNC_IAsyncOperation_StorageFolder_, result);
    storage_folder_create(path, &f);
    return obj_async(&ASYNC_IAsyncOperation_StorageFolder_, f, result);
}

HRESULT OV_IStorageFolder_GetFileAsync(struct obj *o, HSTRING name, void **result)
{
    WCHAR path[MAX_PATH];
    IInspectable *f;
    DWORD attr;
    child_path(o, name, path);
    attr = GetFileAttributesW(path);
    TRACE("%s attr %#lx\n", debugstr_w(path), attr);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
        return not_found_async(&ASYNC_IAsyncOperation_StorageFile_, result);
    storage_file_create(path, &f);
    return obj_async(&ASYNC_IAsyncOperation_StorageFile_, f, result);
}

static HRESULT get_item(struct obj *o, HSTRING name, BOOL try, void **result)
{
    WCHAR path[MAX_PATH];
    IInspectable *f = NULL, *item = NULL;
    DWORD attr;
    child_path(o, name, path);
    attr = GetFileAttributesW(path);
    TRACE("%s attr %#lx\n", debugstr_w(path), attr);
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        if (!try) return not_found_async(&ASYNC_IAsyncOperation_IStorageItem_, result);
        return async_create(&ASYNC_IAsyncOperation_IStorageItem_, ASYNC_OP, S_OK, PK_OBJECT, &item, sizeof(item), result);
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) storage_folder_create(path, &f);
    else storage_file_create(path, &f);
    IInspectable_QueryInterface(f, &IID_Windows_Storage_IStorageItem, (void **)&item);
    IInspectable_Release(f);
    return obj_async(&ASYNC_IAsyncOperation_IStorageItem_, item, result);
}

HRESULT OV_IStorageFolder_GetItemAsync(struct obj *o, HSTRING name, void **result)
{
    return get_item(o, name, FALSE, result);
}

HRESULT OV_IStorageFolder2_TryGetItemAsync(struct obj *o, HSTRING name, void **result)
{
    return get_item(o, name, TRUE, result);
}

HRESULT OV_IStorageItem_DeleteAsync_2(struct obj *o, INT32 option, void **result)
{
    const WCHAR *path = path_of(o);
    DWORD attr = GetFileAttributesW(path);
    BOOL ok;
    TRACE("%s\n", debugstr_w(path));
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        WCHAR from[MAX_PATH + 2] = {0};
        SHFILEOPSTRUCTW op = {0};
        wcscpy(from, path);
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI;
        ok = !SHFileOperationW(&op);
    }
    else ok = DeleteFileW(path);
    return async_create(&ASYNC_IAsyncAction, ASYNC_ACTION, ok ? S_OK : HRESULT_FROM_WIN32(GetLastError()),
                        PK_PRIM, NULL, 0, result);
}

HRESULT OV_IStorageItem_DeleteAsync(struct obj *o, void **result)
{
    return OV_IStorageItem_DeleteAsync_2(o, 0, result);
}

HRESULT OV_IStorageItem_IsOfType(struct obj *o, UINT32 type, boolean *result)
{
    /* StorageItemTypes: 1 File, 2 Folder */
    BOOL dir = !wcscmp(o->cls->name, L"Windows.Storage.StorageFolder");
    *result = (type == 2 && dir) || (type == 1 && !dir);
    return S_OK;
}
