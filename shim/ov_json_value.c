/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Json: statics, IJsonValue, JsonObject (overview: amshim_json.h). */
#include "amshim_json.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- statics ----------------------------------------------------------- */

static HRESULT jparse_ret(HSTRING input, int type, const GUID *iid, void **result, boolean *ok)
{
    struct obj *o = jparse_text(input, type);
    HRESULT hr;
    *result = NULL;
    if (ok) *ok = o != NULL;
    if (!o) return ok ? S_OK : JSON_E_INVALID;
    hr = jret(o, iid, result);
    release(o);
    return hr;
}

HRESULT OV_IJsonObjectStatics_Parse(struct obj *o, HSTRING input, void **result)
{ return jparse_ret(input, JT_OBJECT, &IID_Windows_Data_Json_IJsonObject, result, NULL); }
HRESULT OV_IJsonObjectStatics_TryParse(struct obj *o, HSTRING input, void **result_, boolean *result)
{ return jparse_ret(input, JT_OBJECT, &IID_Windows_Data_Json_IJsonObject, result_, result); }
HRESULT OV_IJsonArrayStatics_Parse(struct obj *o, HSTRING input, void **result)
{ return jparse_ret(input, JT_ARRAY, &IID_Windows_Data_Json_IJsonArray, result, NULL); }
HRESULT OV_IJsonArrayStatics_TryParse(struct obj *o, HSTRING input, void **result_, boolean *result)
{ return jparse_ret(input, JT_ARRAY, &IID_Windows_Data_Json_IJsonArray, result_, result); }
HRESULT OV_IJsonValueStatics_Parse(struct obj *o, HSTRING input, void **result)
{ return jparse_ret(input, -1, &IID_Windows_Data_Json_IJsonValue, result, NULL); }
HRESULT OV_IJsonValueStatics_TryParse(struct obj *o, HSTRING input, void **result_, boolean *result)
{ return jparse_ret(input, -1, &IID_Windows_Data_Json_IJsonValue, result_, result); }

static HRESULT jmake(int type, boolean b, double num, HSTRING str, void **result)
{
    struct obj *o = jnew(type);
    HRESULT hr;
    if (!o) return E_OUTOFMEMORY;
    node(o)->b = b; node(o)->num = num;
    if (type == JT_STRING) WindowsDuplicateString(str, &node(o)->str);
    hr = RET_VALUE(o, result);
    release(o);
    return hr;
}
HRESULT OV_IJsonValueStatics_CreateBooleanValue(struct obj *o, boolean input, void **result) { return jmake(JT_BOOLEAN, input, 0, NULL, result); }
HRESULT OV_IJsonValueStatics_CreateNumberValue(struct obj *o, DOUBLE input, void **result) { return jmake(JT_NUMBER, 0, input, NULL, result); }
HRESULT OV_IJsonValueStatics_CreateStringValue(struct obj *o, HSTRING input, void **result) { return jmake(JT_STRING, 0, 0, input, result); }
HRESULT OV_IJsonValueStatics2_CreateNullValue(struct obj *o, void **result) { return jmake(JT_NULL, 0, 0, NULL, result); }

/* ---- IJsonValue (all three classes) ------------------------------------ */

HRESULT OV_IJsonValue_get_ValueType(struct obj *o, INT32 *result) { *result = node(o)->type; return S_OK; }
HRESULT OV_IJsonValue_Stringify(struct obj *o, HSTRING *result) { return jstringify(o, result); }
/* IStringable::ToString is shared by several classes: see OV_IStringable_ToString in ov_uri.c */
HRESULT json_to_string(struct obj *o, HSTRING *result) { return jstringify(o, result); }

HRESULT jget_string(struct obj *v, HSTRING *result)
{
    *result = NULL;
    if (node(v)->type != JT_STRING) return JSON_E_TYPE;
    return WindowsDuplicateString(node(v)->str, result);
}
HRESULT jget_number(struct obj *v, DOUBLE *result)
{
    *result = 0;
    if (node(v)->type != JT_NUMBER) return JSON_E_TYPE;
    *result = node(v)->num;
    return S_OK;
}
HRESULT jget_boolean(struct obj *v, boolean *result)
{
    *result = FALSE;
    if (node(v)->type != JT_BOOLEAN) return JSON_E_TYPE;
    *result = node(v)->b;
    return S_OK;
}
HRESULT jget_typed(struct obj *v, int type, void **result)
{
    *result = NULL;
    if (node(v)->type != type) return JSON_E_TYPE;
    return type == JT_ARRAY ? RET_ARRAY(v, result) : RET_OBJECT(v, result);
}

HRESULT OV_IJsonValue_GetString(struct obj *o, HSTRING *result) { return jget_string(o, result); }
HRESULT OV_IJsonValue_GetNumber(struct obj *o, DOUBLE *result) { return jget_number(o, result); }
HRESULT OV_IJsonValue_GetBoolean(struct obj *o, boolean *result) { return jget_boolean(o, result); }
HRESULT OV_IJsonValue_GetArray(struct obj *o, void **result) { return jget_typed(o, JT_ARRAY, result); }
HRESULT OV_IJsonValue_GetObject(struct obj *o, void **result) { return jget_typed(o, JT_OBJECT, result); }

/* ---- JsonObject -------------------------------------------------------- */

static struct obj *jnamed(struct obj *o, HSTRING name)
{
    int i = jfind(node(o), WindowsGetStringRawBuffer(name, NULL));
    return i < 0 ? NULL : node(o)->vals[i];
}

