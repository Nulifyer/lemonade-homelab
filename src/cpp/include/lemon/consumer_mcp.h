#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace lemon {
bool valid_consumer_image_size(const nlohmann::json &size);
nlohmann::json consumer_image_options_schema();
void validate_consumer_image_options(const nlohmann::json &options);
// Bounded JSON-RPC dispatch. ConsumerService owns HTTP sessions and cancellation.
std::optional<nlohmann::json> consumer_mcp(
    const nlohmann::json &message, const nlohmann::json &documents,
    const std::function<nlohmann::json(const std::string &)> &read,
    const std::function<nlohmann::json(const nlohmann::json &)> &image = {});
} // namespace lemon
