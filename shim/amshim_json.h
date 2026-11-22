/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Json (JsonObject, JsonArray, JsonValue). Wine's windows.web.dll only has a skeleton
 * (no Parse, no object accessors), so the whole thing lives here:
 *   amshim_json.c       object model (jnode behind every Json* object)
 *   amshim_json_text.c  parser and serializer
 *   ov_json_value.c     statics (Parse, Create*), IJsonValue, JsonObject (incl. its IMap)
 *   ov_json_array.c     JsonArray and the iterators */
#pragma once
#include <math.h>
#include "gen/amshim_gen.h"

#define JSON_E_INVALID   ((HRESULT)0x83750007)   /* WEB_E_INVALID_JSON_STRING */
#define JSON_E_NOT_FOUND ((HRESULT)0x8375000A)   /* WEB_E_JSON_VALUE_NOT_FOUND */
#define JSON_E_TYPE      ((HRESULT)0x8000000E)   /* E_ILLEGAL_METHOD_CALL: wrong value type */

enum { JT_NULL, JT_BOOLEAN, JT_NUMBER, JT_STRING, JT_ARRAY, JT_OBJECT };   /* = JsonValueType */

struct jnode
{
    int type;
    boolean b;
    double num;
    HSTRING str;
    UINT32 count, cap;
    HSTRING *keys;           /* objects only */
    struct obj **vals;       /* children (one reference each) */
};

struct jiter { struct obj *owner; UINT32 pos; };

static inline struct jnode *node(struct obj *o) { return o->priv; }
static inline void addref(struct obj *o) { IInspectable_AddRef((IInspectable *)&o->ifcs[0]); }
static inline void release(struct obj *o) { IInspectable_Release((IInspectable *)&o->ifcs[0]); }

void jnode_clear(struct jnode *n);
struct obj *jnew(int type);
void jgrow(struct jnode *n);
void jappend(struct jnode *n, struct obj *val);
int jfind(struct jnode *n, const WCHAR *key);
BOOL jset(struct jnode *n, const WCHAR *key, struct obj *val);
struct obj *jfrom_iface(void *value);
HRESULT jret(struct obj *o, const GUID *iid, void **result);
#define RET_VALUE(o, result)  jret(o, &IID_Windows_Data_Json_IJsonValue, result)
#define RET_OBJECT(o, result) jret(o, &IID_Windows_Data_Json_IJsonObject, result)
#define RET_ARRAY(o, result)  jret(o, &IID_Windows_Data_Json_IJsonArray, result)
struct obj *jparse_text(HSTRING input, int want_type);
HRESULT jstringify(struct obj *o, HSTRING *result);
HRESULT jget_string(struct obj *v, HSTRING *result);
HRESULT jget_number(struct obj *v, DOUBLE *result);
HRESULT jget_boolean(struct obj *v, boolean *result);
HRESULT jget_typed(struct obj *v, int type, void **result);
