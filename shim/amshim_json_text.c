/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Json: parser and serializer (overview: amshim_json.h). */
#include "amshim_json.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- parser ---------------------------------------------------------- */

struct jparse { const WCHAR *p, *end; int depth; };

static void skip_ws(struct jparse *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

static int hexval(WCHAR c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL parse_string(struct jparse *s, HSTRING *out)
{
    WCHAR *buf;
    UINT32 n = 0;
    if (s->p >= s->end || *s->p != '"') return FALSE;
    s->p++;
    buf = malloc((s->end - s->p + 1) * sizeof(WCHAR));
    while (s->p < s->end && *s->p != '"')
    {
        WCHAR c = *s->p++;
        if (c == '\\')
        {
            if (s->p >= s->end) goto fail;
            c = *s->p++;
            switch (c)
            {
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
            {
                int i, v = 0;
                if (s->end - s->p < 4) goto fail;
                for (i = 0; i < 4; i++)
                {
                    int h = hexval(s->p[i]);
                    if (h < 0) goto fail;
                    v = v * 16 + h;
                }
                s->p += 4;
                c = v;
                break;
            }
            case '"': case '\\': case '/': break;
            default: goto fail;
            }
        }
        buf[n++] = c;
    }
    if (s->p >= s->end) goto fail;
    s->p++;
    WindowsCreateString(buf, n, out);
    free(buf);
    return TRUE;
fail:
    free(buf);
    return FALSE;
}

static struct obj *parse_value(struct jparse *s)
{
    struct obj *o = NULL, *child;
    skip_ws(s);
    if (s->p >= s->end || ++s->depth > 200) return NULL;
    if (*s->p == '{')
    {
        o = jnew(JT_OBJECT);
        s->p++;
        skip_ws(s);
        if (s->p < s->end && *s->p == '}') { s->p++; goto done; }
        for (;;)
        {
            HSTRING key;
            skip_ws(s);
            if (!parse_string(s, &key)) goto fail;
            skip_ws(s);
            if (s->p >= s->end || *s->p++ != ':') { WindowsDeleteString(key); goto fail; }
            child = parse_value(s);
            if (child) jset(node(o), WindowsGetStringRawBuffer(key, NULL), child);
            WindowsDeleteString(key);
            if (!child) goto fail;
            skip_ws(s);
            if (s->p < s->end && *s->p == ',') { s->p++; continue; }
            if (s->p < s->end && *s->p == '}') { s->p++; break; }
            goto fail;
        }
    }
    else if (*s->p == '[')
    {
        o = jnew(JT_ARRAY);
        s->p++;
        skip_ws(s);
        if (s->p < s->end && *s->p == ']') { s->p++; goto done; }
        for (;;)
        {
            if (!(child = parse_value(s))) goto fail;
            jappend(node(o), child);
            skip_ws(s);
            if (s->p < s->end && *s->p == ',') { s->p++; continue; }
            if (s->p < s->end && *s->p == ']') { s->p++; break; }
            goto fail;
        }
    }
    else if (*s->p == '"')
    {
        o = jnew(JT_STRING);
        if (!parse_string(s, &node(o)->str)) goto fail;
    }
    else if (s->end - s->p >= 4 && !wcsncmp(s->p, L"true", 4)) { o = jnew(JT_BOOLEAN); node(o)->b = TRUE; s->p += 4; }
    else if (s->end - s->p >= 5 && !wcsncmp(s->p, L"false", 5)) { o = jnew(JT_BOOLEAN); s->p += 5; }
    else if (s->end - s->p >= 4 && !wcsncmp(s->p, L"null", 4)) { o = jnew(JT_NULL); s->p += 4; }
    else if (*s->p == '-' || (*s->p >= '0' && *s->p <= '9'))
    {
        WCHAR num[64], *stop;
        UINT32 n = 0;
        while (s->p + n < s->end && n < ARRAY_SIZE(num) - 1 && wcschr(L"+-0123456789.eE", s->p[n])) { num[n] = s->p[n]; n++; }
        num[n] = 0;
        o = jnew(JT_NUMBER);
        node(o)->num = wcstod(num, &stop);
        if (stop == num) goto fail;
        s->p += stop - num;
    }
    else return NULL;
done:
    s->depth--;
    return o;
fail:
    if (o) release(o);
    return NULL;
}

struct obj *jparse_text(HSTRING input, int want_type)
{
    UINT32 len;
    const WCHAR *w = WindowsGetStringRawBuffer(input, &len);
    struct jparse s = { w, w + len, 0 };
    struct obj *o = parse_value(&s);
    if (o) skip_ws(&s);
    if (o && (s.p != s.end || (want_type >= 0 && node(o)->type != want_type))) { release(o); o = NULL; }
    if (!o) TRACE("invalid JSON (%u chars): %s\n", len, debugstr_wn(w, min(len, 120)));
    return o;
}

/* ---- serializer -------------------------------------------------------- */

struct jbuf { WCHAR *p; size_t len, cap; };

static void jb_add(struct jbuf *b, const WCHAR *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        b->cap = max(b->cap * 2, b->len + n + 256);
        b->p = realloc(b->p, b->cap * sizeof(WCHAR));
    }
    memcpy(b->p + b->len, s, n * sizeof(WCHAR));
    b->len += n;
}

static void jb_str(struct jbuf *b, HSTRING h)
{
    UINT32 len, i;
    const WCHAR *w = WindowsGetStringRawBuffer(h, &len);
    jb_add(b, L"\"", 1);
    for (i = 0; i < len; i++)
    {
        WCHAR c = w[i], esc[8];
        switch (c)
        {
        case '"': jb_add(b, L"\\\"", 2); break;
        case '\\': jb_add(b, L"\\\\", 2); break;
        case '\b': jb_add(b, L"\\b", 2); break;
        case '\f': jb_add(b, L"\\f", 2); break;
        case '\n': jb_add(b, L"\\n", 2); break;
        case '\r': jb_add(b, L"\\r", 2); break;
        case '\t': jb_add(b, L"\\t", 2); break;
        default:
            if (c < 0x20) { swprintf(esc, ARRAY_SIZE(esc), L"\\u%04x", c); jb_add(b, esc, 6); }
            else jb_add(b, &c, 1);
        }
    }
    jb_add(b, L"\"", 1);
}

static void jb_value(struct jbuf *b, struct obj *o)
{
    struct jnode *n = node(o);
    WCHAR num[64];
    UINT32 i;
    switch (n->type)
    {
    case JT_NULL: jb_add(b, L"null", 4); break;
    case JT_BOOLEAN: if (n->b) jb_add(b, L"true", 4); else jb_add(b, L"false", 5); break;
    case JT_NUMBER:
        if (n->num == floor(n->num) && fabs(n->num) < 1e15) swprintf(num, ARRAY_SIZE(num), L"%.0f", n->num);
        else swprintf(num, ARRAY_SIZE(num), L"%.17g", n->num);
        jb_add(b, num, wcslen(num));
        break;
    case JT_STRING: jb_str(b, n->str); break;
    case JT_ARRAY:
        jb_add(b, L"[", 1);
        for (i = 0; i < n->count; i++) { if (i) jb_add(b, L",", 1); jb_value(b, n->vals[i]); }
        jb_add(b, L"]", 1);
        break;
    case JT_OBJECT:
        jb_add(b, L"{", 1);
        for (i = 0; i < n->count; i++)
        {
            if (i) jb_add(b, L",", 1);
            jb_str(b, n->keys[i]);
            jb_add(b, L":", 1);
            jb_value(b, n->vals[i]);
        }
        jb_add(b, L"}", 1);
        break;
    }
}

HRESULT jstringify(struct obj *o, HSTRING *result)
{
    struct jbuf b = {0};
    HRESULT hr;
    jb_value(&b, o);
    hr = WindowsCreateString(b.p, b.len, result);
    free(b.p);
    return hr;
}
