/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Xml.Dom: IXmlNode, selector, serializer (overview: amshim_xml.h). */
#include "amshim_xml.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- IXmlNode --------------------------------------------------------- */

HRESULT OV_IXmlNode_get_NodeType(struct obj *o, INT32 *result)
{
    DOMNodeType t = NODE_ELEMENT;
    IXMLDOMNode_get_nodeType(dom(o), &t);
    *result = t; /* WinRT NodeType values match DOM node types */
    return S_OK;
}

HRESULT OV_IXmlNode_get_NodeName(struct obj *o, HSTRING *result)
{
    BSTR b = NULL;
    HRESULT hr = bstr_out(IXMLDOMNode_get_nodeName(dom(o), &b), &b, result);
    TRACE("-> %s\n", debugstr_hstring(*result));
    return hr;
}

HRESULT OV_IXmlNode_get_NodeValue(struct obj *o, IInspectable **result)
{
    VARIANT v;
    HRESULT hr;
    VariantInit(&v);
    *result = NULL;
    hr = IXMLDOMNode_get_nodeValue(dom(o), &v);
    if (SUCCEEDED(hr) && V_VT(&v) == VT_BSTR) hr = box_string(V_BSTR(&v), result);
    VariantClear(&v);
    return hr == S_FALSE ? S_OK : hr;
}

HRESULT OV_IXmlNode_get_LocalName(struct obj *o, IInspectable **result)
{
    BSTR b = NULL;
    HRESULT hr = IXMLDOMNode_get_baseName(dom(o), &b);
    if (SUCCEEDED(hr)) hr = box_string(b, result); else *result = NULL;
    SysFreeString(b);
    return S_OK;
}

HRESULT OV_IXmlNode_get_NamespaceUri(struct obj *o, IInspectable **result)
{
    BSTR b = NULL;
    *result = NULL;
    if (SUCCEEDED(IXMLDOMNode_get_namespaceURI(dom(o), &b)) && b && *b) box_string(b, result);
    SysFreeString(b);
    return S_OK;
}

HRESULT OV_IXmlNode_get_Prefix(struct obj *o, IInspectable **result)
{
    BSTR b = NULL;
    *result = NULL;
    if (SUCCEEDED(IXMLDOMNode_get_prefix(dom(o), &b)) && b && *b) box_string(b, result);
    SysFreeString(b);
    return S_OK;
}

HRESULT OV_IXmlNode_HasChildNodes(struct obj *o, boolean *result)
{
    VARIANT_BOOL b = VARIANT_FALSE;
    IXMLDOMNode_hasChildNodes(dom(o), &b);
    *result = !!b;
    return S_OK;
}

HRESULT OV_IXmlNode_get_ChildNodes(struct obj *o, void **result)
{
    IXMLDOMNodeList *list = NULL;
    HRESULT hr = IXMLDOMNode_get_childNodes(dom(o), &list);
    if (FAILED(hr)) return hr;
    return list_wrap(list, result);
}

HRESULT OV_IXmlNode_get_OwnerDocument(struct obj *o, void **result)
{
    IXMLDOMDocument *d = NULL;
    IXMLDOMNode_get_ownerDocument(dom(o), &d);
    return wrap((IXMLDOMNode *)d, &IID_Windows_Data_Xml_Dom_IXmlDocument, result);
}

HRESULT OV_IXmlNode_AppendChild(struct obj *o, void *child, void **result)
{
    IXMLDOMNode *n = NULL;
    HRESULT hr = IXMLDOMNode_appendChild(dom(o), dom_of(child), &n);
    if (FAILED(hr)) return hr;
    if (n) IXMLDOMNode_Release(n);
    *result = child;
    IUnknown_AddRef((IUnknown *)child);
    return S_OK;
}

HRESULT OV_IXmlNode_RemoveChild(struct obj *o, void *child, void **result)
{
    IXMLDOMNode *n = NULL;
    HRESULT hr = IXMLDOMNode_removeChild(dom(o), dom_of(child), &n);
    if (FAILED(hr)) return hr;
    if (n) IXMLDOMNode_Release(n);
    *result = child;
    IUnknown_AddRef((IUnknown *)child);
    return S_OK;
}

HRESULT OV_IXmlNode_CloneNode(struct obj *o, boolean deep, void **result)
{
    IXMLDOMNode *n = NULL;
    HRESULT hr = IXMLDOMNode_cloneNode(dom(o), deep ? VARIANT_TRUE : VARIANT_FALSE, &n);
    if (FAILED(hr)) return hr;
    return wrap(n, IID_NODE, result);
}

HRESULT OV_IXmlNode_get_Attributes(struct obj *o, void **result)
{
    IXMLDOMNamedNodeMap *map = NULL;
    IInspectable *inst;
    HRESULT hr = IXMLDOMNode_get_attributes(dom(o), &map);
    *result = NULL;
    if (FAILED(hr) || !map) return S_OK;
    if (FAILED(hr = create_object_by_name(L"Windows.Data.Xml.Dom.XmlNamedNodeMap", &inst)))
    {
        IXMLDOMNamedNodeMap_Release(map);
        return hr;
    }
    impl_from_inspectable(inst)->priv = map;
    impl_from_inspectable(inst)->priv_free = node_free;
    *result = inst;
    return S_OK;
}

/* ---- IXmlNodeSelector / IXmlNodeSerializer --------------------------- */

HRESULT OV_IXmlNodeSelector_SelectSingleNode(struct obj *o, HSTRING xpath, void **result)
{
    IXMLDOMNode *n = NULL;
    BSTR b = bstr_in(xpath);
    HRESULT hr = IXMLDOMNode_selectSingleNode(dom(o), b, &n);
    SysFreeString(b);
    if (FAILED(hr)) return hr;
    return wrap(n, IID_NODE, result);
}

HRESULT OV_IXmlNodeSelector_SelectNodes(struct obj *o, HSTRING xpath, void **result)
{
    IXMLDOMNodeList *list = NULL;
    BSTR b = bstr_in(xpath);
    HRESULT hr = IXMLDOMNode_selectNodes(dom(o), b, &list);
    SysFreeString(b);
    if (FAILED(hr)) return hr;
    return list_wrap(list, result);
}

HRESULT OV_IXmlNodeSelector_SelectSingleNodeNS(struct obj *o, HSTRING xpath, IInspectable *ns, void **result) { return OV_IXmlNodeSelector_SelectSingleNode(o, xpath, result); }
HRESULT OV_IXmlNodeSelector_SelectNodesNS(struct obj *o, HSTRING xpath, IInspectable *ns, void **result) { return OV_IXmlNodeSelector_SelectNodes(o, xpath, result); }

HRESULT OV_IXmlNodeSerializer_GetXml(struct obj *o, HSTRING *result)
{
    BSTR b = NULL;
    return bstr_out(IXMLDOMNode_get_xml(dom(o), &b), &b, result);
}

HRESULT OV_IXmlNodeSerializer_get_InnerText(struct obj *o, HSTRING *result)
{
    BSTR b = NULL;
    return bstr_out(IXMLDOMNode_get_text(dom(o), &b), &b, result);
}

HRESULT OV_IXmlNodeSerializer_put_InnerText(struct obj *o, HSTRING value)
{
    BSTR b = bstr_in(value);
    HRESULT hr = IXMLDOMNode_put_text(dom(o), b);
    SysFreeString(b);
    return hr;
}
