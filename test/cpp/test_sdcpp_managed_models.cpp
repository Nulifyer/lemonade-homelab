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
        fs::remove_all(root);
        std::cout << "Managed diffusion checkpoint tests passed\n";
    } catch (...) {
        fs::remove_all(root);
        throw;
    }
}
