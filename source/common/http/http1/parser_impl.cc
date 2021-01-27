#include "common/http/http1/parser_impl.h"

#include <llhttp.h>

#include <iostream>

#include "common/common/assert.h"
#include "common/http/http1/parser.h"

namespace Envoy {
namespace Http {
namespace Http1 {

class HttpParserImpl::Impl {
public:
  Impl(llhttp_type_t type) {
    llhttp_init(&parser_, type, &settings_);
    llhttp_set_lenient_chunked_length(&parser_, 1);
    llhttp_set_lenient_headers(&parser_, 1);
  }

  Impl(llhttp_type_t type, void* data) : Impl(type) {
    parser_.data = data;
    settings_ = {
        [](llhttp_t* parser) -> int {
          return static_cast<ParserCallbacks*>(parser->data)->onMessageBegin();
        },
        [](llhttp_t* parser, const char* at, size_t length) -> int {
          return static_cast<ParserCallbacks*>(parser->data)->onUrl(at, length);
        },
        // TODO(dereka) onStatus
        nullptr,
        [](llhttp_t* parser, const char* at, size_t length) -> int {
          return static_cast<ParserCallbacks*>(parser->data)->onHeaderField(at, length);
        },
        [](llhttp_t* parser, const char* at, size_t length) -> int {
          return static_cast<ParserCallbacks*>(parser->data)->onHeaderValue(at, length);
        },
        [](llhttp_t* parser) -> int {
          return static_cast<ParserCallbacks*>(parser->data)->onHeadersComplete();
        },
        [](llhttp_t* parser, const char* at, size_t length) -> int {
          return static_cast<ParserCallbacks*>(parser->data)->bufferBody(at, length);
        },
        [](llhttp_t* parser) -> int {
          return static_cast<ParserCallbacks*>(parser->data)->onMessageComplete();
        },
        [](llhttp_t* parser) -> int {
          // A 0-byte chunk header is used to signal the end of the chunked body.
          // When this function is called, http-parser holds the size of the chunk in
          // parser->content_length. See
          // https://github.com/nodejs/http-parser/blob/v2.9.3/http_parser.h#L336
          const bool is_final_chunk = (parser->content_length == 0);
          return static_cast<ParserCallbacks*>(parser->data)->onChunkHeader(is_final_chunk);
        },
        nullptr, // on_chunk_complete
        nullptr, // on_url_complete
        nullptr, // on_status_complete
        nullptr, // on_header_field_complete
        nullptr  // on_header_value_complete
    };
  }

  rcVal execute(const char* slice, int len) {
    llhttp_errno_t err;
    if (slice == nullptr || len == 0) {
      err = llhttp_finish(&parser_);
    } else {
      err = llhttp_execute(&parser_, slice, len);
    }
    size_t nread = len;
    // Adjust nread in case of error.
    if (err != HPE_OK) {
      nread = llhttp_get_error_pos(&parser_) - slice;
      // In case of HPE_PAUSED_UPGRADE, resume.
      if (err == HPE_PAUSED_UPGRADE) {
        err = HPE_OK;
        llhttp_resume_after_upgrade(&parser_);
      }
    }
    return {nread, err};
  }

  void resume() { llhttp_resume(&parser_); }

  int pause() {
    // llhttp pauses by returning HPE_PAUSED. llhttp_pause cannot be called through user callbacks.
    return HPE_PAUSED;
  }

  int getErrno() {
    return llhttp_get_errno(&parser_);
  }

  int statusCode() const { return parser_.status_code; }

  int httpMajor() const { return parser_.http_major; }

  int httpMinor() const { return parser_.http_minor; }

  uint64_t contentLength() const { return parser_.content_length; }

  int flags() const { return parser_.flags; }

  uint16_t method() const { return parser_.method; }

  const char* methodName() const {
    return llhttp_method_name(static_cast<llhttp_method>(parser_.method));
  }

  int usesTransferEncoding() const { return parser_.flags & F_TRANSFER_ENCODING; }

  bool seenContentLength() const { return seen_content_length_; }
  void setSeenContentLength(bool val) { seen_content_length_ = val; }

private:
  llhttp_t parser_;
  llhttp_settings_s settings_;
  bool seen_content_length_;
};

HttpParserImpl::HttpParserImpl(MessageType type, ParserCallbacks* data) {
  llhttp_type_t parser_type;
  switch (type) {
  case MessageType::Request:
    parser_type = HTTP_REQUEST;
    break;
  case MessageType::Response:
    parser_type = HTTP_RESPONSE;
    break;
  default:
    NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
  }

  impl_ = std::make_unique<Impl>(parser_type, data);
}

// Because we have a pointer-to-impl using std::unique_ptr, we must place the destructor in the
// same compilation unit so that the destructor has a complete definition of Impl.
HttpParserImpl::~HttpParserImpl() = default;

HttpParserImpl::rcVal HttpParserImpl::execute(const char* slice, int len) {
  return impl_->execute(slice, len);
}

void HttpParserImpl::resume() { impl_->resume(); }

int HttpParserImpl::pause() { return impl_->pause(); }

int HttpParserImpl::getErrno() { return impl_->getErrno(); }

int HttpParserImpl::statusCode() const { return impl_->statusCode(); }

int HttpParserImpl::httpMajor() const { return impl_->httpMajor(); }

int HttpParserImpl::httpMinor() const { return impl_->httpMinor(); }

uint64_t HttpParserImpl::contentLength() const { return impl_->contentLength(); }

int HttpParserImpl::flags() const { return impl_->flags(); }

uint16_t HttpParserImpl::method() const { return impl_->method(); }

const char* HttpParserImpl::methodName() const { return impl_->methodName(); }

const char* HttpParserImpl::errnoName() {
  return llhttp_errno_name(static_cast<llhttp_errno_t>(impl_->getErrno()));
}

const char* HttpParserImpl::errnoName(int rc) const {
  return llhttp_errno_name(static_cast<llhttp_errno_t>(rc));
}

int HttpParserImpl::usesTransferEncoding() const { return impl_->usesTransferEncoding(); }

bool HttpParserImpl::seenContentLength() const { return impl_->seenContentLength(); }
void HttpParserImpl::setSeenContentLength(bool val) { impl_->setSeenContentLength(val); }

int HttpParserImpl::statusToInt(const ParserStatus code) const {
  switch (code) {
  case ParserStatus::Error:
    return -1;
  case ParserStatus::Success:
    return 0;
  case ParserStatus::NoBody:
    return 1;
  case ParserStatus::NoBodyData:
    return 2;
  case ParserStatus::Paused:
    return 21;
  }
}


} // namespace Http1
} // namespace Http
} // namespace Envoy
