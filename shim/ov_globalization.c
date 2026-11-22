/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Hand-written overrides: Windows.Globalization */
#include "gen/amshim_gen.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

HRESULT OV_INumeralSystemTranslator_TranslateNumerals(struct obj *o, HSTRING value, HSTRING *result)
{
    /* Only Latin digits are supported: return the input unchanged. */
    return WindowsDuplicateString(value, result);
}

HRESULT OV_INumeralSystemTranslator_get_ResolvedLanguage(struct obj *o, HSTRING *result)
{
    return hstring_from_w(L"en-US", result);
}

/* Surface the app's own log messages in the Wine log (+amshim) */
HRESULT OV_ILoggingChannel_LogMessage(struct obj *o, HSTRING msg)
{
    MESSAGE("amlog: %s\n", debugstr_hstring(msg));
    return S_OK;
}

HRESULT OV_ILoggingChannel_LogMessage_2(struct obj *o, HSTRING msg, INT32 level)
{
    MESSAGE("amlog[%d]: %s\n", level, debugstr_hstring(msg));
    return S_OK;
}

/* ---- DateTimeFormatter: Wine has none. Formats with the user locale via kernel32;
 * the template only selects date vs time and long vs short. ---- */
static HRESULT dtf_make(const WCHAR *tmpl, void **result)
{
    HSTRING h;
    HRESULT hr = create_object_by_name(L"Windows.Globalization.DateTimeFormatting.DateTimeFormatter",
                                       (IInspectable **)result);
    if (FAILED(hr)) return hr;
    hstring_from_w(tmpl, &h);
    prop_put(impl_from_inspectable(*result), L"FormatTemplate", PK_STRING, &h, sizeof(h));
    WindowsDeleteString(h);
    return S_OK;
}

HRESULT OV_IDateTimeFormatterStatics_get_LongDate(struct obj *o, void **result) { return dtf_make(L"longdate", result); }
HRESULT OV_IDateTimeFormatterStatics_get_LongTime(struct obj *o, void **result) { return dtf_make(L"longtime", result); }
HRESULT OV_IDateTimeFormatterStatics_get_ShortDate(struct obj *o, void **result) { return dtf_make(L"shortdate", result); }
HRESULT OV_IDateTimeFormatterStatics_get_ShortTime(struct obj *o, void **result) { return dtf_make(L"shorttime", result); }

HRESULT OV_IDateTimeFormatter_get_Template(struct obj *o, HSTRING *result)
{
    return prop_get(o, L"FormatTemplate", PK_STRING, result, sizeof(*result), NULL);
}

HRESULT OV_IDateTimeFormatter_Format(struct obj *o, S_Windows_Foundation_DateTime value, HSTRING *result)
{
    struct prop *p = prop_find(o, L"FormatTemplate");
    const WCHAR *t = (p && p->kind == PK_STRING) ? WindowsGetStringRawBuffer(p->v.str, NULL) : NULL;
    BOOL has_tmpl = t && *t;
    BOOL time = has_tmpl ? (wcsstr(t, L"time") || wcsstr(t, L"hour") || wcsstr(t, L"minute"))
                         : prop_find(o, L"HourFormat") != NULL;
    BOOL date = has_tmpl ? (wcsstr(t, L"date") || wcsstr(t, L"year") || wcsstr(t, L"month") || wcsstr(t, L"day"))
                         : (prop_find(o, L"YearFormat") != NULL || !time);
    BOOL longf = has_tmpl && (wcsstr(t, L"long") || wcsstr(t, L"full"));
    WCHAR buf[256] = L"", part[128];
    FILETIME ft, lft;
    SYSTEMTIME st;

    /* Windows.Foundation.DateTime.UniversalTime has the FILETIME epoch and unit */
    ft.dwLowDateTime = (DWORD)value.UniversalTime;
    ft.dwHighDateTime = (DWORD)((UINT64)value.UniversalTime >> 32);
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    if (date && GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, longf ? DATE_LONGDATE : DATE_SHORTDATE, &st, NULL,
                                part, ARRAY_SIZE(part), NULL))
        wcscat(buf, part);
    if (time && GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, (has_tmpl && wcsstr(t, L"longtime")) ? 0 : TIME_NOSECONDS,
                                &st, NULL, part, ARRAY_SIZE(part)))
    {
        if (buf[0]) wcscat(buf, L" ");
        wcscat(buf, part);
    }
    return hstring_from_w(buf, result);
}

