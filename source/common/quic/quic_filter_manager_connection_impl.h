#pragma once

#include <functional>

#include "envoy/event/dispatcher.h"
#include "envoy/network/connection.h"

#include "common/common/empty_string.h"
#include "common/common/logger.h"
#include "common/http/http3/codec_stats.h"
#include "common/network/connection_impl_base.h"
#include "common/quic/envoy_quic_connection.h"
#include "common/quic/envoy_quic_simulated_watermark_buffer.h"
#include "common/quic/send_buffer_monitor.h"
#include "common/stream_info/stream_info_impl.h"

namespace Envoy {

class TestPauseFilterForQuic;

namespace Quic {

// Act as a Network::Connection to HCM and a FilterManager to FilterFactoryCb.
class QuicFilterManagerConnectionImpl : public Network::ConnectionImplBase,
                                        public SendBufferMonitor {
public:
  QuicFilterManagerConnectionImpl(EnvoyQuicConnection& connection, Event::Dispatcher& dispatcher,
                                  uint32_t send_buffer_limit);
  ~QuicFilterManagerConnectionImpl() override = default;

  // Network::FilterManager
  // Overridden to delegate calls to filter_manager_.
  void addWriteFilter(Network::WriteFilterSharedPtr filter) override;
  void addFilter(Network::FilterSharedPtr filter) override;
  void addReadFilter(Network::ReadFilterSharedPtr filter) override;
  void removeReadFilter(Network::ReadFilterSharedPtr filter) override;
  bool initializeReadFilters() override;

  // Network::Connection
  void addBytesSentCallback(Network::Connection::BytesSentCb /*cb*/) override {
    // TODO(danzh): implement to support proxy. This interface is only called from
    // TCP proxy code.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
  void enableHalfClose(bool enabled) override;
  bool isHalfCloseEnabled() override;
  void close(Network::ConnectionCloseType type) override;
  Event::Dispatcher& dispatcher() override { return dispatcher_; }
  std::string nextProtocol() const override { return EMPTY_STRING; }
  void noDelay(bool /*enable*/) override {
    // No-op. TCP_NODELAY doesn't apply to UDP.
  }
  // TODO(#14829) both readDisable and detectEarlyCloseWhenReadDisabled are used for upstream QUIC
  // and needs to be hooked up before it is production-safe.
  void readDisable(bool /*disable*/) override {}
  void detectEarlyCloseWhenReadDisabled(bool /*value*/) override {}
  bool readEnabled() const override { return true; }
  const Network::SocketAddressSetter& addressProvider() const override {
    return quic_connection_->connectionSocket()->addressProvider();
  }
  Network::SocketAddressProviderSharedPtr addressProviderSharedPtr() const override {
    return quic_connection_->connectionSocket()->addressProviderSharedPtr();
  }
  absl::optional<Network::Connection::UnixDomainSocketPeerCredentials>
  unixSocketPeerCredentials() const override {
    // Unix domain socket is not supported.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
  void setConnectionStats(const Network::Connection::ConnectionStats& stats) override {
    // TODO(danzh): populate stats.
    Network::ConnectionImplBase::setConnectionStats(stats);
    quic_connection_->setConnectionStats(stats);
  }
  Ssl::ConnectionInfoConstSharedPtr ssl() const override;
  Network::Connection::State state() const override {
    if (quic_connection_ != nullptr && quic_connection_->connected()) {
      return Network::Connection::State::Open;
    }
    return Network::Connection::State::Closed;
  }
  bool connecting() const override {
    if (quic_connection_ != nullptr && !quic_connection_->IsHandshakeComplete()) {
      return true;
    }
    return false;
  }
  void write(Buffer::Instance& /*data*/, bool /*end_stream*/) override {
    // All writes should be handled by Quic internally.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
  void setBufferLimits(uint32_t limit) override;
  uint32_t bufferLimit() const override {
    // As quic connection is not HTTP1.1, this method shouldn't be called by HCM.
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
  bool aboveHighWatermark() const override;

  const Network::ConnectionSocket::OptionsSharedPtr& socketOptions() const override;
  StreamInfo::StreamInfo& streamInfo() override { return stream_info_; }
  const StreamInfo::StreamInfo& streamInfo() const override { return stream_info_; }
  absl::string_view transportFailureReason() const override { return transport_failure_reason_; }
  bool startSecureTransport() override { return false; }
  absl::optional<std::chrono::milliseconds> lastRoundTripTime() const override { return {}; }

  // Network::FilterManagerConnection
  void rawWrite(Buffer::Instance& data, bool end_stream) override;

  // Network::ReadBufferSource
  Network::StreamBuffer getReadBuffer() override { return {empty_buffer_, false}; }
  // Network::WriteBufferSource
  Network::StreamBuffer getWriteBuffer() override { NOT_REACHED_GCOVR_EXCL_LINE; }

  // SendBufferMonitor
  // Update the book keeping of the aggregated buffered bytes cross all the
  // streams, and run watermark check.
  void updateBytesBuffered(size_t old_buffered_bytes, size_t new_buffered_bytes) override;

  // Called after each write when a previous connection close call is postponed.
  void maybeApplyDelayClosePolicy();

  uint32_t bytesToSend() { return bytes_to_send_; }

  void setHttp3Options(const envoy::config::core::v3::Http3ProtocolOptions& http3_options) {
    http3_options_ =
        std::reference_wrapper<const envoy::config::core::v3::Http3ProtocolOptions>(http3_options);
  }

  void setCodecStats(Http::Http3::CodecStats& stats) {
    codec_stats_ = std::reference_wrapper<Http::Http3::CodecStats>(stats);
  }

protected:
  // Propagate connection close to network_connection_callbacks_.
  void onConnectionCloseEvent(const quic::QuicConnectionCloseFrame& frame,
                              quic::ConnectionCloseSource source);

  void closeConnectionImmediately() override;

  virtual bool hasDataToWrite() PURE;

  EnvoyQuicConnection* quic_connection_{nullptr};

  absl::optional<std::reference_wrapper<Http::Http3::CodecStats>> codec_stats_;
  absl::optional<std::reference_wrapper<const envoy::config::core::v3::Http3ProtocolOptions>>
      http3_options_;

private:
  friend class Envoy::TestPauseFilterForQuic;

  // Called when aggregated buffered bytes across all the streams exceeds high watermark.
  void onSendBufferHighWatermark();
  // Called when aggregated buffered bytes across all the streams declines to low watermark.
  void onSendBufferLowWatermark();

  // Currently ConnectionManagerImpl is the one and only filter. If more network
  // filters are added, ConnectionManagerImpl should always be the last one.
  // Its onRead() is only called once to trigger ReadFilter::onNewConnection()
  // and the rest incoming data bypasses these filters.
  Network::FilterManagerImpl filter_manager_;

  StreamInfo::StreamInfoImpl stream_info_;
  std::string transport_failure_reason_;
  uint32_t bytes_to_send_{0};
  // Keeps the buffer state of the connection, and react upon the changes of how many bytes are
  // buffered cross all streams' send buffer. The state is evaluated and may be changed upon each
  // stream write. QUICHE doesn't buffer data in connection, all the data is buffered in stream's
  // send buffer.
  EnvoyQuicSimulatedWatermarkBuffer write_buffer_watermark_simulation_;
  Buffer::OwnedImpl empty_buffer_;
};

} // namespace Quic
} // namespace Envoy
