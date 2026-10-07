#pragma once
#include "lemon/backends/llamacpp/llamacpp_server.h"

namespace lemon { namespace backends {
class HybridServer : public LlamaCppServer {
public:
    using LlamaCppServer::LlamaCppServer;
protected:
    RuntimeLaunch prepare_runtime(const std::string& backend, const RecipeOptions& options) override;
};
namespace hybrid {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<HybridServer>(); }
}
}}
