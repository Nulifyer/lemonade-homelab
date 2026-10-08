#include "lemon/consumer_mcp.h"
#include <map>
#include <regex>
#include <stdexcept>

namespace lemon {
using json = nlohmann::json;
bool valid_consumer_image_size(const json &size) {
    if (!size.is_string()) return false;
    static const std::regex pattern("^([1-9][0-9]{2,3})x([1-9][0-9]{2,3})$");
    std::smatch match;
    const auto &text = size.get_ref<const std::string &>();
    if (!std::regex_match(text, match, pattern)) return false;
    const int width = std::stoi(match[1]), height = std::stoi(match[2]);
    return width >= 256 && width <= 1024 && height >= 256 && height <= 1024 &&
           width % 64 == 0 && height % 64 == 0;
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
              image ? "Generate one bounded on-demand image. No model "
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
                 "Generate one image using the locally selected model. "
                 "Size defaults to the service canvas when omitted. "
                 "Describe the scene plainly; generation can take a minute.",
                 {{"type", "object"},
                  {"properties",
                   {{"prompt",
                     {{"type", "string"},
                      {"minLength", 1},
                      {"maxLength", 2000}}},
                    {"size", {{"type", "string"},
                              {"description", "Output WIDTHxHEIGHT. Each dimension is 256 through 1024 in multiples of 64."},
                              {"pattern", "^(256|320|384|448|512|576|640|704|768|832|896|960|1024)x(256|320|384|448|512|576|640|704|768|832|896|960|1024)$"}}}}},
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
            if (name != "generate_image" || !args.contains("prompt") ||
                !args["prompt"].is_string() || args.size() > 2 ||
                (args.size() == 2 && !args.contains("size")) ||
                (args.contains("size") && !valid_consumer_image_size(args["size"])))
                return error(-32602, "Bounded image prompt required");
            auto generated = image(args);
            if (!generated.contains("data") || generated["data"].size() != 1)
                throw std::runtime_error("Image unavailable");
            const auto data =
                generated["data"][0].at("b64_json").get<std::string>();
            if (data.empty() || data.size() > 8 * 1024 * 1024)
                throw std::runtime_error("Image response exceeded limit");
            return result({{"content", json::array({
                                           {{"type", "text"},
                                            {"text", "Image generated successfully. "
                                                     "The image is attached to this "
                                                     "tool result."}},
                                           {{"type", "image"},
                                            {"mimeType", "image/png"},
                                            {"data", data}}})},
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