HRESULT OV_IDateTimeFormatter2_Format(struct obj *o, S_Windows_Foundation_DateTime datetime, HSTRING tz, HSTRING *result)
{
    return OV_IDateTimeFormatter_Format(o, datetime, result);
}

HRESULT OV_IDateTimeFormatter_get_ResolvedLanguage(struct obj *o, HSTRING *result)
{
    WCHAR loc[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(loc, ARRAY_SIZE(loc));
    return hstring_from_w(loc, result);
}

/* ---- DecimalFormatter: Wine has none. Formats through kernel32's GetNumberFormatEx with the
 * user locale's separators; honours FractionDigits, IntegerDigits and IsGrouped. Parsing is
 * left to the generated stubs. ---- */
HRESULT INIT_DecimalFormatter(struct obj *o)
{
    INT32 frac = 2, integer = 1;
    boolean grouped = FALSE;
    prop_put(o, L"FractionDigits", PK_PRIM, &frac, sizeof(frac));
    prop_put(o, L"IntegerDigits", PK_PRIM, &integer, sizeof(integer));
    prop_put(o, L"IsGrouped", PK_PRIM, &grouped, sizeof(grouped));
    return S_OK;
}

static INT32 prim_i32(struct obj *o, const WCHAR *name, INT32 def)
{
    struct prop *p = prop_find(o, name);
    INT32 v = def;
    if (p && p->kind == PK_PRIM) memcpy(&v, p->v.raw, min(p->size, sizeof(v)));
    return v;
}

static HRESULT decimal_format(struct obj *o, const WCHAR *plain, HSTRING *result)
{
    WCHAR dec[8] = L".", thou[8] = L",", grouping[16] = L"3;0", out[128], padded[160];
    NUMBERFMTW fmt = {0};
    INT32 integer = prim_i32(o, L"IntegerDigits", 1);
    struct prop *g = prop_find(o, L"IsGrouped");
    const WCHAR *digits = plain + (*plain == '-');
    const WCHAR *dot = wcschr(digits, '.');
    int have = dot ? (int)(dot - digits) : (int)wcslen(digits), n = 0;

    /* left-pad the integer part with zeros up to IntegerDigits */
    if (*plain == '-') padded[n++] = '-';
    for (; have < integer && n < 60; have++) padded[n++] = '0';
    wcscpy(padded + n, digits);

    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, ARRAY_SIZE(dec));
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, thou, ARRAY_SIZE(thou));
    fmt.NumDigits = max(prim_i32(o, L"FractionDigits", 2), 0);
    fmt.LeadingZero = 1;
    fmt.Grouping = (g && g->v.raw[0]) ? 3 : 0;
    fmt.lpDecimalSep = dec;
    fmt.lpThousandSep = thou;
    fmt.NegativeOrder = 1;
    if (!GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, padded, &fmt, out, ARRAY_SIZE(out))) wcscpy(out, padded);
    return hstring_from_w(out, result);
}

HRESULT OV_INumberFormatter2_FormatInt(struct obj *o, INT64 value, HSTRING *result)
{
    WCHAR buf[40];
    swprintf(buf, ARRAY_SIZE(buf), L"%I64d", value);
    return decimal_format(o, buf, result);
}
HRESULT OV_INumberFormatter2_FormatUInt(struct obj *o, UINT64 value, HSTRING *result)
{
    WCHAR buf[40];
    swprintf(buf, ARRAY_SIZE(buf), L"%I64u", value);
    return decimal_format(o, buf, result);
}
HRESULT OV_INumberFormatter2_FormatDouble(struct obj *o, DOUBLE value, HSTRING *result)
{
    WCHAR buf[80];
    swprintf(buf, ARRAY_SIZE(buf), L"%.*f", min(max(prim_i32(o, L"FractionDigits", 2), 0), 15), value);
    return decimal_format(o, buf, result);
}
HRESULT OV_INumberFormatter_Format(struct obj *o, INT64 value, HSTRING *result) { return OV_INumberFormatter2_FormatInt(o, value, result); }
HRESULT OV_INumberFormatter_Format_2(struct obj *o, UINT64 value, HSTRING *result) { return OV_INumberFormatter2_FormatUInt(o, value, result); }
HRESULT OV_INumberFormatter_Format_3(struct obj *o, DOUBLE value, HSTRING *result) { return OV_INumberFormatter2_FormatDouble(o, value, result); }
