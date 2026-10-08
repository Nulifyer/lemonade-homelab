#include "lemon/consumer_mcp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <regex>
#include <stdexcept>

namespace lemon {
using json = nlohmann::json;
bool valid_consumer_image_size(const json &size) {
    if (!size.is_string()) return false;
    static const std::regex pattern("^([1-9][0-9]{0,9})x([1-9][0-9]{0,9})$");
    std::smatch match;
    const auto &text = size.get_ref<const std::string &>();
    if (!std::regex_match(text, match, pattern)) return false;
    const auto width = std::stoll(match[1]), height = std::stoll(match[2]);
    return width <= INT32_MAX && height <= INT32_MAX;
}
json consumer_image_options_schema() {
    return {{"prompt", {{"type", "string"}, {"minLength", 1}}},
            {"negative_prompt", {{"type", "string"}}},
            {"size", {{"type", "string"}, {"pattern", "^[1-9][0-9]{0,9}x[1-9][0-9]{0,9}$"},
                      {"description", "Output WIDTHxHEIGHT, positive 32-bit dimensions. Model-specific alignment and available hardware still apply."}}},
            {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", INT32_MAX}}},
            {"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", INT32_MAX}}},
            {"seed", {{"type", "integer"}, {"minimum", -1}, {"maximum", INT64_MAX}}},
            {"cfg_scale", {{"type", "number"}, {"minimum", 0}}},
            {"flow_shift", {{"type", "number"}}},
            {"clip_skip", {{"type", "integer"}, {"minimum", 0}, {"maximum", INT32_MAX}}},
            {"sample_method", {{"type", "string"}, {"enum", {"euler", "euler_a", "heun", "dpm2", "dpm++2s_a", "dpm++2m", "dpm++2mv2", "ipndm", "ipndm_v", "lcm", "ddim_trailing", "tcd"}}}},
            {"scheduler", {{"type", "string"}, {"enum", {"discrete", "karras", "exponential", "ays", "gits", "smoothstep", "sgm_uniform", "simple", "kl_optimal", "lcm", "beta"}}}}};
}
void validate_consumer_image_options(const json &options) {
    if (!options.is_object()) throw std::invalid_argument("Image options must be an object");
    const auto schema = consumer_image_options_schema();
    for (const auto &[key, value] : options.items()) {
        if (!schema.contains(key)) throw std::invalid_argument("Unsupported image option: " + key);
        const auto &field = schema.at(key);
        const auto type = field.at("type");
        if (type == "string") {
            if (!value.is_string()) throw std::invalid_argument(key + " must be a string");
            const auto &text = value.get_ref<const std::string &>();
            if (key == "prompt" && text.empty()) throw std::invalid_argument("Image prompt required");
            if ((key == "prompt" || key == "negative_prompt") && text.find("<sd_cpp_extra_args>") != std::string::npos)
                throw std::invalid_argument("Runtime control markup is not an image prompt");
            if (key == "size" && !valid_consumer_image_size(value))
                throw std::invalid_argument("Image size must be positive WIDTHxHEIGHT within 32-bit dimensions");
            if (field.contains("enum") && std::find(field["enum"].begin(), field["enum"].end(), value) == field["enum"].end())
                throw std::invalid_argument("Unsupported image " + key);
        } else if (type == "integer") {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(INT64_MAX)))
                throw std::invalid_argument(key + " must be an integer within the runtime range");
            const auto number = value.get<int64_t>();
            if (number < field.at("minimum").get<int64_t>() || number > field.at("maximum").get<int64_t>())
                throw std::invalid_argument("Invalid image " + key);
        } else {
            if (!value.is_number()) throw std::invalid_argument(key + " must be a number");
            const auto number = value.get<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max() ||
                (field.contains("minimum") && number < field["minimum"].get<double>()))
                throw std::invalid_argument("Invalid image " + key);
        }
    }
}
std::optional<json>
consumer_mcp(const json &message, const json &documents,
             const std::function<json(const std::string &)> &read,
             const std::function<json(const json &)> &image) {
    const json id = message.is_object() ? message.value("id", json(nullptr))
                                        : json(nullptr);
    auto error = [&](int code, const char *text) {
        return json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"error", {{"code", code}, {"message", text}}}};
    };
    auto result = [&](const json &value) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", value}};
    };
    if (!message.is_object() || message.value("jsonrpc", json()) != "2.0" ||
        !message.contains("method") || !message["method"].is_string() ||
        !(id.is_null() || id.is_string() || id.is_number_integer()))
        return error(-32600, "Invalid JSON-RPC request");
    const auto method = message["method"].get<std::string>();
    if (!message.contains("id"))
        return std::nullopt;
    if (method == "initialize")
        return result(
            {{"protocolVersion", "2025-06-18"},
             {"capabilities", {{"tools", json::object()}}},
             {"serverInfo", {{"name", "lemonade-consumer"}, {"version", "1"}}},
             {"instructions",
              image ? "Generate on-demand images with client-selected controls. No model "
                      "administration or downloads."
                    : "Read-only model service information and reviewed "
                      "documentation. No downloads, model lifecycle, shell, "
                      "files, "
                      "credentials or infrastructure mutations."}});
    if (method == "ping")
        return result(json::object());
    std::map<std::string, std::pair<std::string, std::string>> paths = {
        {"model_services",
         {"/v1/service",
          "Read model identities, capabilities, URLs, presets and "
          "critical readiness."}},
        {"critical_readiness",
         {"/ready",
          "Inspect whether Home Assistant's critical models are ready."}},
        {"speech_voices",
         {"/v1/audio/voices",
          "List installed English voices and the default voice."}}};
    if (image)
        paths.clear();
    if (method == "tools/list") {
        json tools = json::array();
        auto tool = [&](const std::string &name, const std::string &description,
                        const json &schema) {
            tools.push_back({{"name", name},
                             {"description", description},
                             {"inputSchema", schema},
                             {"annotations",
                              {{"readOnlyHint", true},
                               {"destructiveHint", false},
                               {"idempotentHint", true},
                               {"openWorldHint", false}}}});
        };
        for (const auto &[name, path] : paths)
            tool(name, path.second,
                 {{"type", "object"},
                  {"properties", json::object()},
                  {"additionalProperties", false}});
        if (image) {
            tool("generate_image",
                 "Generate images using the locally selected model. "
                 "Omitted controls use service/model defaults; explicit controls take precedence. "
                 "Describe the scene plainly; generation can take a minute.",
                 {{"type", "object"},
                  {"properties", consumer_image_options_schema()},
                  {"required", {"prompt"}},
                  {"additionalProperties", false}});
            tools.back()["annotations"]["readOnlyHint"] = false;
            tools.back()["annotations"]["idempotentHint"] = false;
        }
        if (!image && !documents.empty()) {
            json names = json::array();
            for (const auto &[name, text] : documents.items())
                names.push_back(name);
            tool(
                "read_runbook",
                "Read an operator-reviewed homelab runbook. This does not read "
                "arbitrary files or current host state.",
                {{"type", "object"},
                 {"properties",
                  {{"name", {{"type", "string"}, {"enum", names}}}}},
                 {"required", {"name"}},
                 {"additionalProperties", false}});
        }
        return result({{"tools", tools}});
    }
    if (method != "tools/call")
        return error(-32601, "Method unavailable");
    try {
        const auto params = message.at("params");
        const auto name = params.at("name").get<std::string>();
        const auto args = params.value("arguments", json::object());
        if (!args.is_object())
            throw std::invalid_argument("Invalid arguments");
        json value;
        if (image) {
            if (name != "generate_image" || !args.contains("prompt"))
                return error(-32602, "Image prompt required");
            try { validate_consumer_image_options(args); }
            catch (const std::invalid_argument &e) { return error(-32602, e.what()); }
            auto generated = image(args);
            if (!generated.contains("data") || !generated["data"].is_array() || generated["data"].empty())
                throw std::runtime_error("Image unavailable");
            json content = json::array({{{"type", "text"},
                {"text", "Image generated successfully. The images are attached to this tool result."}}});
            for (const auto &item : generated["data"]) {
                const auto data = item.at("b64_json").get<std::string>();
                if (data.empty()) throw std::runtime_error("Empty image response");
                content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", data}});
            }
            return result({{"content", content},
                           {"isError", false}});
        }
        if (name == "read_runbook" && args.size() == 1 &&
            args.contains("name") && args["name"].is_string() &&
            documents.contains(args["name"].get<std::string>())) {
            const auto doc = args["name"].get<std::string>();
            value = {
                {"name", doc},
                {"text", documents[doc]},
                {"source",
                 "operator-reviewed deployment document; not live inventory"}};
        } else if (paths.count(name) && args.empty())
            value = read(paths.at(name).first);
        else
            return error(-32602, "Unknown tool or unsupported arguments");
        if (name == "model_services" && value.contains("configuration") &&
            value["configuration"].contains("documents")) {
            json names = json::array();
            for (const auto &[doc, text] :
                 value["configuration"]["documents"].items())
                names.push_back(doc);
            value["configuration"]["documents"] = names;
        }
        return result({{"content", json::array({{{"type", "text"},
                                                 {"text", value.dump()}}})},
                       {"isError", false}});
    } catch (...) {
        return result({{"content",
                        json::array({{{"type", "text"},
                                      {"text", "Model service tool failed"}}})},
                       {"isError", true}});
    }
}
} // namespace lemon
