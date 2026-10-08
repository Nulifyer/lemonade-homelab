#include "lemon/backends/sdcpp/sdcpp_server.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using lemon::ModelInfo;
using lemon::backends::SDServer;
namespace fs = std::filesystem;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    const auto root = fs::temp_directory_path() / ("lemon-sdcpp-model-args-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    try {
        ModelInfo info;
        for (const auto* role : {"main", "text_encoder", "t5xxl", "vae", "clip_l", "clip_g"}) {
            const auto path = root / role;
            std::ofstream(path).put('x');
            info.resolved_paths[role] = path.string();
        }
        info.checkpoints = {{"main", "repo:model"}};
        check(SDServer::build_model_args(info) == std::vector<std::string>{"-m", info.resolved_path()},
              "Bundled SDXL model must retain -m");
        info.checkpoints["t5xxl"] = "repo:t5";
        bool failed = false;
        try { SDServer::build_model_args(info); } catch (...) { failed = true; }
        check(failed, "Split model without VAE accepted");
        info.checkpoints["vae"] = "repo:vae";
        check(SDServer::build_model_args(info) == std::vector<std::string>{
                  "--diffusion-model", info.resolved_path(), "--t5xxl", info.resolved_path("t5xxl"),
                  "--vae", info.resolved_path("vae")}, "Chroma must use T5, not LLM flags");
        info.checkpoints.erase("t5xxl");
        info.checkpoints["text_encoder"] = "repo:qwen";
        const auto zargs = SDServer::build_model_args(info);
        check(zargs[2] == "--llm", "Existing Qwen text encoders changed");
        info.checkpoints["clip_l"] = "repo:clip";
        fs::remove(root / "clip_l");
        failed = false;
        try { SDServer::build_model_args(info); } catch (...) { failed = true; }
        check(failed, "Incomplete managed auxiliary checkpoint accepted");
        lemon::RecipeOptions options("sd-cpp", {{"steps", 8}, {"cfg_scale", 1.0},
                                      {"sampling_method", "ipndm"}, {"scheduler", "beta"}});
        const auto defaults = SDServer::build_generation_params({}, options);
        check(defaults["sample_params"]["sample_steps"] == 8 &&
                  defaults["sample_params"]["guidance"]["txt_cfg"] == 1.0 &&
                  defaults["sample_params"]["sample_method"] == "ipndm" &&
                  defaults["sample_params"]["scheduler"] == "beta" &&
                  defaults["seed"].is_number_integer() && defaults["seed"].get<int>() >= 0,
              "Saved image sampling defaults or automatic seed missing");
        const auto explicit_params = SDServer::build_generation_params(
            {{"steps", 6}, {"cfg_scale", 0.0}, {"sample_method", "euler"},
             {"scheduler", "discrete"}, {"seed", 42}}, options);
        check(explicit_params["sample_params"]["sample_steps"] == 6 &&
                  explicit_params["sample_params"]["guidance"]["txt_cfg"] == 0.0 &&
                  explicit_params["sample_params"]["sample_method"] == "euler" &&
                  explicit_params["sample_params"]["scheduler"] == "discrete" &&
                  explicit_params["seed"] == 42,
              "Client image sampling values lost precedence");
        const auto high_params = SDServer::build_generation_params(
            {{"steps", 16}, {"cfg_scale", 7.0}, {"sample_method", "heun"},
             {"scheduler", "karras"}, {"seed", 4294967296LL}, {"clip_skip", 2},
             {"flow_shift", 3.0}}, options);
        check(high_params["sample_params"]["sample_steps"] == 16 &&
                  high_params["sample_params"]["guidance"]["txt_cfg"] == 7.0 &&
                  high_params["sample_params"]["sample_method"] == "heun" &&
                  high_params["sample_params"]["scheduler"] == "karras" &&
                  high_params["sample_params"]["flow_shift"] == 3.0 &&
                  high_params["seed"] == 4294967296LL && high_params["clip_skip"] == 2,
              "Explicit client controls were truncated or ignored by the runtime adapter");
        fs::remove_all(root);
        std::cout << "Managed diffusion checkpoint tests passed\n";
    } catch (...) {
        fs::remove_all(root);
        throw;
    }
}
