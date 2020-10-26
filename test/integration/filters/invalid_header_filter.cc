#include <string>

#include "envoy/http/filter.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "common/http/header_utility.h"

#include "extensions/filters/http/common/pass_through_filter.h"

#include "test/extensions/filters/http/common/empty_http_filter_config.h"
#include "test/integration/filters/common.h"

namespace Envoy {

// Faulty filter that may remove critical headers.
class InvalidHeaderFilter : public Http::PassThroughFilter {
public:
  constexpr static char name[] = "invalid-header-filter";

  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers, bool) override {
    // Remove method when there is a "remove-method" header.
    if (!headers.get(Http::LowerCaseString("remove-method")).empty()) {
      headers.removeMethod();
    }
    if (!headers.get(Http::LowerCaseString("remove-path")).empty()) {
      headers.removePath();
    }
    if (Http::HeaderUtility::isConnect(headers)) {
      ENVOY_LOG_MISC(info, "REMOVING Host FROM CONNECT");
      headers.removeHost();
    }
    return Http::FilterHeadersStatus::Continue;
  }
};

constexpr char InvalidHeaderFilter::name[];
static Registry::RegisterFactory<SimpleFilterConfig<InvalidHeaderFilter>,
                                 Server::Configuration::NamedHttpFilterConfigFactory>
    decoder_register_;

} // namespace Envoy
