#pragma once
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"
namespace lemon { namespace backends {
class ParakeetServer : public WrappedServer, public ITranscriptionServer {
public:
    ParakeetServer(const std::string& log_level, ModelManager* mm, BackendManager* bm);
    ~ParakeetServer() override;
    void load(const std::string& model_name, const ModelInfo& info, const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;
    json audio_transcriptions(const json& request) override;
};
namespace parakeet {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<ParakeetServer>(); }
}
}}
