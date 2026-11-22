/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#define COBJMACROS
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <windef.h>
#include <winbase.h>
#include <winstring.h>
#include <objbase.h>
#include <inspectable.h>
#include <roapi.h>
#include <activation.h>
#include "wine/debug.h"

struct obj;

struct ifc
{
    const void *vtbl;
    struct obj *obj;
};

struct iface_entry
{
    const GUID *iid;
    const void *vtbl;
    const char *name;
};

struct class_info
{
    const WCHAR *name;
    const struct iface_entry *ifaces;
    const struct iface_entry *factory_ifaces;
    int default_ctor;
    /* optional hook: return TRUE to handle the call (sets *hr) */
    BOOL (*dispatch)(struct obj *o, const GUID *iid, unsigned int idx, HRESULT *hr, void **args);
    HRESULT (*init)(struct obj *o);
};

enum prop_kind { PK_PRIM, PK_STRING, PK_OBJECT, PK_STRUCT };

struct prop
{
    struct prop *next;
    const WCHAR *name;
    enum prop_kind kind;
    unsigned int size;
    union { BYTE raw[64]; HSTRING str; IUnknown *unk; } v;
};

struct obj
{
    const struct class_info *cls;
    BOOL is_factory;
    LONG ref;
    unsigned int count;
    struct ifc *ifcs;
    const struct iface_entry *entries;
    struct prop *props;
    CRITICAL_SECTION cs;
    void *priv;
    void (*priv_free)(struct obj *o);
    struct ifc weak_src;        /* IWeakReferenceSource tear-off */
    const GUID *extra_iid;      /* optional extra (non-WinRT) COM interface */
    struct ifc extra;
};

extern struct class_info *all_classes[];

HRESULT WINAPI ifc_QueryInterface(struct ifc *This, REFIID iid, void **out);
ULONG WINAPI ifc_AddRef(struct ifc *This);
ULONG WINAPI ifc_Release(struct ifc *This);
HRESULT WINAPI ifc_GetIids(struct ifc *This, ULONG *count, IID **iids);
HRESULT WINAPI ifc_GetRuntimeClassName(struct ifc *This, HSTRING *name);
HRESULT WINAPI ifc_GetTrustLevel(struct ifc *This, TrustLevel *level);

struct obj *impl_from_inspectable(IInspectable *p);
HRESULT create_object(const struct class_info *cls, IInspectable **out);
HRESULT create_object_by_name(const WCHAR *name, IInspectable **out);
const struct class_info *find_class(const WCHAR *name);
HRESULT obj_query(struct obj *o, REFIID iid, void **out);

HRESULT prop_put(struct obj *o, const WCHAR *name, enum prop_kind kind, const void *value, unsigned int size);
HRESULT prop_get(struct obj *o, const WCHAR *name, enum prop_kind kind, void *result, unsigned int size,
                 const WCHAR *cls);
struct prop *prop_find(struct obj *o, const WCHAR *name);

HRESULT hstring_from_w(const WCHAR *s, HSTRING *out);


/* ov_misc.c: called once for each new activation factory (to attach extra interfaces) */
void factory_created(struct obj *factory);

/* amshim_async.c */
enum async_kind { ASYNC_ACTION, ASYNC_OP, ASYNC_ACTION_PROGRESS, ASYNC_OP_PROGRESS };
HRESULT async_create(const struct iface_entry *op, enum async_kind kind, HRESULT status,
                     enum prop_kind rk, const void *value, unsigned int size, void **out);
/* pending operation: complete later (from any thread) with async_complete() */
HRESULT async_create_pending(const struct iface_entry *op, enum async_kind kind, struct obj **o, void **out);
void async_complete(struct obj *o, HRESULT status, enum prop_kind rk, const void *value, unsigned int size);
/* block until a foreign IAsyncInfo-implementing operation leaves the Started state */
HRESULT async_wait(IUnknown *op);

/* ov_storage.c */
HRESULT storage_folder_create(const WCHAR *path, IInspectable **out);

/* ov_json.c */
HRESULT json_to_string(struct obj *o, HSTRING *result);

/* ov_vector.c */
HRESULT string_vector_create(const WCHAR **strs, UINT32 count, IInspectable **out);
HRESULT user_languages_vector(IInspectable **out);

/* ov_collections.c: string->object map helpers (usable on any struct obj) */
IInspectable *omap_get(struct obj *o, HSTRING key);
void omap_set(struct obj *o, HSTRING key, IInspectable *value);
void omap_remove(struct obj *o, HSTRING key);
void omap_copy(struct obj *dst, struct obj *src);
/* back a settings map (ApplicationDataContainerSettings) with a registry key (amshim_settings.c);
 * the map owns the key from then on */
void omap_bind_registry(struct obj *o, HKEY key);
HKEY omap_registry(struct obj *o);

/* amshim_settings.c: persistent ApplicationData settings in the registry */
HKEY settings_open(HKEY parent, const WCHAR *sub, BOOL create);   /* parent NULL: the settings root */
HRESULT settings_write(HKEY key, HSTRING name, IInspectable *value); /* E_NOTIMPL: type not stored */
HRESULT settings_read(HKEY key, HSTRING name, IInspectable **out);   /* S_FALSE: no such value */
BOOL settings_has(HKEY key, HSTRING name);
UINT32 settings_count(HKEY key);
void settings_delete(HKEY key, HSTRING name);
void settings_clear(HKEY key);

/* ov_smap.c: case-insensitive string->string map stored in o->priv */
const WCHAR *smap_get(struct obj *o, const WCHAR *key);
void smap_set(struct obj *o, const WCHAR *key, const WCHAR *value);
void smap_append(struct obj *o, const WCHAR *key, const WCHAR *value);
void smap_remove(struct obj *o, const WCHAR *key);
UINT32 smap_count(struct obj *o);
void smap_at(struct obj *o, UINT32 i, const WCHAR **key, const WCHAR **value);
void smap_copy(struct obj *dst, struct obj *src);

/* ov_buffer.c */
HRESULT byte_buffer_create(const void *data, UINT32 len, UINT32 cap, IInspectable **out);
HRESULT buffer_bytes(IUnknown *buf, BYTE **data, UINT32 *len, UINT32 *cap);
HRESULT buffer_set_length(IUnknown *buf, UINT32 len);
HRESULT buffer_input_stream_create(IInspectable *buf, IInspectable **out);
