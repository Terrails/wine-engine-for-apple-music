/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Json: the object model (overview: amshim_json.h). */
#include "amshim_json.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

void jnode_clear(struct jnode *n)
{
    UINT32 i;
    for (i = 0; i < n->count; i++)
    {
        if (n->keys) WindowsDeleteString(n->keys[i]);
        release(n->vals[i]);
    }
    n->count = 0;
}

static void jnode_free(struct obj *o)
{
    struct jnode *n = o->priv;
    jnode_clear(n);
    WindowsDeleteString(n->str);
    free(n->keys);
    free(n->vals);
    free(n);
}

static HRESULT jinit(struct obj *o, int type)
{
    struct jnode *n = calloc(1, sizeof(*n));
    n->type = type;
    o->priv = n;
    o->priv_free = jnode_free;
    return S_OK;
}

HRESULT INIT_JsonObject(struct obj *o) { return jinit(o, JT_OBJECT); }
HRESULT INIT_JsonArray(struct obj *o) { return jinit(o, JT_ARRAY); }
HRESULT INIT_JsonValue(struct obj *o) { return jinit(o, JT_NULL); }

struct obj *jnew(int type)
{
    static const WCHAR *const cls[] = { L"Windows.Data.Json.JsonValue", L"Windows.Data.Json.JsonValue",
        L"Windows.Data.Json.JsonValue", L"Windows.Data.Json.JsonValue", L"Windows.Data.Json.JsonArray",
        L"Windows.Data.Json.JsonObject" };
    IInspectable *inst;
    struct obj *o;
    if (FAILED(create_object_by_name(cls[type], &inst))) return NULL;
    o = impl_from_inspectable(inst);
    node(o)->type = type;
    return o;
}

void jgrow(struct jnode *n)
{
    if (n->count < n->cap) return;
    n->cap = n->cap ? n->cap * 2 : 8;
    n->vals = realloc(n->vals, n->cap * sizeof(*n->vals));
    if (n->type == JT_OBJECT) n->keys = realloc(n->keys, n->cap * sizeof(*n->keys));
}

/* takes ownership of the caller's reference to `val` */
void jappend(struct jnode *n, struct obj *val)
{
    jgrow(n);
    n->vals[n->count++] = val;
}

int jfind(struct jnode *n, const WCHAR *key)
{
    UINT32 i;
    for (i = 0; i < n->count; i++)
        if (!wcscmp(WindowsGetStringRawBuffer(n->keys[i], NULL), key)) return i;
    return -1;
}

/* takes ownership of the caller's reference to `val`; returns TRUE if a value was replaced */
BOOL jset(struct jnode *n, const WCHAR *key, struct obj *val)
{
    int i = jfind(n, key);
    if (i >= 0)
    {
        release(n->vals[i]);
        n->vals[i] = val;
        return TRUE;
    }
    jgrow(n);
    hstring_from_w(key, &n->keys[n->count]);
    n->vals[n->count++] = val;
    return FALSE;
}

/* a foreign IJsonValue is only accepted if it is one of ours */
struct obj *jfrom_iface(void *value)
{
    struct obj *o;
    if (!value) return jnew(JT_NULL);
    o = impl_from_inspectable(value);
    if (wcsncmp(o->cls->name, L"Windows.Data.Json.", 18)) { FIXME("foreign IJsonValue %s\n", debugstr_w(o->cls->name)); return NULL; }
    addref(o);
    return o;
}

HRESULT jret(struct obj *o, const GUID *iid, void **result)
{
    *result = NULL;
    return o ? obj_query(o, iid, result) : E_FAIL;
}
