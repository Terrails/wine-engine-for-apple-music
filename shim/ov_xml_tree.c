/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Xml.Dom: navigation and tree edits (overview: amshim_xml.h). */
#include "amshim_xml.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- node navigation and the remaining tree edits ---------------------- */

HRESULT OV_IXmlNode_get_ParentNode(struct obj *o, void **result)
{
    IXMLDOMNode *n = NULL;
    IXMLDOMNode_get_parentNode(dom(o), &n);
    return wrap(n, &IID_Windows_Data_Xml_Dom_IXmlNode, result);
}

HRESULT OV_IXmlNode_get_FirstChild(struct obj *o, void **result)
{
    IXMLDOMNode *n = NULL;
    IXMLDOMNode_get_firstChild(dom(o), &n);
    return wrap(n, &IID_Windows_Data_Xml_Dom_IXmlNode, result);
}

HRESULT OV_IXmlNode_get_LastChild(struct obj *o, void **result)
{
    IXMLDOMNode *n = NULL;
    IXMLDOMNode_get_lastChild(dom(o), &n);
    return wrap(n, &IID_Windows_Data_Xml_Dom_IXmlNode, result);
}

HRESULT OV_IXmlNode_get_PreviousSibling(struct obj *o, void **result)
{
    IXMLDOMNode *n = NULL;
    IXMLDOMNode_get_previousSibling(dom(o), &n);
    return wrap(n, &IID_Windows_Data_Xml_Dom_IXmlNode, result);
}

HRESULT OV_IXmlNode_get_NextSibling(struct obj *o, void **result)
{
    IXMLDOMNode *n = NULL;
    IXMLDOMNode_get_nextSibling(dom(o), &n);
    return wrap(n, &IID_Windows_Data_Xml_Dom_IXmlNode, result);
}

HRESULT OV_IXmlNode_InsertBefore(struct obj *o, void *child, void *ref, void **result)
{
    IXMLDOMNode *n = NULL;
    VARIANT v;
    HRESULT hr;
    V_VT(&v) = ref ? VT_UNKNOWN : VT_NULL;
    V_UNKNOWN(&v) = (IUnknown *)dom_of(ref);
    hr = IXMLDOMNode_insertBefore(dom(o), dom_of(child), v, &n);
    *result = NULL;
    if (FAILED(hr)) return hr;
    if (n) IXMLDOMNode_Release(n);
    *result = child;
    IUnknown_AddRef((IUnknown *)child);
    return S_OK;
}

HRESULT OV_IXmlNode_ReplaceChild(struct obj *o, void *child, void *old, void **result)
{
    IXMLDOMNode *n = NULL;
    HRESULT hr = IXMLDOMNode_replaceChild(dom(o), dom_of(child), dom_of(old), &n);
    *result = NULL;
    if (FAILED(hr)) return hr;
    if (n) IXMLDOMNode_Release(n);
    *result = old;
    if (old) IUnknown_AddRef((IUnknown *)old);
    return S_OK;
}
