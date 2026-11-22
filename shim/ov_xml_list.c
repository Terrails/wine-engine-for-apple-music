/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Xml.Dom: XmlNodeList (overview: amshim_xml.h). */
#include "amshim_xml.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

/* ---- XmlNodeList (IXmlNodeList, IVectorView<IXmlNode>, IIterable<IXmlNode>) */

HRESULT list_wrap(IXMLDOMNodeList *list, void **result)
{
    IInspectable *inst;
    HRESULT hr = create_object_by_name(L"Windows.Data.Xml.Dom.XmlNodeList", &inst);
    if (FAILED(hr)) { IXMLDOMNodeList_Release(list); return hr; }
    impl_from_inspectable(inst)->priv = list;
    impl_from_inspectable(inst)->priv_free = node_free;
    *result = inst;
    return S_OK;
}

static LONG list_len(struct obj *o)
{
    LONG len = 0;
    if (!wcscmp(o->cls->name, L"Windows.Data.Xml.Dom.XmlNamedNodeMap"))
        IXMLDOMNamedNodeMap_get_length((IXMLDOMNamedNodeMap *)o->priv, &len);
    else
        IXMLDOMNodeList_get_length((IXMLDOMNodeList *)o->priv, &len);
    return len;
}

static HRESULT list_item(struct obj *o, UINT32 i, void **result)
{
    IXMLDOMNode *n = NULL;
    if (!wcscmp(o->cls->name, L"Windows.Data.Xml.Dom.XmlNamedNodeMap"))
        IXMLDOMNamedNodeMap_get_item((IXMLDOMNamedNodeMap *)o->priv, i, &n);
    else
        IXMLDOMNodeList_get_item((IXMLDOMNodeList *)o->priv, i, &n);
    return wrap(n, IID_NODE, result);
}

HRESULT OV_IXmlNodeList_get_Length(struct obj *o, UINT32 *result) { *result = list_len(o); return S_OK; }
HRESULT OV_IXmlNodeList_Item(struct obj *o, UINT32 index, void **result) { return list_item(o, index, result); }
HRESULT OV_IXmlNamedNodeMap_get_Length(struct obj *o, UINT32 *result) { *result = list_len(o); return S_OK; }
HRESULT OV_IXmlNamedNodeMap_Item(struct obj *o, UINT32 index, void **result) { return list_item(o, index, result); }

HRESULT OV_IXmlNamedNodeMap_GetNamedItem(struct obj *o, HSTRING name, void **result)
{
    IXMLDOMNode *n = NULL;
    BSTR b = bstr_in(name);
    IXMLDOMNamedNodeMap_getNamedItem((IXMLDOMNamedNodeMap *)o->priv, b, &n);
    SysFreeString(b);
    return wrap(n, IID_NODE, result);
}

HRESULT OV_IVectorView_IXmlNode__get_Size(struct obj *o, UINT32 *result) { *result = list_len(o); return S_OK; }

HRESULT OV_IVectorView_IXmlNode__GetAt(struct obj *o, UINT32 index, void **result)
{
    if (index >= (UINT32)list_len(o)) { *result = NULL; return E_BOUNDS; }
    return list_item(o, index, result);
}

HRESULT OV_IVectorView_IXmlNode__GetMany(struct obj *o, UINT32 start, UINT32 items_len, void **items, UINT32 *result)
{
    UINT32 i, n = list_len(o);
    for (i = 0; start + i < n && i < items_len; i++) list_item(o, start + i, &items[i]);
    *result = i;
    return S_OK;
}

struct niter { struct obj *list; UINT32 pos; };

static void niter_free(struct obj *o)
{
    struct niter *it = o->priv;
    IInspectable_Release((IInspectable *)&it->list->ifcs[0]);
    free(it);
}

HRESULT OV_IIterable_IXmlNode__First(struct obj *o, void **result)
{
    struct niter *it;
    HRESULT hr = create_object(&class_amshim_XmlNodeIterator, (IInspectable **)result);
    if (FAILED(hr)) return hr;
    it = calloc(1, sizeof(*it));
    it->list = o;
    IInspectable_AddRef((IInspectable *)&o->ifcs[0]);
    impl_from_inspectable(*result)->priv = it;
    impl_from_inspectable(*result)->priv_free = niter_free;
    return S_OK;
}

HRESULT OV_IIterator_IXmlNode__get_Current(struct obj *o, void **result)
{
    struct niter *it = o->priv;
    return OV_IVectorView_IXmlNode__GetAt(it->list, it->pos, result);
}

HRESULT OV_IIterator_IXmlNode__get_HasCurrent(struct obj *o, boolean *result)
{
    struct niter *it = o->priv;
    *result = it->pos < (UINT32)list_len(it->list);
    return S_OK;
}

HRESULT OV_IIterator_IXmlNode__MoveNext(struct obj *o, boolean *result)
{
    struct niter *it = o->priv;
    UINT32 n = list_len(it->list);
    if (it->pos < n) it->pos++;
    *result = it->pos < n;
    return S_OK;
}
