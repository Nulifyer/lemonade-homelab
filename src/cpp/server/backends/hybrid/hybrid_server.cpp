#include "lemon/backends/hybrid/hybrid_server.h"
#include "lemon/backends/hybrid/hybrid.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include <algorithm>
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
namespace {
class HybridOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const override {
        llamacpp::ops()->populate_metadata(info, ctx);
        // This native integration accepts text and disables speculative decode;
        // checkpoint capabilities do not establish supported hybrid features.
        info.labels.erase(std::remove_if(info.labels.begin(), info.labels.end(),
            [](const std::string& label) { return label == "mtp" || label == "vision"; }),
            info.labels.end());
    }

    void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const override {
        llamacpp::ops()->resolve_runtime_options(info, options);
    }

    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        return llamacpp::ops()->resolve_checkpoint_path(info, ctx);
    }

    std::string find_imported_checkpoint(const std::string& dir) const override {
        return llamacpp::ops()->find_imported_checkpoint(dir);
    }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        return llamacpp::ops()->validate_registration_checkpoint(checkpoint);
    }

    std::string validate_checkpoint_file(const std::string& path) const override {
        return llamacpp::ops()->validate_checkpoint_file(path);
    }

    std::string resolve_version(const std::string& backend,
                                const std::string& file_version) const override {
        return llamacpp::ops()->resolve_version(backend, file_version);
    }

    InstallCheck check_install(const std::string& backend, bool binary_found) const override {
        return llamacpp::ops()->check_install(backend, binary_found);
    }
};
}
std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return make_server<HybridServer>(ctx); }
const BackendSpec* spec() {
    static const BackendSpec s{"hybrid", "hybrid-server"};
    return &s;
}
const BackendOps* ops() { return single_ops<HybridOps>(); }
}
}}