HRESULT OV_IJsonObject_GetNamedValue(struct obj *o, HSTRING name, void **result)
{
    struct obj *v = jnamed(o, name);
    *result = NULL;
    return v ? RET_VALUE(v, result) : JSON_E_NOT_FOUND;
}
HRESULT OV_IJsonObject_SetNamedValue(struct obj *o, HSTRING name, void *value)
{
    struct obj *v = jfrom_iface(value);
    if (!v) return E_INVALIDARG;
    jset(node(o), WindowsGetStringRawBuffer(name, NULL), v);
    return S_OK;
}
HRESULT OV_IJsonObject_GetNamedObject(struct obj *o, HSTRING name, void **result)
{
    struct obj *v = jnamed(o, name);
    *result = NULL;
    return v ? jget_typed(v, JT_OBJECT, result) : JSON_E_NOT_FOUND;
}
HRESULT OV_IJsonObject_GetNamedArray(struct obj *o, HSTRING name, void **result)
{
    struct obj *v = jnamed(o, name);
    *result = NULL;
    return v ? jget_typed(v, JT_ARRAY, result) : JSON_E_NOT_FOUND;
}
HRESULT OV_IJsonObject_GetNamedString(struct obj *o, HSTRING name, HSTRING *result)
{
    struct obj *v = jnamed(o, name);
    *result = NULL;
    return v ? jget_string(v, result) : JSON_E_NOT_FOUND;
}
HRESULT OV_IJsonObject_GetNamedNumber(struct obj *o, HSTRING name, DOUBLE *result)
{
    struct obj *v = jnamed(o, name);
    *result = 0;
    return v ? jget_number(v, result) : JSON_E_NOT_FOUND;
}
HRESULT OV_IJsonObject_GetNamedBoolean(struct obj *o, HSTRING name, boolean *result)
{
    struct obj *v = jnamed(o, name);
    *result = FALSE;
    return v ? jget_boolean(v, result) : JSON_E_NOT_FOUND;
}

/* the *WithDefaultValues variants return the default when the name is missing */
HRESULT OV_IJsonObjectWithDefaultValues_GetNamedValue(struct obj *o, HSTRING name, void *def, void **result)
{
    struct obj *v = jnamed(o, name);
    if (v) return RET_VALUE(v, result);
    *result = def;
    if (def) IUnknown_AddRef((IUnknown *)def);
    return S_OK;
}
HRESULT OV_IJsonObjectWithDefaultValues_GetNamedObject(struct obj *o, HSTRING name, void *def, void **result)
{
    struct obj *v = jnamed(o, name);
    if (v) return jget_typed(v, JT_OBJECT, result);
    *result = def;
    if (def) IUnknown_AddRef((IUnknown *)def);
    return S_OK;
}
HRESULT OV_IJsonObjectWithDefaultValues_GetNamedArray(struct obj *o, HSTRING name, void *def, void **result)
{
    struct obj *v = jnamed(o, name);
    if (v) return jget_typed(v, JT_ARRAY, result);
    *result = def;
    if (def) IUnknown_AddRef((IUnknown *)def);
    return S_OK;
}
HRESULT OV_IJsonObjectWithDefaultValues_GetNamedString(struct obj *o, HSTRING name, HSTRING def, HSTRING *result)
{
    struct obj *v = jnamed(o, name);
    return v ? jget_string(v, result) : WindowsDuplicateString(def, result);
}
HRESULT OV_IJsonObjectWithDefaultValues_GetNamedNumber(struct obj *o, HSTRING name, DOUBLE def, DOUBLE *result)
{
    struct obj *v = jnamed(o, name);
    if (v) return jget_number(v, result);
    *result = def;
    return S_OK;
}
HRESULT OV_IJsonObjectWithDefaultValues_GetNamedBoolean(struct obj *o, HSTRING name, boolean def, boolean *result)
{
    struct obj *v = jnamed(o, name);
    if (v) return jget_boolean(v, result);
    *result = def;
    return S_OK;
}

/* IMap<String, IJsonValue> */
HRESULT OV_IMap_String_IJsonValue__Lookup(struct obj *o, HSTRING key, void **result)
{
    struct obj *v = jnamed(o, key);
    *result = NULL;
    return v ? RET_VALUE(v, result) : E_BOUNDS;
}
HRESULT OV_IMap_String_IJsonValue__get_Size(struct obj *o, UINT32 *result) { *result = node(o)->count; return S_OK; }
HRESULT OV_IMap_String_IJsonValue__HasKey(struct obj *o, HSTRING key, boolean *result) { *result = jnamed(o, key) != NULL; return S_OK; }
HRESULT OV_IMap_String_IJsonValue__Insert(struct obj *o, HSTRING key, void *value, boolean *result)
{
    struct obj *v = jfrom_iface(value);
    if (!v) return E_INVALIDARG;
    *result = jset(node(o), WindowsGetStringRawBuffer(key, NULL), v);
    return S_OK;
}
HRESULT OV_IMap_String_IJsonValue__Remove(struct obj *o, HSTRING key)
{
    struct jnode *n = node(o);
    int i = jfind(n, WindowsGetStringRawBuffer(key, NULL));
    if (i < 0) return E_BOUNDS;
    WindowsDeleteString(n->keys[i]);
    release(n->vals[i]);
    memmove(&n->keys[i], &n->keys[i + 1], (n->count - i - 1) * sizeof(*n->keys));
    memmove(&n->vals[i], &n->vals[i + 1], (n->count - i - 1) * sizeof(*n->vals));
    n->count--;
    return S_OK;
}
HRESULT OV_IMap_String_IJsonValue__Clear(struct obj *o) { jnode_clear(node(o)); return S_OK; }

/* ---- iterators --------------------------------------------------------- */
