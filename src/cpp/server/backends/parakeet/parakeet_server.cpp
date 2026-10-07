#include "lemon/backends/parakeet/parakeet_server.h"
#include "lemon/backends/parakeet/parakeet.h"
#include "lemon/model_manager.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"
#include <filesystem>
#include <stdexcept>

namespace lemon { namespace backends {
ParakeetServer::ParakeetServer(const std::string& log_level, ModelManager* mm, BackendManager* bm)
    : WrappedServer("parakeet-server", log_level, mm, bm) {}
ParakeetServer::~ParakeetServer() { unload(); }
void ParakeetServer::load(const std::string& model_name, const ModelInfo& info,
                         const RecipeOptions& options, bool do_not_upgrade) {
    (void)model_name; (void)do_not_upgrade;
    const std::string model = info.resolved_path();
    if (model.empty() || !std::filesystem::is_regular_file(model))
        throw std::invalid_argument("Parakeet model file is missing");
    const json threads = options.get_option("parakeet_threads");
    if (!threads.is_number_integer() || threads.get<int>() < 1 || threads.get<int>() > 12)
        throw std::invalid_argument("parakeet_threads must be between 1 and 12");
    device_type_ = DEVICE_CPU;
    port_ = choose_port();
    const std::string exe = BackendUtils::get_backend_binary_path(*parakeet::spec(), "cpu");
    std::vector<std::string> args = {"--model", model, "--host", "127.0.0.1", "--port", std::to_string(port_),
                                     "--threads", threads.dump(), "--concurrency", "1"};
    set_process_handle(utils::ProcessManager::start_process(exe, args, "", true, true), exe, args);
    if (!wait_for_ready("/health")) {
        unload();
        throw std::runtime_error("Parakeet failed its startup readiness check");
    }
}
void ParakeetServer::unload() {
    stop_backend_watchdog();
    const auto h = consume_process_handle_for_cleanup();
    if (has_process_handle(h)) utils::ProcessManager::stop_process(h);
}
json ParakeetServer::audio_transcriptions(const json& request) {
    if (!request.contains("file_data") || !request["file_data"].is_string())
        return {{"error", {{"message", "Missing audio file"}, {"status_code", 400}}}};
    const std::string audio = request["file_data"].get<std::string>();
    if (audio.size() < 12 || audio.size() > 16 * 1024 * 1024 || audio.substr(0,4) != "RIFF" || audio.substr(8,4) != "WAVE")
        return {{"error", {{"message", "Expected a bounded WAV upload"}, {"status_code", 400}}}};
    std::vector<utils::MultipartField> fields;
    utils::MultipartField file;
    file.name = "file"; file.data = audio; file.filename = "audio.wav"; file.content_type = "audio/wav";
    fields.push_back(std::move(file));
    utils::MultipartField format;
    format.name = "response_format"; format.data = "json"; fields.push_back(std::move(format));
    const auto response = utils::HttpClient::post_multipart("http://127.0.0.1:" + std::to_string(port_) + "/v1/audio/transcriptions",
                            fields, 60, utils::HttpSecurityPolicy::TrustedLoopback,
                            current_request_cancel_context());
    if (response.status_code != 200)
        return {{"error", {{"message", "Parakeet transcription failed"}, {"status_code", response.status_code == 400 ? 400 : 502}}}};
    json result = json::parse(response.body);
    if (!result.contains("text") || !result["text"].is_string()) throw std::runtime_error("Parakeet returned an invalid transcript");
    return result;
}
namespace parakeet {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return make_server<ParakeetServer>(ctx); }
const BackendSpec* spec() { static const BackendSpec s{"parakeet", "parakeet-server"}; return &s; }
const BackendOps* ops() { return default_backend_ops(); }
}
}}
