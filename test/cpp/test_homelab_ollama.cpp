#include "lemon/ollama_api.h"
#include <iostream>
#include <stdexcept>

namespace lemon {
struct OllamaApiTestAccess {
    static json convert(OllamaApi& api, const json& request) {
        return api.convert_ollama_to_openai_chat(request);
    }
};
}

int main() {
    using namespace lemon;
    OllamaApi api(nullptr, nullptr, [](const std::string& name) {
        return name == "small-task" ? "user.HA-Qwen35-2B" : name;
    });
    const json out = OllamaApiTestAccess::convert(api, {
        {"model", "small-task:latest"}, {"messages", json::array()}, {"think", false},
        {"top_k", 7}, {"options", {{"temperature", 0.25}, {"top_p", 0.9}, {"top_k", 20},
            {"min_p", 0.05}, {"repeat_penalty", 1.1}, {"frequency_penalty", 0.2}, {"presence_penalty", 0.3}}}
    });
    if (out.at("model") != "user.HA-Qwen35-2B" || out.at("top_k") != 7 ||
        out.at("min_p") != 0.05 || out.at("repeat_penalty") != 1.1 ||
        out.at("frequency_penalty") != 0.2 || out.at("presence_penalty") != 0.3 ||
        out.at("temperature") != 0.25 || out.at("enable_thinking") != false)
        throw std::runtime_error("Ollama aliases or sampling settings were lost");
    OllamaApi plain(nullptr, nullptr);
    if (OllamaApiTestAccess::convert(plain, {{"model", "plain:latest"}, {"messages", json::array()}}).at("model") != "plain")
        throw std::runtime_error("Default normalization changed");
    std::cout << "Ollama alias, sampling and thinking conversion passed\n";
}
