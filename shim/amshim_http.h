/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows.Web.Http implemented on WinHTTP, split over:
 *   amshim_http.c       helpers (object properties, URIs, methods)
 *   ov_http_content.c   HttpMethod, header collections, media types, content, request/response messages
 *   ov_http_send.c      the network part: HttpBaseProtocolFilter.SendRequestAsync on WinHTTP (gzip, worker)
 *   ov_http_client.c    HttpClient (routes requests through its filter chain)
 *   ov_httpheaders.c    response cache headers (Cache-Control max-age, Expires, Last-Modified)
 * HttpBaseProtocolFilter.SendRequestAsync does the network work on a worker thread and completes a
 * pending IAsyncOperationWithProgress. */
#pragma once
#include "gen/amshim_gen.h"
#include <winhttp.h>
#include <stdio.h>

#define VCALL(obj, slot, type) ((type)((void **)*(void ***)(obj))[slot])

extern const GUID IID_IUriRuntimeClass;

void http_put_s(struct obj *o, const WCHAR *name, const WCHAR *val);
void http_put_o(struct obj *o, const WCHAR *name, IInspectable *val);
IInspectable *http_get_o(struct obj *o, const WCHAR *name);
HRESULT http_new_obj(const WCHAR *cls, IInspectable **out);
struct obj *http_ours(IUnknown *p, const WCHAR *cls);
HRESULT http_uri_string(IUnknown *uri, HSTRING *out);
HRESULT http_method_string(IUnknown *method, HSTRING *out);

/* ov_http_content.c */
HRESULT http_method_create(const WCHAR *name, void **result);
struct obj *http_content_headers(struct obj *o);
HRESULT http_content_body(IUnknown *content, IInspectable **buf);
/* ov_http_send.c: the request -> response round trip (blocking) */
HRESULT http_perform(IInspectable *request, IInspectable **response);
