#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace lemon {
// Stateless MCP transport for separate bounded metadata and image tool
// surfaces. The callback accepts only the three fixed consumer GET paths below.
std::optional<nlohmann::json> consumer_mcp(
    const nlohmann::json &message, const nlohmann::json &documents,
    const std::function<nlohmann::json(const std::string &)> &read,
    const std::function<nlohmann::json(const nlohmann::json &)> &image = {});
} // namespace lemon
