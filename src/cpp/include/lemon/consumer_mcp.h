#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace lemon {
// Bounded JSON-RPC dispatch. ConsumerService owns HTTP sessions and cancellation.
std::optional<nlohmann::json> consumer_mcp(
    const nlohmann::json &message, const nlohmann::json &documents,
    const std::function<nlohmann::json(const std::string &)> &read,
    const std::function<nlohmann::json(const nlohmann::json &)> &image = {});
} // namespace lemon
