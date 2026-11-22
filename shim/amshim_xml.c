/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Xml.Dom: helpers (overview: amshim_xml.h). */
#include "gen/amshim_gen.h"
#include <initguid.h>   /* the MSXML GUIDs are defined here, once */
#include "amshim_xml.h"

WINE_DEFAULT_DEBUG_CHANNEL(amshim);

void node_free(struct obj *o)
{
    if (o->priv) IUnknown_Release((IUnknown *)o->priv);
}

/* the DOM node behind one of our WinRT node interface pointers */
IXMLDOMNode *dom_of(void *p)
{
    return p ? ((struct ifc *)p)->obj->priv : NULL;
}

static const WCHAR *class_for_type(DOMNodeType t)
{
    switch (t)
    {
    case NODE_ATTRIBUTE: return L"Windows.Data.Xml.Dom.XmlAttribute";
    case NODE_TEXT: return L"Windows.Data.Xml.Dom.XmlText";
    case NODE_CDATA_SECTION: return L"Windows.Data.Xml.Dom.XmlCDataSection";
    case NODE_PROCESSING_INSTRUCTION: return L"Windows.Data.Xml.Dom.XmlProcessingInstruction";
    case NODE_COMMENT: return L"Windows.Data.Xml.Dom.XmlComment";
    case NODE_DOCUMENT: return L"Windows.Data.Xml.Dom.XmlDocument";
    case NODE_DOCUMENT_TYPE: return L"Windows.Data.Xml.Dom.XmlDocumentType";
    default: return L"Windows.Data.Xml.Dom.XmlElement";
    }
}

/* Wrap a DOM node (takes ownership of the reference) and return interface iid. */
HRESULT wrap(IXMLDOMNode *node, const GUID *iid, void **result)
{
    IInspectable *inst;
    DOMNodeType t = NODE_ELEMENT;
    HRESULT hr;

    *result = NULL;
    if (!node) return S_OK;
    IXMLDOMNode_get_nodeType(node, &t);
    {
        /* keep the most derived DOM interface; all of them start with IXMLDOMNode */
        const GUID *want = t == NODE_ELEMENT ? &IID_IXMLDOMElement : t == NODE_ATTRIBUTE ? &IID_IXMLDOMAttribute :
                           (t == NODE_TEXT || t == NODE_CDATA_SECTION || t == NODE_COMMENT) ? &IID_IXMLDOMCharacterData :
                           t == NODE_DOCUMENT ? &IID_IXMLDOMDocument2 : NULL;
        IXMLDOMNode *derived;
        if (want && SUCCEEDED(IXMLDOMNode_QueryInterface(node, want, (void **)&derived)))
        {
            IXMLDOMNode_Release(node);
            node = derived;
        }
    }
    if (FAILED(hr = create_object_by_name(class_for_type(t), &inst))) { IXMLDOMNode_Release(node); return hr; }
    if (t == NODE_DOCUMENT)
    {
        /* drop the fresh DOM document INIT_XmlDocument created */
        struct obj *io = impl_from_inspectable(inst);
        if (io->priv) IUnknown_Release((IUnknown *)io->priv);
    }
    impl_from_inspectable(inst)->priv = node;
    impl_from_inspectable(inst)->priv_free = node_free;
    hr = iid ? IInspectable_QueryInterface(inst, iid, result) : (IInspectable_AddRef(inst), *result = inst, S_OK);
    IInspectable_Release(inst);
    return hr;
}


/* `b` is taken by pointer on purpose: callers write bstr_out(get_x(&b), &b, result), and C leaves
 * the argument evaluation order open (here the BSTR value was read before the getter ran). */
HRESULT bstr_out(HRESULT hr, BSTR *pb, HSTRING *result)
{
    BSTR b = *pb;
    *result = NULL;
    if (SUCCEEDED(hr) && b) hr = WindowsCreateString(b, SysStringLen(b), result);
    SysFreeString(b);
    return hr == S_FALSE ? S_OK : hr;
}

BSTR bstr_in(HSTRING s)
{
    UINT32 len;
    const WCHAR *p = WindowsGetStringRawBuffer(s, &len);
    return SysAllocStringLen(p, len);
}

/* box a string as IPropertyValue (Wine's Windows.Foundation.PropertyValue) */
HRESULT box_string(BSTR b, IInspectable **out)
{
    static const GUID IID_IPropertyValueStatics = {0x629bdbc8, 0xd932, 0x4ff4, {0x96, 0xb9, 0x8d, 0x96, 0xc5, 0xc1, 0xe8, 0x58}};
    static IUnknown *statics;
    HSTRING s, cls;
    HRESULT hr;

    *out = NULL;
    if (!b) return S_OK;
    if (!statics)
    {
        hstring_from_w(L"Windows.Foundation.PropertyValue", &cls);
        hr = RoGetActivationFactory(cls, &IID_IPropertyValueStatics, (void **)&statics);
        WindowsDeleteString(cls);
        if (FAILED(hr)) return hr;
    }
    WindowsCreateString(b, SysStringLen(b), &s);
    hr = VCALL(statics, 18, HRESULT (WINAPI *)(IUnknown *, HSTRING, IInspectable **))(statics, s, out); /* CreateString */
    WindowsDeleteString(s);
    return hr;
}
