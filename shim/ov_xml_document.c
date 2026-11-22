/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Xml.Dom: XmlDocument (overview: amshim_xml.h). */
#include "amshim_xml.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- XmlDocument ------------------------------------------------------ */

HRESULT INIT_XmlDocument(struct obj *o)
{
    IXMLDOMDocument2 *doc;
    HRESULT hr = CoCreateInstance(&CLSID_DOMDocument60, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument2, (void **)&doc);
    if (hr == CO_E_NOTINITIALIZED)
    {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        hr = CoCreateInstance(&CLSID_DOMDocument60, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument2, (void **)&doc);
    }
    if (FAILED(hr)) { ERR("msxml6 unavailable: %#lx\n", hr); return S_OK; }
    IXMLDOMDocument2_put_async(doc, VARIANT_FALSE);
    IXMLDOMDocument2_put_validateOnParse(doc, VARIANT_FALSE);
    IXMLDOMDocument2_put_resolveExternals(doc, VARIANT_FALSE);
    {
        /* plists carry a DOCTYPE: allow (but don't fetch) DTDs */
        VARIANT v;
        V_VT(&v) = VT_BOOL; V_BOOL(&v) = VARIANT_TRUE;
        IXMLDOMDocument2_setProperty(doc, (BSTR)L"ProhibitDTD", v);
        V_BOOL(&v) = VARIANT_FALSE;
        IXMLDOMDocument2_setProperty(doc, (BSTR)L"ProhibitDTD", v);
        V_VT(&v) = VT_BSTR; V_BSTR(&v) = SysAllocString(L"XPath");
        IXMLDOMDocument2_setProperty(doc, (BSTR)L"SelectionLanguage", v);
        SysFreeString(V_BSTR(&v));
    }
    o->priv = doc;
    o->priv_free = node_free;
    return S_OK;
}

static IXMLDOMDocument2 *doc_of(struct obj *o)
{
    return o->priv;
}

HRESULT OV_IXmlDocumentIO_LoadXml(struct obj *o, HSTRING xml)
{
    VARIANT_BOOL ok = VARIANT_FALSE;
    BSTR b = bstr_in(xml);
    WCHAR *dt, *end;
    HRESULT hr;

    /* Wine's MSXML reports success but produces a document without a root element when the
     * text has a DOCTYPE with an external subset (every Apple plist has one). The DTD is never
     * used (ResolveExternals is off), so blank the declaration out before parsing. */
    if (b && (dt = wcsstr(b, L"<!DOCTYPE")))
    {
        WCHAR *subset = wcschr(dt, '[');
        end = wcschr(dt, '>');
        if (subset && end && subset < end) end = wcsstr(subset, L"]>") ? wcsstr(subset, L"]>") + 1 : NULL;
        if (end) for (; dt <= end; dt++) *dt = ' ';
    }
    hr = doc_of(o) ? IXMLDOMDocument2_loadXML(doc_of(o), b, &ok) : E_FAIL;
    SysFreeString(b);
    TRACE("%u chars -> %#lx ok %d\n", WindowsGetStringLen(xml), hr, ok);
    if (SUCCEEDED(hr) && !ok)
    {
        IXMLDOMParseError *err;
        BSTR reason = NULL;
        if (SUCCEEDED(IXMLDOMDocument2_get_parseError(doc_of(o), &err)))
        {
            IXMLDOMParseError_get_reason(err, &reason);
            WARN("parse error %s\n", debugstr_w(reason));
            SysFreeString(reason);
            IXMLDOMParseError_Release(err);
        }
        return 0xc00ce584; /* XML parse error */
    }
    return hr;
}

HRESULT OV_IXmlDocumentIO_LoadXml_2(struct obj *o, HSTRING xml, void *settings) { return OV_IXmlDocumentIO_LoadXml(o, xml); }

HRESULT OV_IXmlDocumentIO2_LoadXmlFromBuffer(struct obj *o, void *buffer)
{
    BYTE *d;
    UINT32 len;
    int n, skip = 0;
    WCHAR *w;
    HSTRING s;
    HRESULT hr;
    buffer_bytes(buffer, &d, &len, NULL);
    if (len >= 3 && d[0] == 0xef && d[1] == 0xbb && d[2] == 0xbf) skip = 3;
    n = MultiByteToWideChar(CP_UTF8, 0, (char *)d + skip, len - skip, NULL, 0);
    w = malloc((n + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, (char *)d + skip, len - skip, w, n);
    WindowsCreateString(w, n, &s);
    free(w);
    hr = OV_IXmlDocumentIO_LoadXml(o, s);
    WindowsDeleteString(s);
    return hr;
}

HRESULT OV_IXmlDocumentIO2_LoadXmlFromBuffer_2(struct obj *o, void *buffer, void *settings) { return OV_IXmlDocumentIO2_LoadXmlFromBuffer(o, buffer); }

HRESULT OV_IXmlDocument_get_DocumentElement(struct obj *o, void **result)
{
    IXMLDOMElement *el = NULL;
    IXMLDOMDocument2_get_documentElement(doc_of(o), &el);
    if (!el) { WARN("document has no root element\n"); *result = NULL; return S_OK; }
    return wrap((IXMLDOMNode *)el, &IID_Windows_Data_Xml_Dom_IXmlElement, result);
}

HRESULT OV_IXmlDocument_CreateElement(struct obj *o, HSTRING name, void **result)
{
    IXMLDOMElement *el = NULL;
    BSTR b = bstr_in(name);
    HRESULT hr = IXMLDOMDocument2_createElement(doc_of(o), b, &el);
    SysFreeString(b);
    if (FAILED(hr)) return hr;
    return wrap((IXMLDOMNode *)el, &IID_Windows_Data_Xml_Dom_IXmlElement, result);
}

HRESULT OV_IXmlDocument_CreateTextNode(struct obj *o, HSTRING data, void **result)
{
    IXMLDOMText *t = NULL;
    BSTR b = bstr_in(data);
    HRESULT hr = IXMLDOMDocument2_createTextNode(doc_of(o), b, &t);
    SysFreeString(b);
    if (FAILED(hr)) return hr;
    return wrap((IXMLDOMNode *)t, &IID_Windows_Data_Xml_Dom_IXmlText, result);
}

HRESULT OV_IXmlDocument_CreateAttribute(struct obj *o, HSTRING name, void **result)
{
    IXMLDOMAttribute *a = NULL;
    BSTR b = bstr_in(name);
    HRESULT hr = IXMLDOMDocument2_createAttribute(doc_of(o), b, &a);
    SysFreeString(b);
    if (FAILED(hr)) return hr;
    return wrap((IXMLDOMNode *)a, &IID_Windows_Data_Xml_Dom_IXmlAttribute, result);
}

HRESULT list_wrap(IXMLDOMNodeList *list, void **result);

HRESULT OV_IXmlDocument_GetElementsByTagName(struct obj *o, HSTRING name, void **result)
{
    IXMLDOMNodeList *list = NULL;
    BSTR b = bstr_in(name);
    HRESULT hr = IXMLDOMDocument2_getElementsByTagName(doc_of(o), b, &list);
    SysFreeString(b);
    if (FAILED(hr)) return hr;
    return list_wrap(list, result);
}
