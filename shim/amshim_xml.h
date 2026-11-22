/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Data.Xml.Dom as a thin wrapper over Wine's MSXML 6 DOM. Every WinRT node object keeps an
 * IXMLDOMNode in o->priv.
 *   amshim_xml.c       helpers: wrapping DOM nodes in WinRT objects, strings
 *   ov_xml_document.c  XmlDocument (LoadXml etc.)
 *   ov_xml_node.c      IXmlNode, IXmlNodeSelector, IXmlNodeSerializer
 *   ov_xml_element.c   IXmlElement, character data, attributes
 *   ov_xml_list.c      XmlNodeList (IXmlNodeList, IVectorView, IIterable)
 *   ov_xml_tree.c      node navigation and the remaining tree edits */
#pragma once
#include "gen/amshim_gen.h"
#include <msxml6.h>

#define VCALL(obj, slot, type) ((type)((void **)*(void ***)(obj))[slot])
#define IID_NODE (&IID_Windows_Data_Xml_Dom_IXmlNode)

static inline IXMLDOMNode *dom(struct obj *o) { return o->priv; }

void node_free(struct obj *o);
IXMLDOMNode *dom_of(void *p);
HRESULT wrap(IXMLDOMNode *node, const GUID *iid, void **result);
HRESULT bstr_out(HRESULT hr, BSTR *pb, HSTRING *result);
BSTR bstr_in(HSTRING s);
HRESULT box_string(BSTR b, IInspectable **out);
HRESULT list_wrap(IXMLDOMNodeList *list, void **result);

HRESULT OV_IXmlDocument_GetElementsByTagName(struct obj *o, HSTRING name, void **result);
IXMLDOMElement *el(struct obj *o);
