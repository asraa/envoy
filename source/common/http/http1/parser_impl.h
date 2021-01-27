#pragma once

#include <memory>

#include "common/http/http1/parser.h"

namespace Envoy {
namespace Http {
namespace Http1 {

class HttpParserImpl : public Parser {
public:
  HttpParserImpl(MessageType type, ParserCallbacks* data);
  ~HttpParserImpl();

  // Http1::Parser
  rcVal execute(const char* data, int len) override;
  void resume() override;
  int pause() override;
  int getErrno() override;
  int statusCode() const override;
  int httpMajor() const override;
  int httpMinor() const override;
  uint64_t contentLength() const override;
  int flags() const override;
  uint16_t method() const override;
  const char* methodName() const override;
  const char* errnoName() override;
  const char* errnoName(int rc) const override;
  int usesTransferEncoding() const override;
  bool seenContentLength() const override;
  void setSeenContentLength(bool val) override;
  int statusToInt(const ParserStatus code) const override;
  int flagsChunked() const override { return 1 << 3; }

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace Http1
} // namespace Http
} // namespace Envoy
