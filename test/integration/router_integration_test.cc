#include "test/integration/http_integration.h"
#include "test/common/upstream/utility.h"
#include "common/api/api_impl.h"

#include "gtest/gtest.h"

namespace Envoy {

TEST(RegressionTest, TestSingleRequest) {
  Network::Address::IpVersion ip_version = Network::Address::IpVersion::v4;
  uint32_t port = 80; // lookupPort("http");
  auto addr = Network::Utility::resolveUrl(
      fmt::format("tcp://{}:{}", Network::Test::getLoopbackAddressUrlString(ip_version), port));
  std::string method = "GET";
  std::string url = "localhost";
  std::string body = "";
  Http::CodecClient::Type type{Http::CodecClient::Type::HTTP2};
  std::string host = "host";
  std::string content_type = "";

  NiceMock<Stats::MockIsolatedStatsStore> mock_stats_store;
  Event::GlobalTimeSystem time_system;
  Api::Impl api(Thread::threadFactoryForTest(), mock_stats_store, time_system,
                Filesystem::fileSystemForTest());
  Event::DispatcherPtr dispatcher(api.allocateDispatcher("test_thread"));
  std::shared_ptr<Upstream::MockClusterInfo> cluster{new NiceMock<Upstream::MockClusterInfo>()};
  Upstream::HostDescriptionConstSharedPtr host_description{
      Upstream::makeTestHostDescription(cluster, "tcp://127.0.0.1:80")};
  Http::CodecClientProd client(
      type,
      dispatcher->createClientConnection(addr, Network::Address::InstanceConstSharedPtr(),
                                         Network::Test::createRawBufferSocket(), nullptr),
      host_description, *dispatcher);
  BufferingStreamDecoderPtr response(new BufferingStreamDecoder([&]() -> void {
    client.close();
    dispatcher->exit();
  }));
  Http::RequestEncoder& encoder = client.newStream(*response);
  encoder.getStream().addCallbacks(*response);

  Http::TestRequestHeaderMapImpl headers;
  headers.setMethod(method);
  headers.setPath(url);
  headers.setHost(host);
  headers.setReferenceScheme(Http::Headers::get().SchemeValues.Http);
  if (!content_type.empty()) {
    headers.setContentType(content_type);
  }
  encoder.encodeHeaders(headers, body.empty());
  if (!body.empty()) {
    Buffer::OwnedImpl body_buffer(body);
    encoder.encodeData(body_buffer, true);
  }

  dispatcher->run(Event::Dispatcher::RunType::Block);
}

TEST(RegressionTest, Test) {
  // BaseIntegrationTest setup
  Stats::IsolatedStoreImpl stats_store;
  Api::ApiPtr api_(Api::createApiForTest(stats_store));
  MockBufferFactory* mock_buffer_factory_(new NiceMock<MockBufferFactory>);
  Event::DispatcherPtr dispatcher_(api_->allocateDispatcher("test", Buffer::WatermarkFactoryPtr{mock_buffer_factory_}));
  Network::Address::IpVersion version_ = Network::Address::IpVersion::v4;
  BaseIntegrationTest::InstanceConstSharedPtrFn upstream_address_fn_ = [version_](int) {
            return Network::Utility::parseInternetAddress(
                Network::Test::getAnyAddressString(version_), 0); };
  testing::NiceMock<Server::Configuration::MockTransportSocketFactoryContext> factory_context_;
  Event::GlobalTimeSystem time_system_;

  ON_CALL(*mock_buffer_factory_, create_(_, _, _))
      .WillByDefault(Invoke([](std::function<void()> below_low, std::function<void()> above_high,
                               std::function<void()> above_overflow) -> Buffer::Instance* {
                              return new Buffer::WatermarkBuffer(below_low, above_high, above_overflow);
      }));
  ON_CALL(factory_context_, api()).WillByDefault(testing::ReturnRef(*api_));

  // Base Integration Test members
  std::vector<std::unique_ptr<FakeUpstream>> fake_upstreams_;

  // Http Integration Test members
  IntegrationCodecClientPtr codec_client_;
  FakeHttpConnectionPtr fake_upstream_connection_;
  FakeStreamPtr upstream_request_;
  Http::RequestEncoder* request_encoder_{nullptr};
  Http::CodecClient::Type downstream_protocol_{Http::CodecClient::Type::HTTP2};
  FakeHttpConnection::Type upstream_protocol_{FakeHttpConnection::Type::HTTP2};

  // Initialize
  // Create Upstreams
  fake_upstreams_.emplace_back(new FakeUpstream(upstream_address_fn_(0), upstream_protocol_, *time_system_,
                                                false, false));
  // Initialize clusters


  // makehttpconnection
  uint32_t port = 80; // lookupPort("http");
  Network::ClientConnectionPtr conn(dispatcher_->createClientConnection(
      Network::Utility::resolveUrl(
          fmt::format("tcp://{}:{}", Network::Test::getLoopbackAddressUrlString(version_), port)),
      Network::Address::InstanceConstSharedPtr(), Network::Test::createRawBufferSocket(), nullptr));
  // bool enable_half_close_ = false;
  // conn->enableHalfClose(enable_half_close_);
  conn->connect();

  // Make fake upstream?

  ENVOY_LOG_MISC(info, "creating codec client");
  std::shared_ptr<Upstream::MockClusterInfo> cluster{new NiceMock<Upstream::MockClusterInfo>()};
  cluster->max_response_headers_count_ = 200;
  cluster->http1_settings_.enable_trailers_ = true;
  Upstream::HostDescriptionConstSharedPtr host_description{Upstream::makeTestHostDescription(
      cluster, fmt::format("tcp://{}:80", Network::Test::getLoopbackAddressUrlString(version_)))};

  ENVOY_LOG_MISC(info, "instantiating codec client");
  codec_client_ = std::make_unique<IntegrationCodecClient>(*dispatcher_, std::move(conn),
                                                           host_description, downstream_protocol_);


  ENVOY_LOG_MISC(info, "Running regression test");
  Http::TestRequestHeaderMapImpl request_headers{{":method", "POST"},
                                                 {":path", "/test/long/url"},
                                                 {":scheme", "http"},
                                                 {":authority", "host"},
                                                 {"x-forwarded-for", "10.0.0.1"},
                                                 {"x-envoy-upstream-rq-timeout-ms", "100"}};
  auto encoder_decoder = codec_client_->startRequest(request_headers);
  auto response = std::move(encoder_decoder.second);
  request_encoder_ = &encoder_decoder.first;

  codec_client_->sendData(*request_encoder_, 100, true);

  // ASSERT_TRUE(upstream_request_->waitForEndStream(*dispatcher_));

  // Respond with headers, not end of stream.
  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  upstream_request_->encodeHeaders(response_headers, false);

  response->waitForHeaders();
  EXPECT_EQ("200", response->headers().getStatusValue());

  // Trigger global timeout.
  time_system_.advanceTimeWait(std::chrono::milliseconds(200));
  codec_client_->close();


}


}  // namespace Envoy
