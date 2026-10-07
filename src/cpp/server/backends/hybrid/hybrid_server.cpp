#include "lemon/backends/hybrid/hybrid_server.h"
#include "lemon/backends/hybrid/hybrid.h"
#include "lemon/backends/backend_utils.h"
#include <filesystem>
#include <stdexcept>

namespace lemon { namespace backends {
LlamaCppServer::RuntimeLaunch HybridServer::prepare_runtime(
    const std::string& backend, const RecipeOptions& options) {
    if (backend != "system") throw std::invalid_argument("Hybrid backend requires its bundled system launcher");
    const json budget = options.get_option("hybrid_copy_gib");
    if (!budget.is_number() || budget.get<double>() <= 0 || budget.get<double>() > 32)
        throw std::invalid_argument("hybrid_copy_gib must be greater than zero and at most 32");
    const std::string executable = BackendUtils::get_backend_binary_path(*hybrid::spec(), "npu");
    return {executable, DEVICE_GPU | DEVICE_NPU,
            {{"HYBRID_REQUIRE_NPU", "1"}, {"GGML_XDNA_MAX_COPY_GB", budget.dump()},
             {"GGML_XDNA_HOST_ONLY", "0"}, {"LLAMA_API_KEY", ""}}};
}
namespace hybrid {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return make_server<HybridServer>(ctx); }
const BackendSpec* spec() {
    static const BackendSpec s{"hybrid", "hybrid-server"};
    return &s;
}
const BackendOps* ops() { return llamacpp::ops(); }
}
}}
