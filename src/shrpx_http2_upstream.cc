/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
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
#include "shrpx_http2_upstream.h"

#include <netinet/tcp.h>
#include <assert.h>
#include <cerrno>
#include <sstream>

#include "shrpx_client_handler.h"
#include "shrpx_https_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_downstream_connection.h"
#include "shrpx_config.h"
#include "shrpx_http.h"
#include "shrpx_accesslog.h"
#include "http2.h"
#include "util.h"
#include "base64.h"
#include "app_helper.h"

using namespace nghttp2;

namespace shrpx {

namespace {
const size_t OUTBUF_MAX_THRES = 64*1024;
} // namespace

namespace {
int on_stream_close_callback
(nghttp2_session *session, int32_t stream_id, nghttp2_error_code error_code,
 void *user_data)
{
  auto upstream = static_cast<Http2Upstream*>(user_data);
  if(LOG_ENABLED(INFO)) {
    ULOG(INFO, upstream) << "Stream stream_id=" << stream_id
                         << " is being closed";
  }
  auto downstream = upstream->find_downstream(stream_id);
  if(downstream) {
    if(downstream->get_request_state() == Downstream::CONNECT_FAIL) {
      upstream->remove_downstream(downstream);
      delete downstream;
    } else {
      downstream->set_request_state(Downstream::STREAM_CLOSED);
      if(downstream->get_response_state() == Downstream::MSG_COMPLETE) {
        // At this point, downstream response was read
        if(!downstream->get_upgraded() &&
           !downstream->get_response_connection_close()) {
          // Keep-alive
          auto dconn = downstream->get_downstream_connection();
          if(dconn) {
            dconn->detach_downstream(downstream);
          }
        }
        upstream->remove_downstream(downstream);
        delete downstream;
      } else {
        // At this point, downstream read may be paused.

        // If shrpx_downstream::push_request_headers() failed, the
        // error is handled here.
        upstream->remove_downstream(downstream);
        delete downstream;
        // How to test this case? Request sufficient large download
        // and make client send RST_STREAM after it gets first DATA
        // frame chunk.
      }
    }
  }
  return 0;
}
} // namespace

int Http2Upstream::upgrade_upstream(HttpsUpstream *http)
{
  int rv;
  std::string settings_payload;
  auto downstream = http->get_downstream();
  for(auto& hd : downstream->get_request_headers()) {
    if(util::strieq(hd.first.c_str(), "http2-settings")) {
      auto val = hd.second;
      util::to_base64(val);
      settings_payload = base64::decode(std::begin(val), std::end(val));
      break;
    }
  }
  rv = nghttp2_session_upgrade
    (session_,
     reinterpret_cast<const uint8_t*>(settings_payload.c_str()),
     settings_payload.size(),
     nullptr);
  if(rv != 0) {
    ULOG(WARNING, this) << "nghttp2_session_upgrade() returned error: "
                        << nghttp2_strerror(rv);
    return -1;
  }
  pre_upstream_.reset(http);
  http->pop_downstream();
  downstream->reset_upstream(this);
  add_downstream(downstream);
  downstream->init_response_body_buf();
  downstream->set_stream_id(1);
  downstream->set_priority(0);

  return 0;
}

namespace {
void settings_timeout_cb(evutil_socket_t fd, short what, void *arg)
{
  auto upstream = static_cast<Http2Upstream*>(arg);
  ULOG(INFO, upstream) << "SETTINGS timeout";
  if(upstream->terminate_session(NGHTTP2_SETTINGS_TIMEOUT) != 0) {
    delete upstream->get_client_handler();
    return;
  }
  if(upstream->send() != 0) {
    delete upstream->get_client_handler();
  }
}
} // namespace

int Http2Upstream::start_settings_timer()
{
  int rv;
  // We submit SETTINGS only once
  if(settings_timerev_) {
    return 0;
  }
  settings_timerev_ = evtimer_new(handler_->get_evbase(), settings_timeout_cb,
                                  this);
  if(settings_timerev_ == nullptr) {
    return -1;
  }
  // SETTINGS ACK timeout is 10 seconds for now
  timeval settings_timeout = { 10, 0 };
  rv = evtimer_add(settings_timerev_, &settings_timeout);
  if(rv == -1) {
    return -1;
  }
  return 0;
}

void Http2Upstream::stop_settings_timer()
{
  if(settings_timerev_ == nullptr) {
    return;
  }
  event_free(settings_timerev_);
  settings_timerev_ = nullptr;
}

namespace {
int on_header_callback(nghttp2_session *session,
                       const nghttp2_frame *frame,
                       const uint8_t *name, size_t namelen,
                       const uint8_t *value, size_t valuelen,
                       void *user_data)
{
  if(get_config()->upstream_frame_debug) {
    verbose_on_header_callback(session, frame, name, namelen, value, valuelen,
                               user_data);
  }
  if(frame->hd.type != NGHTTP2_HEADERS ||
     frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  auto upstream = static_cast<Http2Upstream*>(user_data);
  auto downstream = upstream->find_downstream(frame->hd.stream_id);
  if(!downstream) {
    return 0;
  }
  if(downstream->get_request_headers_sum() > Downstream::MAX_HEADERS_SUM) {
    if(LOG_ENABLED(INFO)) {
      ULOG(INFO, upstream) << "Too large header block size="
                           << downstream->get_request_headers_sum();
    }
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  if(!http2::check_nv(name, namelen, value, valuelen)) {
    return 0;
  }
  downstream->split_add_request_header(name, namelen, value, valuelen);
  return 0;
}
} // namespace

namespace {
int on_begin_headers_callback(nghttp2_session *session,
                              const nghttp2_frame *frame,
                              void *user_data)
{
  auto upstream = static_cast<Http2Upstream*>(user_data);

  if(frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  if(LOG_ENABLED(INFO)) {
    ULOG(INFO, upstream) << "Received upstream request HEADERS stream_id="
                         << frame->hd.stream_id;
  }
  auto downstream = new Downstream(upstream,
                                   frame->hd.stream_id,
                                   frame->headers.pri);
  upstream->add_downstream(downstream);
  downstream->init_response_body_buf();

  return 0;
}
} // namespace

namespace {
int on_request_headers(Http2Upstream *upstream,
                       nghttp2_session *session,
                       const nghttp2_frame *frame)
{
  int rv;
  auto downstream = upstream->find_downstream(frame->hd.stream_id);
  if(!downstream) {
    return 0;
  }

  downstream->normalize_request_headers();
  auto& nva = downstream->get_request_headers();

  if(LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for(auto& nv : nva) {
      ss << TTY_HTTP_HD << nv.first << TTY_RST << ": " << nv.second << "\n";
    }
    ULOG(INFO, upstream) << "HTTP request headers. stream_id="
                         << downstream->get_stream_id()
                         << "\n" << ss.str();
  }

  if(get_config()->http2_upstream_dump_request_header) {
    http2::dump_nv(get_config()->http2_upstream_dump_request_header, nva);
  }

  if(!http2::check_http2_headers(nva)) {
    upstream->rst_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
    return 0;
  }

  auto host = http2::get_unique_header(nva, "host");
  auto authority = http2::get_unique_header(nva, ":authority");
  auto path = http2::get_unique_header(nva, ":path");
  auto method = http2::get_unique_header(nva, ":method");
  auto scheme = http2::get_unique_header(nva, ":scheme");
  bool is_connect = method  && "CONNECT" == method->second;
  bool having_host = http2::non_empty_value(host);
  bool having_authority = http2::non_empty_value(authority);

  if(is_connect) {
    // Here we strictly require :authority header field.
    if(scheme || path || !having_authority) {
      upstream->rst_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
      return 0;
    }
  } else {
    // For proxy, :authority is required. Otherwise, we can accept
    // :authority or host for methods.
    if(!http2::non_empty_value(method) ||
       !http2::non_empty_value(scheme) ||
       (get_config()->http2_proxy && !having_authority) ||
       (!get_config()->http2_proxy && !having_authority && !having_host) ||
       !http2::non_empty_value(path)) {
      upstream->rst_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
      return 0;
    }
  }
  if(!is_connect &&
     (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) == 0) {
    auto content_length = http2::get_header(nva, "content-length");
    if(!content_length || http2::value_lws(content_length)) {
      // If content-length is missing,
      // Downstream::push_upload_data_chunk will fail and
      upstream->rst_stream(downstream, NGHTTP2_PROTOCOL_ERROR);
      return 0;
    }
  }

  downstream->set_request_method(http2::value_to_str(method));
  downstream->set_request_http2_scheme(http2::value_to_str(scheme));
  downstream->set_request_http2_authority(http2::value_to_str(authority));
  downstream->set_request_path(http2::value_to_str(path));

  downstream->check_upgrade_request();

  auto dconn = upstream->get_client_handler()->get_downstream_connection();
  rv = dconn->attach_downstream(downstream);
  if(rv != 0) {
    // If downstream connection fails, issue RST_STREAM.
    upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    downstream->set_request_state(Downstream::CONNECT_FAIL);
    return 0;
  }
  rv = downstream->push_request_headers();
  if(rv != 0) {
    upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    return 0;
  }
  downstream->set_request_state(Downstream::HEADER_COMPLETE);
  if(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
    downstream->set_request_state(Downstream::MSG_COMPLETE);
  }

  return 0;
}

} // namespace

namespace {
int on_frame_recv_callback
(nghttp2_session *session, const nghttp2_frame *frame, void *user_data)
{
  int rv;
  if(get_config()->upstream_frame_debug) {
    verbose_on_frame_recv_callback(session, frame, user_data);
  }
  auto upstream = static_cast<Http2Upstream*>(user_data);
  switch(frame->hd.type) {
  case NGHTTP2_DATA: {
    auto downstream = upstream->find_downstream(frame->hd.stream_id);
    if(!downstream) {
      break;
    }
    if(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      downstream->end_upload_data();
      downstream->set_request_state(Downstream::MSG_COMPLETE);
    }
    break;
  }
  case NGHTTP2_HEADERS:
    return on_request_headers(upstream, session, frame);
  case NGHTTP2_PRIORITY: {
    auto downstream = upstream->find_downstream(frame->hd.stream_id);
    if(!downstream) {
      break;
    }
    rv = downstream->change_priority(frame->priority.pri);
    if(rv != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    break;
  }
  case NGHTTP2_SETTINGS:
    if((frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
      break;
    }
    upstream->stop_settings_timer();
    break;
  case NGHTTP2_PUSH_PROMISE:
    rv = nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE,
                                   frame->push_promise.promised_stream_id,
                                   NGHTTP2_REFUSED_STREAM);
    if(rv != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    break;
  default:
    break;
  }
  return 0;
}
} // namespace

namespace {
int on_data_chunk_recv_callback(nghttp2_session *session,
                                uint8_t flags, int32_t stream_id,
                                const uint8_t *data, size_t len,
                                void *user_data)
{
  auto upstream = static_cast<Http2Upstream*>(user_data);
  auto downstream = upstream->find_downstream(stream_id);
  if(downstream) {
    if(downstream->push_upload_data_chunk(data, len) != 0) {
      upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
      return 0;
    }
  }
  return 0;
}
} // namespace

namespace {
int on_frame_send_callback(nghttp2_session* session,
                           const nghttp2_frame *frame, void *user_data)
{
  if(get_config()->upstream_frame_debug) {
    verbose_on_frame_send_callback(session, frame, user_data);
  }
  auto upstream = static_cast<Http2Upstream*>(user_data);
  if(frame->hd.type == NGHTTP2_SETTINGS &&
     (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
    if(upstream->start_settings_timer() != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }
  return 0;
}
} // namespace

namespace {
int on_frame_not_send_callback(nghttp2_session *session,
                               const nghttp2_frame *frame,
                               int lib_error_code, void *user_data)
{
  auto upstream = static_cast<Http2Upstream*>(user_data);
  ULOG(WARNING, upstream) << "Failed to send control frame type="
                          << static_cast<uint32_t>(frame->hd.type)
                          << ", lib_error_code=" << lib_error_code << ":"
                          << nghttp2_strerror(lib_error_code);
  if(frame->hd.type == NGHTTP2_HEADERS &&
     frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
    // To avoid stream hanging around, issue RST_STREAM.
    auto downstream = upstream->find_downstream(frame->hd.stream_id);
    if(downstream) {
      upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
    }
  }
  return 0;
}
} // namespace

namespace {
int on_unknown_frame_recv_callback(nghttp2_session *session,
                                   const uint8_t *head, size_t headlen,
                                   const uint8_t *payload, size_t payloadlen,
                                   void *user_data)
{
  auto upstream = static_cast<Http2Upstream*>(user_data);
  if(LOG_ENABLED(INFO)) {
    ULOG(INFO, upstream) << "Received unknown control frame.";
  }
  return 0;
}
} // namespace

namespace {
nghttp2_error_code infer_upstream_rst_stream_error_code
(nghttp2_error_code downstream_error_code)
{
  // Only propagate NGHTTP2_REFUSED_STREAM so that upstream client
  // can resend request.
  if(downstream_error_code != NGHTTP2_REFUSED_STREAM) {
    return NGHTTP2_INTERNAL_ERROR;
  } else {
    return downstream_error_code;
  }
}
} // namespace

Http2Upstream::Http2Upstream(ClientHandler *handler)
  : handler_(handler),
    session_(nullptr),
    settings_timerev_(nullptr)
{
  handler->set_upstream_timeouts(&get_config()->http2_upstream_read_timeout,
                                 &get_config()->upstream_write_timeout);

  nghttp2_session_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.on_stream_close_callback = on_stream_close_callback;
  callbacks.on_frame_recv_callback = on_frame_recv_callback;
  callbacks.on_data_chunk_recv_callback = on_data_chunk_recv_callback;
  callbacks.on_frame_send_callback = on_frame_send_callback;
  callbacks.on_frame_not_send_callback = on_frame_not_send_callback;
  callbacks.on_unknown_frame_recv_callback = on_unknown_frame_recv_callback;
  callbacks.on_header_callback = on_header_callback;
  callbacks.on_begin_headers_callback = on_begin_headers_callback;
  if(get_config()->padding) {
    callbacks.select_padding_callback = http::select_padding_callback;
  }

  nghttp2_opt_set opt_set;
  opt_set.no_auto_stream_window_update = 1;
  opt_set.no_auto_connection_window_update = 1;
  uint32_t opt_set_mask =
    NGHTTP2_OPT_NO_AUTO_STREAM_WINDOW_UPDATE |
    NGHTTP2_OPT_NO_AUTO_CONNECTION_WINDOW_UPDATE;
  int rv;
  rv = nghttp2_session_server_new2(&session_, &callbacks, this,
                                   opt_set_mask, &opt_set);
  assert(rv == 0);

  flow_control_ = true;

  // TODO Maybe call from outside?
  nghttp2_settings_entry entry[2];
  entry[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
  entry[0].value = get_config()->http2_max_concurrent_streams;

  entry[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
  entry[1].value = (1 << get_config()->http2_upstream_window_bits) - 1;

  rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE,
                               entry,
                               sizeof(entry)/sizeof(nghttp2_settings_entry));
  assert(rv == 0);

  if(get_config()->http2_upstream_connection_window_bits > 16) {
    int32_t delta = (1 << get_config()->http2_upstream_connection_window_bits)
      - 1 - NGHTTP2_INITIAL_CONNECTION_WINDOW_SIZE;
    rv = nghttp2_submit_window_update(session_, NGHTTP2_FLAG_NONE, 0, delta);
    assert(rv == 0);
  }
}

Http2Upstream::~Http2Upstream()
{
  nghttp2_session_del(session_);
  if(settings_timerev_) {
    event_free(settings_timerev_);
  }
}

int Http2Upstream::on_read()
{
  ssize_t rv = 0;
  auto bev = handler_->get_bev();
  auto input = bufferevent_get_input(bev);
  auto inputlen = evbuffer_get_length(input);
  auto mem = evbuffer_pullup(input, -1);

  rv = nghttp2_session_mem_recv(session_, mem, inputlen);
  if(rv < 0) {
    ULOG(ERROR, this) << "nghttp2_session_recv() returned error: "
                      << nghttp2_strerror(rv);
    return -1;
  }
  evbuffer_drain(input, rv);
  return send();
}

int Http2Upstream::on_write()
{
  return send();
}

// After this function call, downstream may be deleted.
int Http2Upstream::send()
{
  int rv;
  auto bev = handler_->get_bev();
  auto output = bufferevent_get_output(bev);
  for(;;) {
    // Check buffer length and return WOULDBLOCK if it is large enough.
    if(handler_->get_outbuf_length() > OUTBUF_MAX_THRES) {
      break;
    }

    const uint8_t *data;
    auto datalen = nghttp2_session_mem_send(session_, &data);

    if(datalen < 0) {
      ULOG(ERROR, this) << "nghttp2_session_mem_send() returned error: "
                        << nghttp2_strerror(datalen);
      return -1;
    }
    if(datalen == 0) {
      break;
    }
    rv = evbuffer_add(output, data, datalen);
    if(rv == -1) {
      ULOG(FATAL, this) << "evbuffer_add() failed";
      return -1;
    }
  }
  if(nghttp2_session_want_read(session_) == 0 &&
     nghttp2_session_want_write(session_) == 0 &&
     handler_->get_outbuf_length() == 0) {
    if(LOG_ENABLED(INFO)) {
      ULOG(INFO, this) << "No more read/write for this HTTP2 session";
    }
    return -1;
  }
  return 0;
}

int Http2Upstream::on_event()
{
  return 0;
}

ClientHandler* Http2Upstream::get_client_handler() const
{
  return handler_;
}

namespace {
void downstream_readcb(bufferevent *bev, void *ptr)
{
  auto dconn = static_cast<DownstreamConnection*>(ptr);
  auto downstream = dconn->get_downstream();
  auto upstream = static_cast<Http2Upstream*>(downstream->get_upstream());
  if(downstream->get_request_state() == Downstream::STREAM_CLOSED) {
    // If upstream HTTP2 stream was closed, we just close downstream,
    // because there is no consumer now. Downstream connection is also
    // closed in this case.
    upstream->remove_downstream(downstream);
    delete downstream;
    return;
  }

  if(downstream->get_response_state() == Downstream::MSG_RESET) {
    // The downstream stream was reset (canceled). In this case,
    // RST_STREAM to the upstream and delete downstream connection
    // here. Deleting downstream will be taken place at
    // on_stream_close_callback.
    upstream->rst_stream(downstream, infer_upstream_rst_stream_error_code
                         (downstream->get_response_rst_stream_error_code()));
    downstream->set_downstream_connection(0);
    delete dconn;
    dconn = 0;
  } else {
    int rv = downstream->on_read();
    if(rv != 0) {
      if(LOG_ENABLED(INFO)) {
        DCLOG(INFO, dconn) << "HTTP parser failure";
      }
      if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
        upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
      } else if(downstream->get_response_state() != Downstream::MSG_COMPLETE) {
        // If response was completed, then don't issue RST_STREAM
        if(upstream->error_reply(downstream, 502) != 0) {
          delete upstream->get_client_handler();
          return;
        }
      }
      downstream->set_response_state(Downstream::MSG_COMPLETE);
      // Clearly, we have to close downstream connection on http parser
      // failure.
      downstream->set_downstream_connection(0);
      delete dconn;
      dconn = 0;
    }
  }
  if(upstream->send() != 0) {
    delete upstream->get_client_handler();
    return;
  }
  // At this point, downstream may be deleted.
}
} // namespace

namespace {
void downstream_writecb(bufferevent *bev, void *ptr)
{
  if(evbuffer_get_length(bufferevent_get_output(bev)) > 0) {
    return;
  }
  auto dconn = static_cast<DownstreamConnection*>(ptr);
  auto downstream = dconn->get_downstream();
  auto upstream = static_cast<Http2Upstream*>(downstream->get_upstream());
  upstream->resume_read(SHRPX_NO_BUFFER, downstream);
}
} // namespace

namespace {
void downstream_eventcb(bufferevent *bev, short events, void *ptr)
{
  auto dconn = static_cast<DownstreamConnection*>(ptr);
  auto downstream = dconn->get_downstream();
  auto upstream = static_cast<Http2Upstream*>(downstream->get_upstream());
  if(events & BEV_EVENT_CONNECTED) {
    if(LOG_ENABLED(INFO)) {
      DCLOG(INFO, dconn) << "Connection established. stream_id="
                         << downstream->get_stream_id();
    }
    int fd = bufferevent_getfd(bev);
    int val = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                  reinterpret_cast<char *>(&val), sizeof(val)) == -1) {
      DCLOG(WARNING, dconn) << "Setting option TCP_NODELAY failed: errno="
                            << errno;
    }
  } else if(events & BEV_EVENT_EOF) {
    if(LOG_ENABLED(INFO)) {
      DCLOG(INFO, dconn) << "EOF. stream_id=" << downstream->get_stream_id();
    }
    if(downstream->get_request_state() == Downstream::STREAM_CLOSED) {
      // If stream was closed already, we don't need to send reply at
      // the first place. We can delete downstream.
      upstream->remove_downstream(downstream);
      delete downstream;
    } else {
      // Delete downstream connection. If we don't delete it here, it
      // will be pooled in on_stream_close_callback.
      downstream->set_downstream_connection(0);
      delete dconn;
      dconn = 0;
      // downstream wil be deleted in on_stream_close_callback.
      if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
        // Server may indicate the end of the request by EOF
        if(LOG_ENABLED(INFO)) {
          ULOG(INFO, upstream) << "Downstream body was ended by EOF";
        }
        downstream->set_response_state(Downstream::MSG_COMPLETE);

        // For tunneled connection, MSG_COMPLETE signals
        // downstream_data_read_callback to send RST_STREAM after
        // pending response body is sent. This is needed to ensure
        // that RST_STREAM is sent after all pending data are sent.
        upstream->on_downstream_body_complete(downstream);
      } else if(downstream->get_response_state() != Downstream::MSG_COMPLETE) {
        // If stream was not closed, then we set MSG_COMPLETE and let
        // on_stream_close_callback delete downstream.
        if(upstream->error_reply(downstream, 502) != 0) {
          delete upstream->get_client_handler();
          return;
        }
        downstream->set_response_state(Downstream::MSG_COMPLETE);
      }
      if(upstream->send() != 0) {
        delete upstream->get_client_handler();
        return;
      }
      // At this point, downstream may be deleted.
    }
  } else if(events & (BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
    if(LOG_ENABLED(INFO)) {
      if(events & BEV_EVENT_ERROR) {
        DCLOG(INFO, dconn) << "Downstream network error: "
                           << evutil_socket_error_to_string
          (EVUTIL_SOCKET_ERROR());
      } else {
        DCLOG(INFO, dconn) << "Timeout";
      }
      if(downstream->get_upgraded()) {
        DCLOG(INFO, dconn) << "Note: this is tunnel connection";
      }
    }
    if(downstream->get_request_state() == Downstream::STREAM_CLOSED) {
      upstream->remove_downstream(downstream);
      delete downstream;
    } else {
      // Delete downstream connection. If we don't delete it here, it
      // will be pooled in on_stream_close_callback.
      downstream->set_downstream_connection(0);
      delete dconn;
      dconn = 0;
      if(downstream->get_response_state() == Downstream::MSG_COMPLETE) {
        // For SSL tunneling, we issue RST_STREAM. For other types of
        // stream, we don't have to do anything since response was
        // complete.
        if(downstream->get_upgraded()) {
          upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
        }
      } else {
        if(downstream->get_response_state() == Downstream::HEADER_COMPLETE) {
          upstream->rst_stream(downstream, NGHTTP2_INTERNAL_ERROR);
        } else {
          unsigned int status;
          if(events & BEV_EVENT_TIMEOUT) {
            status = 504;
          } else {
            status = 502;
          }
          if(upstream->error_reply(downstream, status) != 0) {
            delete upstream->get_client_handler();
            return;
          }
        }
        downstream->set_response_state(Downstream::MSG_COMPLETE);
      }
      if(upstream->send() != 0) {
        delete upstream->get_client_handler();
        return;
      }
      // At this point, downstream may be deleted.
    }
  }
}
} // namespace

int Http2Upstream::rst_stream(Downstream *downstream,
                              nghttp2_error_code error_code)
{
  if(LOG_ENABLED(INFO)) {
    ULOG(INFO, this) << "RST_STREAM stream_id="
                     << downstream->get_stream_id()
                     << " with error_code="
                     << error_code;
  }
  int rv;
  rv = nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE,
                                 downstream->get_stream_id(), error_code);
  if(rv < NGHTTP2_ERR_FATAL) {
    ULOG(FATAL, this) << "nghttp2_submit_rst_stream() failed: "
                      << nghttp2_strerror(rv);
    DIE();
  }
  return 0;
}

int Http2Upstream::window_update(Downstream *downstream,
                                 int32_t window_size_increment)
{
  int rv;
  rv = nghttp2_submit_window_update(session_, NGHTTP2_FLAG_NONE,
                                    downstream ?
                                    downstream->get_stream_id() : 0,
                                    window_size_increment);
  if(rv < NGHTTP2_ERR_FATAL) {
    ULOG(FATAL, this) << "nghttp2_submit_window_update() failed: "
                      << nghttp2_strerror(rv);
    DIE();
  }
  return 0;
}

int Http2Upstream::terminate_session(nghttp2_error_code error_code)
{
  int rv;
  rv = nghttp2_session_terminate_session(session_, error_code);
  if(rv != 0) {
    return -1;
  }
  return 0;
}

namespace {
ssize_t downstream_data_read_callback(nghttp2_session *session,
                                      int32_t stream_id,
                                      uint8_t *buf, size_t length,
                                      int *eof,
                                      nghttp2_data_source *source,
                                      void *user_data)
{
  auto downstream = static_cast<Downstream*>(source->ptr);
  auto upstream = static_cast<Http2Upstream*>(downstream->get_upstream());
  auto handler = upstream->get_client_handler();
  auto body = downstream->get_response_body_buf();
  assert(body);
  int nread = evbuffer_remove(body, buf, length);
  if(nread == -1) {
    ULOG(FATAL, upstream) << "evbuffer_remove() failed";
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  if(nread == 0 &&
     downstream->get_response_state() == Downstream::MSG_COMPLETE) {
    if(!downstream->get_upgraded()) {
      *eof = 1;
    } else {
      // For tunneling, issue RST_STREAM to finish the stream.
      if(LOG_ENABLED(INFO)) {
        ULOG(INFO, upstream) << "RST_STREAM to tunneled stream stream_id="
                             << stream_id;
      }
      upstream->rst_stream(downstream, infer_upstream_rst_stream_error_code
                           (downstream->get_response_rst_stream_error_code()));
    }
  }
  // Send WINDOW_UPDATE before buffer is empty to avoid delay because
  // of RTT.
  if(*eof != 1 &&
     handler->get_outbuf_length() + evbuffer_get_length(body) <
     OUTBUF_MAX_THRES) {
    if(downstream->resume_read(SHRPX_NO_BUFFER) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
  }
  if(nread == 0 && *eof != 1) {
    return NGHTTP2_ERR_DEFERRED;
  }
  return nread;
}
} // namespace

int Http2Upstream::error_reply(Downstream *downstream,
                               unsigned int status_code)
{
  int rv;
  auto html = http::create_error_html(status_code);
  downstream->init_response_body_buf();
  auto body = downstream->get_response_body_buf();
  rv = evbuffer_add(body, html.c_str(), html.size());
  if(rv == -1) {
    ULOG(FATAL, this) << "evbuffer_add() failed";
    return -1;
  }
  downstream->set_response_state(Downstream::MSG_COMPLETE);

  nghttp2_data_provider data_prd;
  data_prd.source.ptr = downstream;
  data_prd.read_callback = downstream_data_read_callback;

  auto content_length = util::utos(html.size());
  auto status_code_str = util::utos(status_code);
  auto nva = std::vector<nghttp2_nv>{
    http2::make_nv_ls(":status", status_code_str),
    http2::make_nv_ll("content-type", "text/html; charset=UTF-8"),
    http2::make_nv_lc("server", get_config()->server_name),
    http2::make_nv_ls("content-length", content_length)
  };

  rv = nghttp2_submit_response(session_, downstream->get_stream_id(),
                               nva.data(), nva.size(), &data_prd);
  if(rv < NGHTTP2_ERR_FATAL) {
    ULOG(FATAL, this) << "nghttp2_submit_response() failed: "
                      << nghttp2_strerror(rv);
    DIE();
  }
  if(get_config()->accesslog) {
    upstream_response(get_client_handler()->get_ipaddr(),
                      status_code, downstream);
  }
  return 0;
}

bufferevent_data_cb Http2Upstream::get_downstream_readcb()
{
  return downstream_readcb;
}

bufferevent_data_cb Http2Upstream::get_downstream_writecb()
{
  return downstream_writecb;
}

bufferevent_event_cb Http2Upstream::get_downstream_eventcb()
{
  return downstream_eventcb;
}

void Http2Upstream::add_downstream(Downstream *downstream)
{
  downstream_queue_.add(downstream);
}

void Http2Upstream::remove_downstream(Downstream *downstream)
{
  downstream_queue_.remove(downstream);
}

Downstream* Http2Upstream::find_downstream(int32_t stream_id)
{
  return downstream_queue_.find(stream_id);
}

nghttp2_session* Http2Upstream::get_http2_session()
{
  return session_;
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
int Http2Upstream::on_downstream_header_complete(Downstream *downstream)
{
  if(LOG_ENABLED(INFO)) {
    DLOG(INFO, downstream) << "HTTP response header completed";
  }
  downstream->normalize_response_headers();
  if(!get_config()->http2_proxy && !get_config()->client_proxy) {
    downstream->rewrite_norm_location_response_header
      (get_client_handler()->get_upstream_scheme(), get_config()->port);
  }
  downstream->concat_norm_response_headers();
  auto end_headers = std::end(downstream->get_response_headers());
  size_t nheader = downstream->get_response_headers().size();
  auto nva = std::vector<nghttp2_nv>();
  // 2 means :status and possible via header field.
  nva.reserve(nheader + 2);
  std::string via_value;
  auto response_status = util::utos(downstream->get_response_http_status());
  nva.push_back(http2::make_nv_ls(":status", response_status));

  http2::copy_norm_headers_to_nva(nva, downstream->get_response_headers());
  auto via = downstream->get_norm_response_header("via");
  if(get_config()->no_via) {
    if(via != end_headers) {
      nva.push_back(http2::make_nv_ls("via", (*via).second));
    }
  } else {
    if(via != end_headers) {
      via_value = (*via).second;
      via_value += ", ";
    }
    via_value += http::create_via_header_value
      (downstream->get_response_major(), downstream->get_response_minor());
    nva.push_back(http2::make_nv_ls("via", via_value));
  }
  if(LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for(auto& nv : nva) {
      ss << TTY_HTTP_HD;
      ss.write(reinterpret_cast<const char*>(nv.name), nv.namelen);
      ss << TTY_RST << ": ";
      ss.write(reinterpret_cast<const char*>(nv.value), nv.valuelen);
      ss << "\n";
    }
    ULOG(INFO, this) << "HTTP response headers. stream_id="
                     << downstream->get_stream_id() << "\n"
                     << ss.str();
  }

  if(get_config()->http2_upstream_dump_response_header) {
    http2::dump_nv(get_config()->http2_upstream_dump_response_header,
                   nva.data(), nva.size());
  }

  nghttp2_data_provider data_prd;
  data_prd.source.ptr = downstream;
  data_prd.read_callback = downstream_data_read_callback;

  int rv;
  rv = nghttp2_submit_response(session_, downstream->get_stream_id(),
                               nva.data(), nva.size(), &data_prd);
  if(rv != 0) {
    ULOG(FATAL, this) << "nghttp2_submit_response() failed";
    return -1;
  }
  if(get_config()->accesslog) {
    upstream_response(get_client_handler()->get_ipaddr(),
                      downstream->get_response_http_status(),
                      downstream);
  }
  return 0;
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
int Http2Upstream::on_downstream_body(Downstream *downstream,
                                      const uint8_t *data, size_t len)
{
  auto upstream = downstream->get_upstream();
  auto handler = upstream->get_client_handler();
  auto body = downstream->get_response_body_buf();
  int rv = evbuffer_add(body, data, len);
  if(rv != 0) {
    ULOG(FATAL, this) << "evbuffer_add() failed";
    return -1;
  }
  nghttp2_session_resume_data(session_, downstream->get_stream_id());

  auto outbuflen = handler->get_outbuf_length() +
    evbuffer_get_length(body);
  if(outbuflen > OUTBUF_MAX_THRES) {
    downstream->pause_read(SHRPX_NO_BUFFER);
  }

  return 0;
}

// WARNING: Never call directly or indirectly nghttp2_session_send or
// nghttp2_session_recv. These calls may delete downstream.
int Http2Upstream::on_downstream_body_complete(Downstream *downstream)
{
  if(LOG_ENABLED(INFO)) {
    DLOG(INFO, downstream) << "HTTP response completed";
  }
  nghttp2_session_resume_data(session_, downstream->get_stream_id());
  return 0;
}

bool Http2Upstream::get_flow_control() const
{
  return flow_control_;
}

void Http2Upstream::pause_read(IOCtrlReason reason)
{}

int Http2Upstream::resume_read(IOCtrlReason reason, Downstream *downstream)
{
  if(get_flow_control()) {
    int32_t window_size_increment;
    window_size_increment = http2::determine_window_update_transmission
      (session_, 0);
    if(window_size_increment != -1) {
      window_update(nullptr, window_size_increment);
    }
    window_size_increment = http2::determine_window_update_transmission
      (session_, downstream->get_stream_id());
    if(window_size_increment != -1) {
      window_update(downstream, window_size_increment);
    }
  }
  return send();
}

} // namespace shrpx
