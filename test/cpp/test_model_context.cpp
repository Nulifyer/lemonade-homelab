#include "lemon/config_file.h"
#include "lemon/model_manager.h"
#include "lemon/ollama_api.h"
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/model_context.h"
#include "lemon/utils/path_utils.h"
#include "lemon/wrapped_server.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace lemon {
class ContextTestServer : public WrappedServer {
public:
    ContextTestServer(const std::string& name, const RecipeOptions& options)
        : WrappedServer("stub", "error", nullptr, nullptr) {
        set_model_metadata(name, "", ModelType::LLM, DEVICE_CPU, options);
        set_state(ModelState::READY);
        set_request_context_length(24576);
    }
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override {}
    bool is_backend_alive() const override { return true; }
    std::string get_backend_health_state() const override { return "ready"; }
};
struct RoutingHelperTestHook {
    static ContextTestServer* add(Router& router, const std::string& name,
                                  const RecipeOptions& options) {
        auto backend = std::make_unique<ContextTestServer>(name, options);
        auto* result = backend.get();
        router.loaded_servers_.push_back(std::move(backend));
        return result;
    }
};
struct OllamaApiTestAccess {
    static json show(OllamaApi& api, const std::string& model) {
        httplib::Request req;
        req.body = json{{"model", model}}.dump();
        httplib::Response res;
        res.status = 200;
        api.handle_show(req, res);
        if (res.status != 200) throw std::runtime_error("Ollama show failed: " + res.body);
        return json::parse(res.body);
    }
};
}  // namespace lemon

static void check(bool passed, const char* message) {
    if (!passed) throw std::runtime_error(message);
}

int main() {
    using namespace lemon;
    using utils::configured_request_context;
    check(configured_request_context(65536, "--parallel 2") == 32768, "Two slots over-advertised");
    check(configured_request_context(131072, "--parallel=2") == 65536, "Equals syntax ignored");
    check(configured_request_context(65536, "--parallel 4 -np 2") == 32768, "Last alias ignored");
    check(configured_request_context(65536, "-np=2 --parallel 1") == 65536, "Last long flag ignored");
    check(configured_request_context(65536, "--parallel 0") == 65536, "Auto unified context divided");
    check(configured_request_context(-1, "--parallel 2") == 0, "Auto context treated as concrete");
    check(configured_request_context(65536, "--parallel invalid") == 65536, "Malformed discovery threw");
    check(configured_request_context(65536, "--chat-template-kwargs '{\"note\":\"--parallel 8\"}'") == 65536,
          "Quoted template content treated as a slot flag");
    check(utils::request_context_from_props({{"total_slots", 2}, {"default_generation_settings", {{"n_ctx", 32768}}}}) == 32768,
          "Live per-slot context divided twice");
    for (const auto& props : {json::object(), json{{"default_generation_settings", nullptr}},
                             json{{"default_generation_settings", {{"n_ctx", -1}}}},
                             json{{"default_generation_settings", {{"n_ctx", "65536"}}}}})
        check(utils::request_context_from_props(props) == 0, "Invalid backend capacity accepted");

    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("model_context_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    utils::set_cache_dir(root.string());
    utils::set_config_dir(root.string());
    utils::set_models_dir(root.string());
    {
        std::ofstream(root / "user_models.json") << json{{"Context-Test", {
            {"checkpoint", "org/test:model.gguf"}, {"recipe", "llamacpp"},
            {"recipe_options", {{"ctx_size", 65536}, {"llamacpp_args", "--parallel 2"}}}
        }}}.dump();
        ModelManager models;
        auto defaults = ConfigFile::base_defaults();
        defaults["offline"] = true;
        defaults["log_level"] = "error";
        defaults["max_loaded_models"] = 3;
        RuntimeConfig runtime(defaults);
        RuntimeConfig::set_global(&runtime);
        {
            Router router(&runtime, &models, nullptr);
            const auto info = models.get_model_info("Context-Test");
            check(router.resolve_model_context_length("Context-Test", info) == 32768,
                  "Unloaded configured context was not per request");
            auto* backend = RoutingHelperTestHook::add(router, models.resolve_model_name("Context-Test"),
                                                      info.recipe_options);
            check(router.resolve_model_context_length("Context-Test", info) == 24576,
                  "Backend adjusted capacity did not override configuration");
            check(router.get_model_recipe_options("Context-Test").get_option("ctx_size") == 65536,
                  "Metadata changed replayable load settings");
            OllamaApi ollama(&router, &models);
            const auto show = OllamaApiTestAccess::show(ollama, "Context-Test");
            check(show["parameters"] == "num_ctx 24576" &&
                      show["model_info"]["llamacpp.context_length"] == 24576,
                  "Ollama disagreed with native effective context");
            backend->set_request_context_length(65536);
            check(router.resolve_model_context_length("Context-Test", info) == 65536,
                  "Unified live capacity divided twice");
            backend->set_request_context_length(0);
            check(router.resolve_model_context_length("Context-Test", info) == 32768,
                  "Missing props did not use conservative configuration");
        }
        RuntimeConfig::set_global(nullptr);
    }
    fs::remove_all(root);
    std::cout << "Context parsing, live capacity, preserved options and Ollama agreement passed\n";
}
