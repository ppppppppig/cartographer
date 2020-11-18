#include "cartographer/cloud/_server_interface.h"

#include "cartographer/cloud/internal/_server.h"
#include "cartographer/common/make_unique.h"

namespace cartographer {
namespace cloud {

void RegisterMapBuilderServerMetrics(metrics::FamilyFactory* factory) {
  MapBuilderServer::RegisterMetrics(factory);
}

std::unique_ptr<MapBuilderServerInterface> CreateMapBuilderServer(
    const proto::MapBuilderServerOptions& _server_options,
    std::unique_ptr<mapping::MapBuilderInterface> ) {
  return common::make_unique<MapBuilderServer>(_server_options,
                                               std::move());
}

}  // namespace cloud
}  // namespace cartographer
