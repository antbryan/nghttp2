/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2013 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef SHRPX_HTTP2_TEST_H
#define SHRPX_HTTP2_TEST_H

namespace shrpx {

void test_http2_split_add_header(void);
void test_http2_sort_nva(void);
void test_http2_check_http2_headers(void);
void test_http2_get_unique_header(void);
void test_http2_get_header(void);
void test_http2_value_lws(void);
void test_http2_concat_norm_headers(void);
void test_http2_copy_norm_headers_to_nva(void);
void test_http2_build_http1_headers_from_norm_headers(void);
void test_http2_lws(void);
void test_http2_rewrite_location_uri(void);

} // namespace shrpx

#endif // SHRPX_HTTP2_TEST_H
