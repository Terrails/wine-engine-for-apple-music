/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Xml.Dom: elements, character data, attributes (overview: amshim_xml.h). */
#include "amshim_xml.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- IXmlElement ------------------------------------------------------ */

IXMLDOMElement *el(struct obj *o) { return (IXMLDOMElement *)o->priv; }

HRESULT OV_IXmlElement_get_TagName(struct obj *o, HSTRING *result)
{
    BSTR b = NULL;
    return bstr_out(IXMLDOMElement_get_tagName(el(o), &b), &b, result);
}

HRESULT OV_IXmlElement_GetAttribute(struct obj *o, HSTRING name, HSTRING *result)
{
    VARIANT v;
    BSTR b = bstr_in(name);
    HRESULT hr;
    VariantInit(&v);
    hr = IXMLDOMElement_getAttribute(el(o), b, &v);
    SysFreeString(b);
    *result = NULL;
    if (hr == S_OK && V_VT(&v) == VT_BSTR) hr = WindowsCreateString(V_BSTR(&v), SysStringLen(V_BSTR(&v)), result);
    VariantClear(&v);
    return SUCCEEDED(hr) ? S_OK : hr; /* missing attribute -> empty string */
}

HRESULT OV_IXmlElement_SetAttribute(struct obj *o, HSTRING name, HSTRING value)
{
    VARIANT v;
    BSTR b = bstr_in(name);
    HRESULT hr;
    V_VT(&v) = VT_BSTR;
    V_BSTR(&v) = bstr_in(value);
    hr = IXMLDOMElement_setAttribute(el(o), b, v);
    SysFreeString(b);
    VariantClear(&v);
    return hr;
}

HRESULT OV_IXmlElement_RemoveAttribute(struct obj *o, HSTRING name)
{
    BSTR b = bstr_in(name);
    HRESULT hr = IXMLDOMElement_removeAttribute(el(o), b);
    SysFreeString(b);
    return hr;
}

HRESULT OV_IXmlElement_GetAttributeNode(struct obj *o, HSTRING name, void **result)
{
    IXMLDOMAttribute *a = NULL;
    BSTR b = bstr_in(name);
    IXMLDOMElement_getAttributeNode(el(o), b, &a);
    SysFreeString(b);
    return wrap((IXMLDOMNode *)a, &IID_Windows_Data_Xml_Dom_IXmlAttribute, result);
}

HRESULT OV_IXmlElement_GetElementsByTagName(struct obj *o, HSTRING name, void **result)
{
    IXMLDOMNodeList *list = NULL;
    BSTR b = bstr_in(name);
    HRESULT hr = IXMLDOMElement_getElementsByTagName(el(o), b, &list);
    SysFreeString(b);
    if (FAILED(hr)) return hr;
    return list_wrap(list, result);
}

/* ---- character data / attributes -------------------------------------- */

HRESULT OV_IXmlCharacterData_get_Data(struct obj *o, HSTRING *result)
{
    BSTR b = NULL;
    return bstr_out(IXMLDOMCharacterData_get_data((IXMLDOMCharacterData *)dom(o), &b), &b, result);
}

HRESULT OV_IXmlCharacterData_put_Data(struct obj *o, HSTRING value)
{
    BSTR b = bstr_in(value);
    HRESULT hr = IXMLDOMCharacterData_put_data((IXMLDOMCharacterData *)dom(o), b);
    SysFreeString(b);
    return hr;
}

HRESULT OV_IXmlCharacterData_get_Length(struct obj *o, UINT32 *result)
{
    LONG len = 0;
    IXMLDOMCharacterData_get_length((IXMLDOMCharacterData *)dom(o), &len);
    *result = len;
    return S_OK;
}

HRESULT OV_IXmlAttribute_get_Name(struct obj *o, HSTRING *result)
{
    BSTR b = NULL;
    return bstr_out(IXMLDOMAttribute_get_name((IXMLDOMAttribute *)dom(o), &b), &b, result);
}

HRESULT OV_IXmlAttribute_get_Value(struct obj *o, HSTRING *result)
{
    BSTR b = NULL;
    return bstr_out(IXMLDOMNode_get_text(dom(o), &b), &b, result);
}

HRESULT OV_IXmlAttribute_put_Value(struct obj *o, HSTRING value)
{
    return OV_IXmlNodeSerializer_put_InnerText(o, value);
}

HRESULT OV_IXmlAttribute_get_Specified(struct obj *o, boolean *result)
{
    *result = TRUE;
    return S_OK;
}
