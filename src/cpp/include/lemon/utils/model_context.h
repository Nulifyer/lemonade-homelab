#pragma once

#include "lemon/utils/custom_args.h"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace lemon::utils {

// Conservative pre-load estimate for an explicit llama.cpp slot count. The
// live backend's /props is authoritative, including unified or adjusted KV.
inline int64_t configured_request_context(int64_t total, const std::string& args) {
    if (total <= 0) return 0;
    int64_t slots = 1;
    const auto tokens = parse_custom_args(args);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto equal = tokens[i].find('=');
        const auto flag = tokens[i].substr(0, equal);
        if (flag != "--parallel" && flag != "-np") continue;
        std::string value;
        if (equal != std::string::npos) value = tokens[i].substr(equal + 1);
        else if (i + 1 < tokens.size()) value = tokens[++i];
        try {
            size_t used = 0;
            const auto count = std::stoll(value, &used);
            if (used == value.size() && count >= 0) slots = count;
        } catch (...) {
            // Invalid arguments fail at load; do not fail model discovery.
        }
    }
    return slots > 0 ? total / slots : total;
}

inline int64_t request_context_from_props(const nlohmann::json& props) {
    const auto settings = props.find("default_generation_settings");
    if (settings == props.end() || !settings->is_object()) return 0;
    const auto context = settings->find("n_ctx");
    if (context == settings->end() || !context->is_number_integer()) return 0;
    const auto value = context->get<int64_t>();
    return value > 0 ? value : 0;
}

}  // namespace lemon::utils
