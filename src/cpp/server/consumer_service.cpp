#include "lemon/consumer_service.h"
#include "lemon/consumer_mcp.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace lemon {
using json = nlohmann::json;
namespace {
const std::map<std::string, std::string> purposes = {
    {"small-task", "Home Assistant and small general tasks"},
    {"agent-work", "Research, coding and general tasks"},
    {"chat-roleplay", "Roleplay and conversation"},
    {"speech-stt", "English speech to text"},
    {"speech-tts", "English utility speech"},
    {"image-generation", "Compact on-demand image generation"}};
const std::vector<std::string> prefixes = {"/api/v0/", "/api/v1/", "/v0/",
                                           "/v1/"};
std::string endpoint(const std::string &path) {
    for (const auto &prefix : prefixes)
        if (path.rfind(prefix, 0) == 0)
            return path.substr(prefix.size());
    if (path == "/ready")
        return "ready";
    if (path == "/health" || path == "/live")
        return "live";
    if (path == "/openapi.json")
        return "openapi.json";
    return path;
}
void reply(httplib::Response &res, int status, const json &body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}
void reject(httplib::Response &res, int status, const char *message) {
    reply(res, status,
          {{"error", {{"message", message}, {"type", "invalid_request_error"}}}});
}
json parse_unique(const std::string &text) {
    std::vector<std::set<std::string>> keys;
    return json::parse(text, [&](int depth, json::parse_event_t event, json &item) {
        if (depth > 64)
            throw std::invalid_argument("JSON nesting is too deep");
        if (event == json::parse_event_t::object_start)
            keys.emplace_back();
        if (event == json::parse_event_t::key &&
            !keys.back().insert(item.get<std::string>()).second)
            throw std::invalid_argument("Duplicate JSON key");
        if (event == json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    });
}
bool valid_id(const std::string &name) {
    return !name.empty() && name.size() <= 200 &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '-' || c == '_' || c == '.';
           });
}
std::string html_escape(const std::string &text) {
    std::string escaped;
    for (const char c : text) {
        switch (c) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped += c;
        }
    }
    return escaped;
}
std::string normalize(std::string name) {
    const std::string suffix = ":latest";
    if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix)
        name.resize(name.size() - suffix.size());
    return name;
}
json english_voices(const json &names) {
    if (!names.is_array() || names.size() > 256)
        throw std::invalid_argument("Invalid voice catalog");
    json list = json::array();
    std::set<std::string> seen;
    for (const auto &raw : names) {
        if (!raw.is_string())
            throw std::invalid_argument("Invalid voice ID");
        const auto name = raw.get<std::string>();
        const auto prefix = name.substr(0, 2);
        if (!valid_id(name) || name.size() < 4 || name[2] != '_' ||
            (prefix != "af" && prefix != "am" && prefix != "bf" && prefix != "bm") ||
            !seen.insert(name).second)
            continue;
        std::string label = name.substr(3);
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        const auto accent = prefix[0] == 'a' ? "American English" : "British English";
        const auto gender = prefix[1] == 'f' ? "female" : "male";
        label += " (" + std::string(accent) + ", " + gender + ", " + name + ")";
        list.push_back({{"id", name},
                        {"name", name},
                        {"description", label},
                        {"languages", {"en"}},
                        {"locale", prefix[0] == 'a' ? "en-US" : "en-GB"},
                        {"gender", gender},
                        {"installed", true}});
    }
    return list;
}
std::string message_text(const json &content) {
    if (content.is_string()) return content.get<std::string>();
    if (content.is_array() && content.size() == 1 &&
        content[0].value("type", "") == "text" && content[0].contains("text"))
        return content[0]["text"].get<std::string>();
    throw std::invalid_argument("A single text image prompt is required");
}
bool image_tool_context(const json &message) {
    const auto role = message.value("role", "");
    if (role == "system" || role == "developer") return true;
    if (role != "user" || !message.contains("content")) return false;
    const auto &content = message["content"];
    if (content.is_string()) {
        const auto &text = content.get_ref<const std::string &>();
        return text.rfind("System notice: this turn has about ", 0) == 0 &&
               text.find(" more tool-calling rounds left before it is cut off.") != std::string::npos;
    }
    if (!content.is_array()) return false;
    bool image = false, success = false;
    for (const auto &block : content) {
        const auto type = block.value("type", "");
        if (type == "image_url") image = true;
        else if (type == "text") {
            const auto text = block.value("text", "");
            success = success || text.rfind("Image generated successfully", 0) == 0;
        } else return false;
    }
    return image && success;
}
struct ImageTurn {
    size_t user;
    size_t assistant;
    size_t result;
};
ImageTurn image_turn(const json &messages) {
    if (!messages.is_array() || messages.empty() || messages.size() > 256)
        throw std::invalid_argument("Bounded messages are required");
    size_t user = messages.size(), assistant = messages.size(), result = messages.size();
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto role = messages[i].value("role", "");
        if (role == "user") user = i;
        if (role == "assistant") assistant = i;
    }
    if (assistant != messages.size() && messages[assistant].contains("tool_calls")) {
        const auto &calls = messages[assistant]["tool_calls"];
        if (!calls.is_array() || calls.size() != 1)
            throw std::invalid_argument("Expected one image tool call");
        for (size_t i = assistant + 1; i < messages.size(); ++i) {
            if (messages[i].value("role", "") == "tool" &&
                messages[i].value("tool_call_id", json()) == calls[0].at("id")) {
                if (result != messages.size()) throw std::invalid_argument("Duplicate image tool result");
                result = i;
            }
        }
        if (result != messages.size()) {
            bool context_only = true;
            for (size_t i = result + 1; i < messages.size(); ++i)
                context_only = context_only && image_tool_context(messages[i]);
            if (context_only) {
                user = messages.size();
                for (size_t i = 0; i < assistant; ++i)
                    if (messages[i].value("role", "") == "user") user = i;
            } else result = messages.size();
        }
    }
    return {user, assistant, result};
}
struct ImagePromptFailure : std::runtime_error {
    int status;
    ImagePromptFailure(int code, const std::string &message)
        : std::runtime_error(message), status(code) {}
};
void dispatch_image(const json &body, const std::string &id, httplib::Response &res,
                    const std::function<std::string(const std::string &)> &write_prompt = {}) {
    if (body.contains("image_size") && !valid_consumer_image_size(body["image_size"]))
        throw std::invalid_argument("Unsupported image size");
    const auto &messages = body.at("messages");
    const auto [user, assistant, result] = image_turn(messages);
    if (user == messages.size()) throw std::invalid_argument("An image prompt is required");
    json message = {{"role", "assistant"}, {"content", nullptr}};
    std::string finish = "stop";
    if (result == messages.size()) {
        auto prompt = message_text(messages[user].at("content"));
        if (prompt.empty() || prompt.size() > 2000)
            throw std::invalid_argument("Image prompt must contain 1 through 2000 bytes");
        if (user + 1 != messages.size())
            throw std::invalid_argument("Expected a new image prompt or matching tool result");
        std::string tool_name;
        const auto &tools = body.at("tools");
        if (!tools.is_array() || tools.size() > 64)
            throw std::invalid_argument("Image tool is required");
        for (const auto &tool : tools) {
            if (tool.value("type", "") != "function") continue;
            const auto &function = tool.at("function");
            const auto name = function.value("name", "");
            if (name != "generate_image" && name.rfind("generate_image_mcp_", 0) != 0)
                continue;
            if (!valid_id(name) || !tool_name.empty() ||
                !function.at("parameters").at("properties").contains("prompt"))
                throw std::invalid_argument("An unambiguous image tool is required");
            tool_name = name;
        }
        if (tool_name.empty() || body.value("tool_choice", json("auto")) == "none")
            throw std::invalid_argument("Image tool must be enabled");
        if (write_prompt) prompt = write_prompt(prompt);
        json arguments = {{"prompt", prompt}};
        if (body.contains("image_size")) arguments["size"] = body["image_size"];
        message["tool_calls"] = json::array({{
            {"id", "image_" + id}, {"type", "function"},
            {"function", {{"name", tool_name},
                          {"arguments", arguments.dump()}}}}});
        finish = "tool_calls";
    } else {
        const auto &calls = messages[assistant]["tool_calls"];
        const auto name = calls[0].at("function").value("name", "");
        if (name != "generate_image" && name.rfind("generate_image_mcp_", 0) != 0)
            throw std::invalid_argument("Unexpected image tool");
        std::string text = messages[result].at("content").is_string()
            ? messages[result]["content"].get<std::string>() : messages[result]["content"].dump();
        // LibreChat projects image artifacts into a synthetic user message and adds budget notices.
        for (size_t i = result + 1; i < messages.size(); ++i) {
            const auto &content = messages[i].at("content");
            if (content.is_array())
                for (const auto &block : content)
                    if (block.value("type", "") == "text") text += block.value("text", "");
        }
        const bool success = text.find("Image generated successfully") != std::string::npos &&
                             text.find("\"isError\":true") == std::string::npos;
        message["content"] = success ? "Image ready." : "Image generation failed. Check the tool details.";
    }
    const auto created = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    json response = {{"id", "chatcmpl-" + id}, {"object", "chat.completion"},
                     {"created", created}, {"model", normalize(body.at("model").get<std::string>())},
                     {"system_fingerprint", "lemonade-image-dispatch"},
                     {"choices", json::array({{{"index", 0}, {"message", message},
                                                {"finish_reason", finish}}})},
                     {"usage", {{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}}}};
    if (!body.value("stream", false)) {
        reply(res, 200, response);
        return;
    }
    auto delta = message;
    if (delta.contains("tool_calls")) delta["tool_calls"][0]["index"] = 0;
    response["object"] = "chat.completion.chunk";
    response.erase("usage");
    response["choices"][0].erase("message");
    response["choices"][0]["delta"] = delta;
    response["choices"][0]["finish_reason"] = nullptr;
    auto final = response;
    final["choices"][0]["delta"] = json::object();
    final["choices"][0]["finish_reason"] = finish;
    const auto data = "data: " + response.dump() + "\n\ndata: " + final.dump() + "\n\ndata: [DONE]\n\n";
    res.status = 200;
    res.set_chunked_content_provider("text/event-stream", [data](size_t, httplib::DataSink &sink) {
        const auto ok = sink.write(data.data(), data.size());
        sink.done();
        return ok;
    });
}
bool healthy(const json &model) {
    const auto health = model.value("backend_health", "");
    return model.value("loaded", false) && model.value("backend_alive", false) &&
           (health == "ready" || health == "busy");
}
} // namespace

json ConsumerConfig::defaults() {
    return {{"enabled", false},
            {"host", "0.0.0.0"},
            {"port", 8080},
            {"wyoming_host", "0.0.0.0"},
            {"wyoming_port", 10300},
            {"tts_voice", "af_heart"},
            {"inference_timeout_seconds", 1200},
            {"reconcile_interval_seconds", 10},
            {"max_inflight", 8},
            {"max_voice_clients", 16},
            {"image_max_steps", 8},
            {"image_timeout_seconds", 300},
            {"image_size", "512x512"},
            {"image_min_available_gib", 12},
            {"image_prompt_model", ""},
            {"critical_models", {"small-task", "speech-stt", "speech-tts"}},
            {"public_url", "http://localhost:8080"},
            {"chat_url", ""},
            {"manager_url", ""},
            {"console_url", ""},
            {"documents", json::object()},
            {"presets",
             {{"small-task",
               {{"temperature", 0},
                {"top_p", 1},
                {"top_k", 20},
                {"min_p", 0},
                {"repeat_penalty", 1},
                {"presence_penalty", 0},
                {"frequency_penalty", 0},
                {"max_tokens", 256}}},
              {"agent-work",
               {{"temperature", 0.6},
                {"top_p", 0.95},
                {"top_k", 20},
                {"min_p", 0},
                {"repeat_penalty", 1},
                {"presence_penalty", 0},
                {"frequency_penalty", 0},
                {"max_tokens", 2048}}},
              {"chat-roleplay",
               {{"temperature", 1},
                {"top_p", 0.95},
                {"top_k", 0},
                {"min_p", 0.05},
                {"repeat_penalty", 1.05},
                {"presence_penalty", 0},
                {"frequency_penalty", 0},
                {"max_tokens", 1024}}}}}};
}
ConsumerConfig ConsumerConfig::parse(const json &input) {
    if (!input.is_object())
        throw std::invalid_argument("consumer must be an object");
    json result = defaults();
    for (const auto &[key, item] : input.items()) {
        if (!result.contains(key))
            throw std::invalid_argument("Unknown consumer setting: " + key);
        if (item.is_null())
            throw std::invalid_argument("Null consumer setting: " + key);
    }
    result.merge_patch(input);
    if (!result["enabled"].is_boolean())
        throw std::invalid_argument("consumer.enabled must be boolean");
    for (const auto &key :
         {"port", "wyoming_port", "inference_timeout_seconds",
          "reconcile_interval_seconds", "max_inflight", "max_voice_clients"}) {
        if (!result[key].is_number_integer())
            throw std::invalid_argument(std::string("consumer.") + key +
                                        " must be an integer");
        const auto n = result[key].get<int64_t>();
        int64_t upper = std::string(key).find("port") != std::string::npos ? 65535 : 1800;
        if (std::string(key).find("max_") == 0)
            upper = 64;
        if (n < 1 || n > upper)
            throw std::invalid_argument(std::string("consumer.") + key +
                                        " is out of range");
    }
    if (result["port"] == result["wyoming_port"])
        throw std::invalid_argument("consumer listeners must have distinct ports");
    for (const auto &key : {"host", "wyoming_host", "tts_voice", "public_url", "chat_url",
                            "manager_url", "console_url"})
        if (!result[key].is_string())
            throw std::invalid_argument(std::string("consumer.") + key +
                                        " must be a string");
    // Wyoming can bind a private container-network alias.
    for (const auto &key : {"host", "wyoming_host"}) {
        if (result[key] != "0.0.0.0" && result[key] != "127.0.0.1" &&
            result[key] != "::" && result[key] != "::1" &&
            !(std::string(key) == "wyoming_host" &&
              valid_id(result[key].get<std::string>())))
            throw std::invalid_argument(
                std::string("consumer.") + key +
                " must be a supported IP or private Wyoming hostname");
    }
    if (!result["image_timeout_seconds"].is_number_integer() ||
        result["image_timeout_seconds"].get<int>() < 1 ||
        result["image_timeout_seconds"].get<int>() > 1800)
        throw std::invalid_argument(
            "image_timeout_seconds must be 1 through 1800");
    if (!valid_consumer_image_size(result["image_size"]))
        throw std::invalid_argument(
            "Image dimensions must be 256 through 1024 in multiples of 64");
    if (!result["image_max_steps"].is_number_integer() ||
        result["image_max_steps"].get<int>() < 1 ||
        result["image_max_steps"].get<int>() > 8)
        throw std::invalid_argument("image_max_steps must be 1 through 8");
    if (!result["image_min_available_gib"].is_number() ||
        !std::isfinite(result["image_min_available_gib"].get<double>()) ||
        result["image_min_available_gib"].get<double>() < 0 ||
        result["image_min_available_gib"].get<double>() > 64)
        throw std::invalid_argument(
            "image_min_available_gib must be 0 through 64");
    if (!result["image_prompt_model"].is_string() ||
        (result["image_prompt_model"] != "" && result["image_prompt_model"] != "chat-roleplay"))
        throw std::invalid_argument("image_prompt_model must be empty or chat-roleplay");
    if (!valid_id(result["tts_voice"]))
        throw std::invalid_argument("Invalid consumer voice ID");
    for (const auto &key : {"public_url", "chat_url", "manager_url", "console_url"}) {
        std::string url = result[key];
        if (url.empty() && std::string(key) != "public_url")
            continue;
        const auto split = url.find("://");
        if (split == std::string::npos ||
            (url.substr(0, split) != "https" && url.substr(0, split) != "http") ||
            url.find_first_of("@?#\r\n\t ") != std::string::npos ||
            split + 3 >= url.size() || url[split + 3] == '/')
            throw std::invalid_argument(std::string("consumer.") + key +
                                        " must be a credential-free HTTP URL");
    }
    if (!result["documents"].is_object() || result["documents"].size() > 16)
        throw std::invalid_argument(
            "documents must be a bounded named text map");
    size_t document_bytes = 0;
    for (const auto &[name, text] : result["documents"].items()) {
        if (!valid_id(name) || !text.is_string() ||
            text.get_ref<const std::string &>().size() > 16384)
            throw std::invalid_argument("Invalid consumer document");
        document_bytes += text.get_ref<const std::string &>().size();
    }
    if (document_bytes > 65536)
        throw std::invalid_argument("Consumer documents exceed 64 KiB");
    std::set<std::string> critical;
    if (!result["critical_models"].is_array())
        throw std::invalid_argument("critical_models must be a list");
    for (const auto &role : result["critical_models"]) {
        if (!role.is_string() || !purposes.count(role.get<std::string>()) ||
            role == "image-generation" ||
            !critical.insert(role.get<std::string>()).second)
            throw std::invalid_argument(
                "critical_models must contain unique supported roles");
    }
    if (!result["presets"].is_object())
        throw std::invalid_argument("presets must be an object");
    for (const auto &[role, preset] : result["presets"].items()) {
        if (!defaults()["presets"].contains(role) || !preset.is_object())
            throw std::invalid_argument("Invalid preset role");
        for (const auto &[key, value] : preset.items()) {
            if (!defaults()["presets"][role].contains(key) || !value.is_number())
                throw std::invalid_argument("Invalid sampling parameter");
            const double n = value.get<double>();
            if (!std::isfinite(n))
                throw std::invalid_argument("Nonfinite sampling parameter");
            if ((key == "top_p" || key == "min_p") && (n < 0 || n > 1))
                throw std::invalid_argument("Sampling probability out of range");
            if (key == "temperature" && (n < 0 || n > 2))
                throw std::invalid_argument("Temperature out of range");
            if ((key == "top_k" || key == "max_tokens") &&
                (!value.is_number_integer() || n < (key == "top_k" ? 0 : 1) ||
                 n > 262144))
                throw std::invalid_argument("Invalid integer sampling parameter");
            if (key == "repeat_penalty" && (n <= 0 || n > 10))
                throw std::invalid_argument("Invalid repeat penalty");
            if ((key == "presence_penalty" || key == "frequency_penalty") &&
                (n < -2 || n > 2))
                throw std::invalid_argument("Invalid additive penalty");
        }
    }
    return {result};
}

struct ConsumerService::Impl {
    ConsumerConfig config;
    Manager manager;
    std::string api_key;
    httplib::Server http;
    std::thread http_thread, warmup_thread;
    std::atomic<bool> stopping{false};
    std::atomic<int> inflight{0};
    std::atomic<int> images_inflight{0};
    struct ImageSession {
        std::chrono::steady_clock::time_point touched;
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> calls;
    };
    std::mutex image_session_mutex;
    std::map<std::string, ImageSession> image_sessions;
    std::atomic<uint64_t> sequence{0}, requests{0}, failures{0};
    std::mutex wait_mutex;
    std::condition_variable wake;
    struct Voice;
    std::unique_ptr<Voice> voice;
    Impl(ConsumerConfig c, Manager m, std::string key);
    ~Impl();
    void start_voice();
    void stop_voice();
};

ConsumerService::ConsumerService(ConsumerConfig config, Manager manager,
                                 std::string api_key)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(manager),
                                   std::move(api_key))) {}
ConsumerService::~ConsumerService() { stop(); }
json ConsumerService::configuration() const { return impl_->config.value; }
json ConsumerService::readiness() const {
    json ready = json::object();
    bool backend = true;
    try {
        const auto models = impl_->manager.health();
        for (const auto &role : impl_->config.value["critical_models"]) {
            const auto metadata = impl_->manager.metadata(role.get<std::string>());
            const auto name = metadata.value("runtime_name", metadata.at("id"));
            bool found = false;
            for (const auto &model : models)
                if (model.value("model_id", model.value("model_name", json())) == name)
                    found = healthy(model) && model.value("pinned", false);
            ready[role.get<std::string>()] = found;
        }
    } catch (...) {
        backend = false;
    }
    bool all = backend;
    for (const auto &value : ready)
        all = all && value.get<bool>();
    return {{"backend", backend}, {"ready", all}, {"critical_models", ready}};
}
void ConsumerService::reconcile() {
    for (const auto &role : impl_->config.value["critical_models"]) {
        if (impl_->stopping)
            return;
        try {
            impl_->manager.ensure_critical(role.get<std::string>(), impl_->stopping);
        } catch (...) {
            std::cerr << json{{"event", "critical_model_failed"}, {"model", role}}.dump()
                      << '\n';
        }
    }
}
void ConsumerService::start() {
    if (!impl_->config.value["enabled"].get<bool>())
        return;
    const auto cfg = configuration();
    impl_->http.new_task_queue = [] { return new httplib::ThreadPool(24, 24, 64); };
    impl_->http.set_payload_max_length(16 * 1024 * 1024 + 65536);
    impl_->http.set_read_timeout(15, 0);
    impl_->http.set_write_timeout(cfg["inference_timeout_seconds"].get<int>(), 0);
    impl_->http.set_keep_alive_timeout(30);
    impl_->http.Get("/.*", [this](const auto &req, auto &res) { handle(req, res); });
    impl_->http.Post("/.*", [this](const auto &req, auto &res) { handle(req, res); });
    impl_->http.Options("/.*", [this](const auto &req, auto &res) { handle(req, res); });
    impl_->http.Delete("/.*", [this](const auto &req, auto &res) { handle(req, res); });
    for (const auto &method : {"PUT", "PATCH"}) {
        auto denied = [](const auto &, auto &res) {
            reject(res, 403, "Consumer administration is unavailable");
        };
        if (std::string(method) == "PUT")
            impl_->http.Put("/.*", denied);
        if (std::string(method) == "PATCH")
            impl_->http.Patch("/.*", denied);
    }
    if (impl_->http.bind_to_port(cfg["host"].get<std::string>(),
                                 cfg["port"].get<int>()) <= 0)
        throw std::runtime_error("Consumer HTTP port is unavailable");
    try {
        impl_->start_voice();
    } catch (...) {
        impl_->http.stop();
        throw;
    }
    impl_->http_thread = std::thread([this] {
        if (!impl_->http.listen_after_bind() && !impl_->stopping) {
            impl_->stopping = true;
            impl_->wake.notify_all();
            std::cerr << "Consumer listener failed\n";
            if (impl_->manager.failed)
                impl_->manager.failed();
        }
    });
    impl_->warmup_thread = std::thread([this] {
        while (!impl_->stopping) {
            reconcile();
            std::unique_lock lock(impl_->wait_mutex);
            impl_->wake.wait_for(
                lock,
                std::chrono::seconds(
                    impl_->config.value["reconcile_interval_seconds"].get<int>()),
                [this] { return impl_->stopping.load(); });
        }
    });
    std::cerr << json{{"event", "consumer_started"},
                      {"port", cfg["port"]},
                      {"wyoming_port", cfg["wyoming_port"]}}
                     .dump()
              << '\n';
}
void ConsumerService::stop() {
    impl_->stopping = true;
    {
        std::lock_guard lock(impl_->image_session_mutex);
        for (auto &[id, session] : impl_->image_sessions)
            for (auto &[request, cancelled] : session.calls)
                *cancelled = true;
    }
    impl_->wake.notify_all();
    impl_->stop_voice();
    impl_->http.stop();
    if (impl_->http_thread.joinable())
        impl_->http_thread.join();
    if (impl_->warmup_thread.joinable())
        impl_->warmup_thread.join();
}

void ConsumerService::handle(const httplib::Request &req, httplib::Response &res) {
    auto &s = *impl_;
    const auto id = "c" + std::to_string(++s.sequence);
    res.set_header("X-Request-ID", id);
    // Compare all bytes even when keys differ in length. Keys are never logged.
    if (!s.api_key.empty()) {
        const auto supplied = req.get_header_value("Authorization");
        const auto expected = "Bearer " + s.api_key;
        size_t diff = supplied.size() ^ expected.size();
        for (size_t i = 0; i < expected.size(); ++i)
            diff |= expected[i] ^ (i < supplied.size() ? supplied[i] : 0);
        if (diff) {
            reject(res, 401, "Unauthorized");
            return;
        }
    }
    const auto ep = endpoint(req.path);
    if (!req.params.empty() || req.target.find('?') != std::string::npos ||
        req.has_header("Content-Encoding")) {
        reject(res, 400, "Query parameters and encoded bodies are unsupported");
        return;
    }
    if (ep == "images/generations" && req.method == "OPTIONS") {
        res.set_header("Allow", "POST, OPTIONS");
        res.status = 204;
        return;
    }
    if (ep == "/sdapi/v1/txt2img" && req.method == "POST") {
        try {
            const auto body = parse_unique(req.body);
            static const std::set<std::string> fields = {
                "model", "prompt", "negative_prompt", "width", "height",
                "steps", "cfg_scale", "seed", "batch_size", "sampler_name", "scheduler"};
            if (!body.is_object()) throw std::invalid_argument("Object required");
            for (const auto &[key, value] : body.items())
                if (!fields.count(key)) throw std::invalid_argument("Unsupported image option");
            if (body.value("batch_size", json(1)) != 1)
                throw std::invalid_argument("One image required");
            const int width = body.value("width", 512), height = body.value("height", 512);
            json translated = {{"model", body.value("model", json("image-generation"))},
                               {"prompt", body.at("prompt")},
                               {"size", std::to_string(width) + "x" + std::to_string(height)}};
            for (const char *field : {"negative_prompt", "steps", "cfg_scale", "seed"})
                if (body.contains(field)) translated[field] = body[field];
            for (const auto &[input, output] : std::map<std::string, std::string>{
                     {"sampler_name", "sample_method"}, {"scheduler", "scheduler"}}) {
                if (body.contains(input) && body[input] != "" && body[input] != "N/A")
                    translated[output] = body[input];
            }
            httplib::Request mapped = req;
            mapped.path = "/v1/images/generations";
            mapped.body = translated.dump();
            handle(mapped, res);
            if (res.status == 200) {
                try {
                    const auto generated = json::parse(res.body);
                    reply(res, 200, {{"images", json::array({generated.at("data").at(0).at("b64_json")})},
                                     {"parameters", body}, {"info", "{}"}});
                } catch (...) {
                    reject(res, 502, "Invalid image backend response");
                }
            }
        } catch (...) {
            reject(res, 400, "Invalid or unsupported image request");
        }
        return;
    }
    if (ep == "/mcp" || ep == "/mcp/images") {
        if (req.has_header("Origin") &&
            req.get_header_value("Origin") != s.config.value["public_url"] &&
            req.get_header_value("Origin") != s.config.value["chat_url"]) {
            reject(res, 403, "MCP origin unavailable");
            return;
        }
        if (ep == "/mcp/images" && req.method == "DELETE") {
            std::lock_guard lock(s.image_session_mutex);
            auto session = s.image_sessions.find(req.get_header_value("Mcp-Session-Id"));
            if (session == s.image_sessions.end()) {
                reject(res, 404, "MCP session unavailable; initialize again");
                return;
            }
            for (auto &[key, cancelled] : session->second.calls)
                *cancelled = true;
            s.image_sessions.erase(session);
            res.status = 204;
            return;
        }
        if (req.method != "POST") {
            res.set_header("Allow", "POST");
            reject(res, 405, "MCP server does not offer an SSE stream");
            return;
        }
        if (req.body.size() > 65536) {
            reject(res, 413, "MCP request too large");
            return;
        }
        try {
            const auto message = parse_unique(req.body);
            const auto method = message.value("method", json());
            std::string session_id = req.get_header_value("Mcp-Session-Id");
            std::shared_ptr<std::atomic<bool>> cancelled;
            std::shared_ptr<void> pending;
            const bool initialize = method == "initialize";
            if (ep == "/mcp/images" && !initialize) {
                if (session_id.empty()) {
                    reject(res, 400, "Initialize an image MCP session first");
                    return;
                }
                std::lock_guard lock(s.image_session_mutex);
                auto session = s.image_sessions.find(session_id);
                if (session != s.image_sessions.end() && session->second.calls.empty() &&
                    std::chrono::steady_clock::now() - session->second.touched >
                        std::chrono::minutes(15)) {
                    s.image_sessions.erase(session);
                    session = s.image_sessions.end();
                }
                if (session == s.image_sessions.end()) {
                    reject(res, 404, "MCP session unavailable; initialize again");
                    return;
                }
                session->second.touched = std::chrono::steady_clock::now();
                if (method == "notifications/cancelled" && !message.contains("id")) {
                    const auto key = message.at("params").at("requestId").dump();
                    auto call = session->second.calls.find(key);
                    if (call != session->second.calls.end())
                        *call->second = true;
                    res.status = 202;
                    return;
                }
                if (method == "tools/call" && message.contains("id") &&
                    (message["id"].is_string() || message["id"].is_number_integer())) {
                    const auto key = message["id"].dump();
                    cancelled = std::make_shared<std::atomic<bool>>(false);
                    if (!session->second.calls.emplace(key, cancelled).second) {
                        reject(res, 409, "MCP request ID is already active");
                        return;
                    }
                    pending = std::shared_ptr<void>(nullptr, [&s, session_id, key](void *) {
                        std::lock_guard lock(s.image_session_mutex);
                        auto session = s.image_sessions.find(session_id);
                        if (session != s.image_sessions.end())
                            session->second.calls.erase(key);
                    });
                }
            }
            std::function<json(const json &)> image;
            if (ep == "/mcp/images")
                image = [&](const json &args) {
                    if (cancelled && cancelled->load())
                        throw std::runtime_error("Image request cancelled");
                    auto nested = req;
                    const auto closed = req.is_connection_closed;
                    nested.is_connection_closed = [closed, cancelled] {
                        return (closed && closed()) || (cancelled && cancelled->load());
                    };
                    nested.path = "/v1/images/generations";
                    nested.target = nested.path;
                    json body = args;
                    body["model"] = "image-generation";
                    nested.body = body.dump();
                    nested.headers.erase("Content-Type");
                    nested.set_header("Content-Type", "application/json");
                    httplib::Response output;
                    handle(nested, output);
                    if (output.status >= 400)
                        throw std::runtime_error("Image request failed");
                    return json::parse(output.body);
                };
            auto response = consumer_mcp(
                message, s.config.value["documents"],
                [&](const std::string &path) {
                    auto nested = req;
                    nested.method = "GET";
                    nested.path = path;
                    nested.target = path;
                    nested.body.clear();
                    httplib::Response output;
                    handle(nested, output);
                    if (output.status >= 400 && output.status != 503)
                        throw std::runtime_error("Consumer read failed");
                    return json::parse(output.body);
                },
                image);
            if (ep == "/mcp/images" && initialize && response && response->contains("result")) {
                std::lock_guard lock(s.image_session_mutex);
                const auto now = std::chrono::steady_clock::now();
                for (auto it = s.image_sessions.begin(); it != s.image_sessions.end();) {
                    if (it->second.calls.empty() &&
                        now - it->second.touched > std::chrono::minutes(15))
                        it = s.image_sessions.erase(it);
                    else
                        ++it;
                }
                if (s.image_sessions.size() >= 64) {
                    reject(res, 503, "Image MCP session capacity reached");
                    return;
                }
                // Keep equal JSON-RPC IDs from different clients isolated.
                std::random_device random;
                do {
                    session_id.clear();
                    for (int i = 0; i < 32; ++i)
                        session_id += "0123456789abcdef"[random() & 15];
                } while (s.image_sessions.count(session_id));
                s.image_sessions.emplace(session_id, Impl::ImageSession{now, {}});
                res.set_header("Mcp-Session-Id", session_id);
            }
            if (cancelled && cancelled->load()) {
                res.status = 202;
                return;
            }
            if (response)
                reply(res, 200, *response);
            else
                res.status = 202;
        } catch (...) {
            reply(res, 200,
                  {{"jsonrpc", "2.0"},
                   {"id", nullptr},
                   {"error", {{"code", -32700}, {"message", "Invalid JSON"}}}});
        }
        return;
    }
    if (req.method == "GET") {
        if (ep == "audio/voices") {
            try {
                reply(res, 200,
                      {{"model", "speech-tts"},
                       {"voices", english_voices(s.manager.voices())},
                       {"default_voice", s.config.value["tts_voice"]}});
            } catch (...) {
                reject(res, 503, "Speech voice catalog unavailable");
            }
            return;
        }
        if (ep == "live") {
            reply(res, 200, {{"alive", true}});
            return;
        }
        if (ep == "ready") {
            const auto state = readiness();
            reply(res, state["ready"].get<bool>() ? 200 : 503, state);
            return;
        }
        if (ep == "models" || ep == "/api/tags" || ep == "service") {
            json list = json::array();
            for (const auto &[role, purpose] : purposes) {
                json item = {{"id", role},
                             {"object", "model"},
                             {"owned_by", "lemonade"},
                             {"purpose", purpose}};
                try {
                    auto metadata = s.manager.metadata(role);
                    item["alias_of"] = metadata["id"];
                    for (const auto &field :
                         {"checkpoint", "labels", "context_length", "size", "recipe"})
                        if (metadata.contains(field))
                            item[field] = metadata[field];
                } catch (...) {
                    item["available"] = false;
                }
                if (role == "image-generation") {
                    item["default_size"] = s.config.value["image_size"];
                    item["size_limits"] = {{"min_dimension", 256}, {"max_dimension", 1024}, {"dimension_multiple", 64}};
                    item["interfaces"] = {"images/generations", "chat/completions"};
                    const auto prompt_model = s.config.value["image_prompt_model"].get<std::string>();
                    item["chat_mode"] = prompt_model.empty()
                        ? "deterministic_image_tool_dispatch" : "creative_image_tool_dispatch";
                    item["chat_modes"] = prompt_model.empty() ? json({"direct"}) : json({"direct", "creative"});
                    item["prompt_model"] = prompt_model;
                    item["prompt_passthrough"] = true;
                    item["chat_prompt_passthrough"] = prompt_model.empty();
                }
                if (ep == "/api/tags") {
                    item["name"] = role + ":latest";
                    item["model"] = role + ":latest";
                    item["digest"] = item.value("alias_of", role);
                    item["size"] = item.value("size", json(0));
                }
                list.push_back(item);
            }
            if (ep == "service")
                reply(res, 200,
                      {{"name", "Lemonade model services"},
                       {"models", list},
                       {"configuration", configuration()},
                       {"readiness", readiness()}});
            else
                reply(res, 200,
                      ep == "/api/tags" ? json{{"models", list}}
                                        : json{{"object", "list"}, {"data", list}});
            return;
        }
        if (ep == "/api/version") {
            reply(res, 200, {{"version", "0.16.1"}});
            return;
        }
        if (ep == "openapi.json") {
            auto contract = s.manager.openapi;
            if (!contract.is_object())
                contract = {
                    {"openapi", "3.0.3"},
                    {"info", {{"title", "Lemonade model services"}, {"version", "1"}}},
                    {"paths", json::object()}};
            contract["servers"] = json::array({{{"url", s.config.value["public_url"]}}});
            contract["security"] = s.api_key.empty()
                                       ? json::array()
                                       : json::array({{{"bearerAuth", json::array()}}});
            for (const auto &prefix : {"/api/v0/", "/api/v1/", "/v0/", "/v1/"}) {
                auto &request = contract["paths"][std::string(prefix) + "chat/completions"]["post"]["requestBody"];
                auto &schema = request["content"]["application/json"]["schema"];
                const auto original = schema.is_object() ? schema : json::object();
                schema = {{"allOf", json::array({original, {
                    {"type", "object"}, {"properties", {{"image_prompt_mode", {
                        {"type", "string"}, {"enum", {"direct", "creative"}},
                        {"description", "For image-generation chat only. Creative uses the configured roleplay writer; direct sends user text unchanged."},
                        {"default", s.config.value["image_prompt_model"] == "" ? "direct" : "creative"}}}}}}})}};
                schema["allOf"][1]["properties"]["image_size"] = {
                    {"type", "string"},
                    {"description", "For image-generation chat only. Explicit output WIDTHxHEIGHT; dimensions are 256 through 1024 in multiples of 64."},
                    {"default", s.config.value["image_size"]}};
            }
            reply(res, 200, contract);
            return;
        }
        if (ep == "/") {
            res.set_header("Content-Security-Policy",
                           "default-src 'none'; style-src 'unsafe-inline'; "
                           "frame-ancestors 'none'; base-uri 'none'");
            std::string links;
            for (const auto &[key, label] : std::map<std::string, std::string>{
                     {"chat_url", "Chat"},
                     {"manager_url", "Model manager"},
                     {"console_url", "Portainer console"}}) {
                const auto url = s.config.value[key].get<std::string>();
                if (!url.empty())
                    links += "<a href=\"" + html_escape(url) + "\">" + label +
                             "</a> &middot; ";
            }
            res.set_content(
                "<!doctype html><html><meta name=viewport "
                "content='width=device-width'><title>Lemonade model "
                "services</title><style>body{max-width:60rem;margin:3rem "
                "auto;padding:1rem;background:#171717;color:#ddd;font:18px "
                "system-ui}a{color:#8cf}</style><h1>Lemonade model "
                "services</h1><p>OpenAI: /v1 &middot; Ollama: /api &middot; "
                "Wyoming: "
                "private port " +
                    std::to_string(s.config.value["wyoming_port"].get<int>()) +
                    "</p><p>" + links +
                    "<a href='/v1/service'>Models and "
                    "configuration</a> &middot; <a href='/openapi.json'>API "
                    "contract</a> "
                    "&middot; <a href='/ready'>Critical "
                    "readiness</a></p></html>",
                "text/html");
            return;
        }
        if (ep == "/metrics") {
            res.set_content("# TYPE lemonade_consumer_requests_total "
                            "counter\nlemonade_consumer_requests_total " +
                                std::to_string(s.requests.load()) +
                                "\n# TYPE lemonade_consumer_failures_total "
                                "counter\nlemonade_consumer_failures_total " +
                                std::to_string(s.failures.load()) + "\n",
                            "text/plain; version=0.0.4");
            return;
        }
        reject(res, 403, "Consumer endpoint unavailable");
        return;
    }
    const std::set<std::string> llm = {"chat/completions", "completions", "responses",
                                       "/api/chat", "/api/show"};
    if (req.method != "POST" ||
        (!llm.count(ep) && ep != "audio/speech" &&
         ep != "audio/transcriptions" && ep != "images/generations")) {
        reject(res, 403, "Consumer endpoint unavailable");
        return;
    }
    httplib::Request mapped = req;
    std::string role;
    try {
        if (ep == "audio/transcriptions") {
            if (!req.is_multipart_form_data() || req.form.get_field_count("model") != 1 ||
                req.form.get_field("model") != "speech-stt" ||
                req.form.get_file_count("file") != 1)
                throw std::invalid_argument("speech-stt WAV required");
            for (const auto &[key, field] : req.form.fields)
                if (req.form.get_field_count(key) != 1)
                    throw std::invalid_argument("Duplicate multipart field");
            const auto file = req.form.get_file("file");
            if (file.content.size() < 12 || file.content.substr(0, 4) != "RIFF" ||
                file.content.substr(8, 4) != "WAVE")
                throw std::invalid_argument("WAV required");
            role = "speech-stt";
        } else {
            if (req.body.size() > 4 * 1024 * 1024) {
                reject(res, 413, "Request too large");
                return;
            }
            if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
                reject(res, 415, "JSON required");
                return;
            }
            auto body = parse_unique(req.body);
            if (!body.is_object() || !body.contains("model") ||
                !body["model"].is_string())
                throw std::invalid_argument("Model is required");
            if (body.contains("model_name"))
                throw std::invalid_argument("Use the model field");
            role = normalize(body["model"].get<std::string>());
            if (!purposes.count(role)) {
                reject(res, 403, "Model not allowed");
                return;
            }
            if (ep == "chat/completions" && role == "image-generation") {
                const auto prompt_model = s.config.value["image_prompt_model"].get<std::string>();
                const auto mode = body.value("image_prompt_mode", prompt_model.empty() ? "direct" : "creative");
                if ((mode != "direct" && mode != "creative") || (mode == "creative" && prompt_model.empty()))
                    throw std::invalid_argument("Invalid image prompt mode");
                bool planned = false;
                std::function<std::string(const std::string &)> write_prompt;
                if (mode == "creative") write_prompt = [&](const std::string &prompt) {
                    json plan = {{"model", prompt_model}, {"stream", false}, {"max_tokens", 384},
                        {"messages", json::array({
                            {{"role", "system"}, {"content",
                                "Write a concrete creative image prompt from the user idea. Preserve the subject, "
                                "style and requested details. Add useful composition, lighting, materials and color. "
                                "Do not invent text, signatures or extra subjects. "
                                "Return only a JSON object with the prompt property. Keep the prompt under 1400 "
                                "characters. Do not describe a tool call."}},
                            {{"role", "user"}, {"content", prompt}}})},
                        {"response_format", {{"type", "json_schema"}, {"json_schema", {
                            {"name", "image_prompt"}, {"strict", true}, {"schema", {
                                {"type", "object"}, {"properties", {{"prompt", {{"type", "string"}, {"maxLength", 2000}}}}},
                                {"required", {"prompt"}}, {"additionalProperties", false}}}}}}}};
                    auto planner_request = req;
                    planner_request.path = "/v1/chat/completions";
                    planner_request.body = plan.dump();
                    const auto closed = req.is_connection_closed;
                    const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(s.config.value["image_timeout_seconds"].get<int>());
                    planner_request.is_connection_closed = [closed, deadline] {
                        return (closed && closed()) || std::chrono::steady_clock::now() >= deadline;
                    };
                    httplib::Response planner_response;
                    handle(planner_request, planner_response);
                    if (planner_response.status >= 400)
                        throw ImagePromptFailure(planner_response.status, "Image prompt writer unavailable; try Direct Images");
                    try {
                        if (planner_response.body.size() > 8192) throw std::runtime_error("Oversized prompt response");
                        const auto result = json::parse(planner_response.body);
                        const auto &choice = result.at("choices").at(0);
                        if (choice.at("finish_reason") != "stop") throw std::runtime_error("Incomplete prompt");
                        const auto object = json::parse(choice.at("message").at("content").get<std::string>());
                        if (!object.is_object() || object.size() != 1) throw std::runtime_error("Invalid prompt object");
                        const auto written = object.at("prompt").get<std::string>();
                        if (written.empty() || written.size() > 2000 ||
                            written.find("sd_cpp_extra_args") != std::string::npos)
                            throw std::runtime_error("Invalid prompt");
                        planned = true;
                        return written;
                    } catch (...) {
                        throw ImagePromptFailure(502, "Image prompt writer returned an invalid or incomplete prompt; try Direct Images");
                    }
                };
                dispatch_image(body, id, res, write_prompt);
                ++s.requests;
                std::cerr << json{{"event", "consumer_image_dispatch"},
                                  {"request_id", id}, {"upstream_llm", planned},
                                  {"prompt_model", planned ? prompt_model : ""}, {"prompt_mode", mode}}
                                 .dump() << '\n';
                return;
            }
            if ((ep == "audio/speech" && role != "speech-tts") ||
                (llm.count(ep) && (role.rfind("speech-", 0) == 0 ||
                                   role == "image-generation")) ||
                (ep == "images/generations" && role != "image-generation") ||
                (ep != "images/generations" && role == "image-generation"))
                throw std::invalid_argument("Model does not support endpoint");
            if (ep == "images/generations") {
                static const std::set<std::string> fields = {
                    "model",           "prompt", "size", "n",
                    "response_format", "steps",  "seed", "cfg_scale", "negative_prompt", "sample_method", "scheduler"};
                for (const auto &[key, value] : body.items())
                    if (!fields.count(key))
                        throw std::invalid_argument("Unsupported image option");
                if (!body.contains("prompt") || !body["prompt"].is_string() ||
                    body["prompt"].get_ref<const std::string &>().empty() ||
                    body["prompt"].get_ref<const std::string &>().size() >
                        2000 ||
                    body["prompt"].get_ref<const std::string &>().find(
                        "sd_cpp_extra_args") != std::string::npos)
                    throw std::invalid_argument(
                        "Bounded plain image prompt required");
                if (body.value("n", json(1)) != 1 ||
                    body.value("response_format", json("b64_json")) != "b64_json")
                    throw std::invalid_argument(
                        "One base64 image required");
                const auto size = body.value("size", s.config.value["image_size"]);
                if (!valid_consumer_image_size(size))
                    throw std::invalid_argument("Unsupported image size");
                const std::map<std::string, std::set<std::string>> sampling = {
                    {"sample_method", {"euler", "euler_a", "heun", "dpm2", "dpm++2s_a", "dpm++2m", "dpm++2mv2", "ipndm", "ipndm_v", "lcm", "ddim_trailing", "tcd"}},
                    {"scheduler", {"discrete", "karras", "exponential", "ays", "gits", "smoothstep", "sgm_uniform", "simple", "kl_optimal", "lcm", "beta"}}};
                for (const auto &[field, supported] : sampling)
                    if (body.contains(field) &&
                        (!body[field].is_string() || !supported.count(body[field].get<std::string>())))
                        throw std::invalid_argument("Unsupported image sampler or scheduler");
                json steps = 4;
                if (body.contains("steps")) {
                    steps = body["steps"];
                } else if (s.manager.metadata) {
                    const auto model = s.manager.metadata(role);
                    const auto options = model.value("recipe_options", json::object());
                    const auto defaults = model.value("image_defaults", json::object());
                    if (options.contains("steps"))
                        steps = options["steps"];
                    else if (defaults.contains("steps"))
                        steps = defaults["steps"];
                }
                if (!steps.is_number_integer() || steps.get<int>() < 1 ||
                    steps.get<int>() >
                        s.config.value["image_max_steps"].get<int>())
                    throw std::invalid_argument("Image step limit exceeded");
                if (body.contains("seed") &&
                    (!body["seed"].is_number_integer() ||
                     body["seed"].get<int64_t>() < -1 ||
                     body["seed"].get<int64_t>() > INT32_MAX))
                    throw std::invalid_argument("Invalid seed");
                if (body.contains("cfg_scale") &&
                    (!body["cfg_scale"].is_number() ||
                     !std::isfinite(body["cfg_scale"].get<double>()) ||
                     body["cfg_scale"].get<double>() < 0 || body["cfg_scale"].get<double>() > 4))
                    throw std::invalid_argument("CFG scale must be 0 through 4");
                if (body.contains("negative_prompt") &&
                    (!body["negative_prompt"].is_string() ||
                     body["negative_prompt"].get_ref<const std::string &>().size() > 2000 ||
                     body["negative_prompt"].get_ref<const std::string &>().find("sd_cpp_extra_args") != std::string::npos))
                    throw std::invalid_argument("Bounded plain negative prompt required");
                body["n"] = 1;
                body["response_format"] = "b64_json";
                body["size"] = size;
                body["steps"] = steps;
            }
            if (s.config.value["presets"].contains(role) && ep != "/api/show") {
                auto *target = &body;
                if (ep == "/api/chat") {
                    if (!body.contains("options"))
                        body["options"] = json::object();
                    if (!body["options"].is_object())
                        throw std::invalid_argument("Options must be an object");
                    target = &body["options"];
                }
                for (const auto &[key, value] : s.config.value["presets"][role].items()) {
                    const auto field =
                        ep == "/api/chat" && key == "max_tokens" ? "num_predict" : key;
                    if (!target->contains(field) &&
                        !(ep == "/api/chat" && body.contains(field)))
                        (*target)[field] = value;
                }
            }
            mapped.body = body.dump();
        }
    } catch (const ImagePromptFailure &error) {
        ++s.failures;
        reject(res, error.status, error.what());
        return;
    } catch (...) {
        reject(res, 400, "Invalid or ambiguous consumer request");
        return;
    }
    std::shared_ptr<void> image_admission;
    if (ep == "images/generations") {
        if (s.manager.available_memory_gib) {
            const auto available = s.manager.available_memory_gib();
            if (available >= 0 &&
                available <
                    s.config.value["image_min_available_gib"].get<double>()) {
                reject(res, 503,
                       "Insufficient host memory headroom for an image job");
                return;
            }
        }
        if (s.images_inflight.fetch_add(1) != 0) {
            --s.images_inflight;
            reject(res, 429, "One image job at a time");
            return;
        }
        image_admission = std::shared_ptr<void>(nullptr, [&s](void *) {
            try {
                if (s.manager.release_image)
                    s.manager.release_image("image-generation");
            } catch (...) {
                std::cerr << "Image runtime release failed\n";
            }
            --s.images_inflight;
        });
    }
    if (s.inflight.fetch_add(1) >= s.config.value["max_inflight"].get<int>()) {
        --s.inflight;
        reject(res, 429, "Inference queue full");
        return;
    }
    // Keep admission until a streaming provider finishes, including
    // disconnects.
    auto admission = std::shared_ptr<void>(nullptr, [&s](void *) { --s.inflight; });
    ++s.requests;
    const auto started = std::chrono::steady_clock::now();
    if (ep == "images/generations") {
        auto closed = req.is_connection_closed;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(
                            s.config.value["image_timeout_seconds"].get<int>());
        mapped.is_connection_closed = [closed, deadline] {
            return (closed && closed()) ||
                   std::chrono::steady_clock::now() >= deadline;
        };
    }
    mapped.path = ep.rfind("/api/", 0) == 0 ? ep : "/api/v1/" + ep;
    try {
        s.manager.invoke(mapped.path, mapped, res);
    } catch (...) {
        reject(res, 502, "Backend unavailable; start a fresh request");
    }
    if (ep == "images/generations" && res.body.size() > 8 * 1024 * 1024)
        reject(res, 502, "Image response exceeds service limit");
    if (res.status < 0)
        res.status = 200;
    if (res.status >= 400)
        ++s.failures;
    if (res.content_provider_) {
        const auto release = res.content_provider_resource_releaser_;
        res.content_provider_resource_releaser_ = [admission, release](bool success) {
            if (release)
                release(success);
        };
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    std::cerr << json{{"event", "consumer_request"},
                      {"request_id", id},
                      {"model", role},
                      {"status", res.status},
                      {"duration_ms", ms}}
                     .dump()
              << '\n';
}

// Wyoming implementation follows below.
} // namespace lemon

#ifndef _WIN32
#include <arpa/inet.h>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace lemon {
#ifndef _WIN32
namespace {
struct AudioFormat {
    int rate = 0, width = 0, channels = 0;
    bool valid() const {
        return rate >= 8000 && rate <= 48000 && width == 2 &&
               (channels == 1 || channels == 2);
    }
    bool operator==(const AudioFormat &f) const {
        return rate == f.rate && width == f.width && channels == f.channels;
    }
    json metadata() const {
        return {{"rate", rate}, {"width", width}, {"channels", channels}};
    }
    static AudioFormat parse(const json &d) {
        return {d.value("rate", 0), d.value("width", 0), d.value("channels", 0)};
    }
};
uint32_t little(const std::string &b, size_t offset, size_t n) {
    if (offset + n > b.size())
        throw std::invalid_argument("Truncated audio");
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= static_cast<uint32_t>(static_cast<unsigned char>(b[offset + i])) << (8 * i);
    return v;
}
void put(std::string &b, uint32_t n, int bytes) {
    for (int i = 0; i < bytes; ++i)
        b.push_back(static_cast<char>(n >> (8 * i)));
}
std::string pack_wav(const std::string &pcm, AudioFormat f) {
    std::string b = "RIFF";
    put(b, 36 + static_cast<uint32_t>(pcm.size()), 4);
    b += "WAVEfmt ";
    put(b, 16, 4);
    put(b, 1, 2);
    put(b, f.channels, 2);
    put(b, f.rate, 4);
    put(b, f.rate * f.channels * f.width, 4);
    put(b, f.channels * f.width, 2);
    put(b, 8 * f.width, 2);
    b += "data";
    put(b, static_cast<uint32_t>(pcm.size()), 4);
    b += pcm;
    return b;
}
std::pair<std::string, AudioFormat> unpack_wav(const std::string &wav) {
    if (wav.size() < 12 || wav.size() > 16 * 1024 * 1024 || wav.substr(0, 4) != "RIFF" ||
        wav.substr(8, 4) != "WAVE")
        throw std::invalid_argument("Invalid WAV");
    AudioFormat f;
    uint32_t encoding = 0;
    std::string pcm;
    for (size_t offset = 12; offset + 8 <= wav.size();) {
        const auto kind = wav.substr(offset, 4);
        size_t size = little(wav, offset + 4, 4), start = offset + 8;
        if (kind == "data" && size == UINT32_MAX)
            size = wav.size() - start;
        if (size > wav.size() - start)
            throw std::invalid_argument("Truncated WAV chunk");
        if (kind == "fmt ") {
            if (size < 16)
                throw std::invalid_argument("Invalid WAV format");
            encoding = little(wav, start, 2);
            f = {static_cast<int>(little(wav, start + 4, 4)),
                 static_cast<int>(little(wav, start + 14, 2)) / 8,
                 static_cast<int>(little(wav, start + 2, 2))};
        }
        if (kind == "data")
            pcm = wav.substr(start, size);
        offset = start + size + (size % 2);
    }
    if (encoding == 3 && f.width == 4) {
        if (pcm.size() % 4)
            throw std::invalid_argument("Unaligned WAV");
        std::string converted;
        converted.reserve(pcm.size() / 2);
        for (size_t i = 0; i < pcm.size(); i += 4) {
            const uint32_t bits = little(pcm, i, 4);
            float sample;
            std::memcpy(&sample, &bits, 4);
            if (!std::isfinite(sample))
                throw std::invalid_argument("Nonfinite audio");
            const auto value =
                static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767);
            put(converted, static_cast<uint16_t>(value), 2);
        }
        pcm = std::move(converted);
        f.width = 2;
    } else if (encoding != 1)
        throw std::invalid_argument("Unsupported audio encoding");
    if (!f.valid() || pcm.empty() || pcm.size() % (f.width * f.channels))
        throw std::invalid_argument("Invalid audio format");
    return {pcm, f};
}
void read_exact(int fd, char *out, size_t size) {
    while (size) {
        const auto n = recv(fd, out, size, 0);
        if (n <= 0)
            throw std::runtime_error("Voice disconnected");
        out += n;
        size -= n;
    }
}
void write_exact(int fd, const std::string &b) {
    size_t offset = 0;
    while (offset < b.size()) {
#ifdef MSG_NOSIGNAL
        const auto n = send(fd, b.data() + offset, b.size() - offset, MSG_NOSIGNAL);
#else
        const auto n = send(fd, b.data() + offset, b.size() - offset, 0);
#endif
        if (n <= 0)
            throw std::runtime_error("Voice disconnected");
        offset += n;
    }
}
struct VoiceEvent {
    std::string type;
    json data;
    std::string payload;
};
VoiceEvent read_event(int fd) {
    std::string line;
    char c;
    while (true) {
        read_exact(fd, &c, 1);
        if (c == '\n')
            break;
        if (line.size() >= 8192)
            throw std::invalid_argument("Voice header too large");
        line += c;
    }
    const auto header = parse_unique(line);
    if (!header.is_object())
        throw std::invalid_argument("Invalid voice event");
    VoiceEvent event{
        header.at("type").get<std::string>(), header.value("data", json::object()), {}};
    if (!event.data.is_object())
        throw std::invalid_argument("Invalid voice data");
    const auto data_size = header.value("data_length", 0),
               payload_size = header.value("payload_length", 0);
    if (data_size < 0 || data_size > 65536 || payload_size < 0 || payload_size > 262144)
        throw std::invalid_argument("Voice frame too large");
    if (data_size) {
        std::string data(data_size, '\0');
        read_exact(fd, data.data(), data.size());
        auto additional = parse_unique(data);
        if (!additional.is_object())
            throw std::invalid_argument("Invalid voice data");
        event.data.update(additional);
    }
    event.payload.resize(payload_size);
    if (payload_size)
        read_exact(fd, event.payload.data(), event.payload.size());
    return event;
}
void send_event(int fd, const std::string &type, json data = json::object(),
                const std::string &payload = {}) {
    const json header = {
        {"type", type}, {"data", data}, {"payload_length", payload.size()}};
    write_exact(fd, header.dump() + "\n");
    if (!payload.empty())
        write_exact(fd, payload);
}
json voice_info(const json &voices) {
    auto artifact = [](const std::string &name, const std::string &url) {
        return json{{"name", name},
                    {"description", "Lemonade native speech service"},
                    {"attribution", {{"name", "Homelab"}, {"url", url}}},
                    {"installed", true},
                    {"version", "1"}};
    };
    auto asr = artifact("redux", "https://github.com/mudler/parakeet.cpp");
    auto model =
        artifact("speech-stt", "https://huggingface.co/moondream/parakeet-redux");
    model["languages"] = {"en"};
    asr["models"] = json::array({model});
    asr["supports_transcript_streaming"] = false;
    asr["requires_external_vad"] = true;
    auto tts = artifact("kokoro", "https://github.com/lucasjinreal/Kokoros");
    tts["voices"] = voices;
    for (auto &voice : tts["voices"]) {
        voice["attribution"] = {{"name", "Kokoro"},
                                {"url", "https://huggingface.co/hexgrad/Kokoro-82M"}};
        voice["version"] = "1";
    }
    tts["supports_synthesize_streaming"] = false;
    return {{"asr", json::array({asr})},
            {"tts", json::array({tts})},
            {"wake", json::array()},
            {"handle", json::array()}};
}
} // namespace

struct ConsumerService::Impl::Voice {
    Impl &owner;
    int listener = -1;
    std::thread accept_thread;
    struct Client {
        int fd;
        std::atomic<bool> done{false};
        std::thread thread;
    };
    std::mutex mutex;
    std::vector<std::shared_ptr<Client>> clients;
    std::mutex stt, tts;
    explicit Voice(Impl &s) : owner(s) {}
    ~Voice() { stop(); }
    void start() {
        const auto host = owner.config.value["wyoming_host"].get<std::string>();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *resolved = nullptr;
        const auto port = std::to_string(owner.config.value["wyoming_port"].get<int>());
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved) != 0 || !resolved)
            throw std::runtime_error("Wyoming bind address cannot be resolved");
        auto addresses =
            std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>(resolved, freeaddrinfo);
        listener = socket(resolved->ai_family, SOCK_STREAM, 0);
        if (listener < 0)
            throw std::runtime_error("Wyoming socket unavailable");
        fcntl(listener, F_SETFD, FD_CLOEXEC);
        int on = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (bind(listener, resolved->ai_addr, resolved->ai_addrlen) != 0 ||
            listen(listener, 16) != 0) {
            close(listener);
            listener = -1;
            throw std::runtime_error("Wyoming port unavailable");
        }
        accept_thread = std::thread([this] {
            while (!owner.stopping) {
                {
                    std::lock_guard lock(mutex);
                    for (auto it = clients.begin(); it != clients.end();) {
                        if ((*it)->done) {
                            (*it)->thread.join();
                            it = clients.erase(it);
                        } else
                            ++it;
                    }
                }
                pollfd p{listener, POLLIN, 0};
                if (poll(&p, 1, 500) <= 0)
                    continue;
                const int fd = accept(listener, nullptr, nullptr);
                if (fd < 0)
                    continue;
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                std::lock_guard lock(mutex);
                if (owner.stopping ||
                    clients.size() >=
                        owner.config.value["max_voice_clients"].get<size_t>()) {
                    close(fd);
                    continue;
                }
                timeval read_timeout{120, 0}, write_timeout{10, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout,
                           sizeof(read_timeout));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &write_timeout,
                           sizeof(write_timeout));
                auto client = std::make_shared<Client>();
                client->fd = fd;
                client->thread = std::thread([this, client] {
                    try {
                        session(client->fd);
                    } catch (...) {
                        ++owner.failures;
                    }
                    // Retain the lock while closing, preventing stop() from
                    // using a recycled fd.
                    std::lock_guard lock(mutex);
                    close(client->fd);
                    client->fd = -1;
                    client->done = true;
                });
                clients.push_back(client);
            }
        });
    }
    void stop() {
        if (listener >= 0)
            shutdown(listener, SHUT_RDWR);
        if (accept_thread.joinable())
            accept_thread.join();
        if (listener >= 0) {
            close(listener);
            listener = -1;
        }
        {
            std::lock_guard lock(mutex);
            for (auto &client : clients)
                if (client->fd >= 0)
                    shutdown(client->fd, SHUT_RDWR);
        }
        for (auto &client : clients)
            if (client->thread.joinable())
                client->thread.join();
        clients.clear();
    }
    httplib::Response speech(const std::string &path, httplib::Request req,
                             std::mutex &admission) {
        std::unique_lock lock(admission, std::try_to_lock);
        if (!lock.owns_lock())
            throw std::runtime_error("Speech busy");
        httplib::Response res;
        req.method = "POST";
        req.path = path;
        if (!owner.api_key.empty())
            req.set_header("Authorization", "Bearer " + owner.api_key);
        owner.manager.invoke(path, req, res);
        if ((res.status != 200 && res.status != -1) || res.body.empty() ||
            res.body.size() > 16 * 1024 * 1024)
            throw std::runtime_error("Speech failed");
        return res;
    }
    void session(int fd) {
        std::string audio;
        AudioFormat format;
        bool transcribing = false, started = false;
        while (!owner.stopping) {
            const auto event = read_event(fd);
            const auto &data = event.data;
            if (event.type == "describe") {
                json voices = json::array();
                try {
                    voices = english_voices(owner.manager.voices());
                } catch (...) {
                }
                send_event(fd, "info", voice_info(voices));
            } else if (event.type == "transcribe") {
                audio.clear();
                transcribing = true;
                started = false;
            } else if (event.type == "audio-start") {
                format = AudioFormat::parse(data);
                if (!transcribing || !format.valid())
                    throw std::invalid_argument("Invalid voice state");
                started = true;
            } else if (event.type == "audio-chunk") {
                if (!transcribing || !started || !(AudioFormat::parse(data) == format) ||
                    audio.size() + event.payload.size() > 2 * 1024 * 1024)
                    throw std::invalid_argument("Invalid voice audio");
                audio += event.payload;
            } else if (event.type == "audio-stop") {
                if (!transcribing || !started || audio.empty() ||
                    audio.size() % (format.width * format.channels))
                    throw std::invalid_argument("Invalid voice audio");
                transcribing = started = false;
                try {
                    httplib::Request req;
                    req.set_header("Content-Type",
                                   "multipart/form-data; boundary=wyoming");
                    for (const auto &[key, value] :
                         std::map<std::string, std::string>{{"model", "speech-stt"},
                                                            {"language", "en"},
                                                            {"response_format", "json"}})
                        req.form.fields.emplace(key, httplib::FormField{key, value, {}});
                    req.form.files.emplace("file",
                                           httplib::FormData{"file",
                                                             pack_wav(audio, format),
                                                             "audio.wav",
                                                             "audio/wav",
                                                             {}});
                    audio.clear();
                    const auto res = speech("/api/v1/audio/transcriptions", req, stt);
                    const auto body = json::parse(res.body);
                    send_event(fd, "transcript", {{"text", body.at("text")}});
                } catch (...) {
                    fail(fd, "transcription-failed");
                }
            } else if (event.type == "synthesize") {
                try {
                    const auto text = data.at("text").get<std::string>();
                    if (text.empty() || text.size() > 8192 ||
                        (data.value("text_format", "text") != "text"))
                        throw std::invalid_argument("Invalid synthesis text");
                    std::string voice = owner.config.value["tts_voice"];
                    if (data.contains("voice") && data["voice"].contains("name"))
                        voice = data["voice"]["name"].get<std::string>();
                    bool supported = false;
                    for (const auto &entry : english_voices(owner.manager.voices()))
                        supported = supported || entry["id"] == voice;
                    if (!supported)
                        throw std::invalid_argument("Unsupported voice");
                    httplib::Request req;
                    req.set_header("Content-Type", "application/json");
                    req.body = json{{"model", "speech-tts"},
                                    {"input", text},
                                    {"voice", voice},
                                    {"response_format", "wav"},
                                    {"speed", 1.0}}
                                   .dump();
                    const auto res = speech("/api/v1/audio/speech", req, tts);
                    const auto [pcm, f] = unpack_wav(res.body);
                    send_event(fd, "audio-start", f.metadata());
                    for (size_t offset = 0; offset < pcm.size(); offset += 4096)
                        send_event(fd, "audio-chunk", f.metadata(),
                                   pcm.substr(offset, 4096));
                    send_event(fd, "audio-stop");
                } catch (...) {
                    fail(fd, "synthesis-failed");
                }
            }
        }
    }
    void fail(int fd, const char *code) {
        ++owner.failures;
        std::cerr << json{{"event", "wyoming_failure"}, {"reason", code}}.dump() << '\n';
        send_event(
            fd, "error",
            {{"code", code}, {"text", "Speech request failed. Start a fresh request."}});
    }
};
#else
struct ConsumerService::Impl::Voice {};
#endif
ConsumerService::Impl::Impl(ConsumerConfig c, Manager m, std::string key)
    : config(std::move(c)), manager(std::move(m)), api_key(std::move(key)) {}
ConsumerService::Impl::~Impl() = default;
void ConsumerService::Impl::start_voice() {
#ifndef _WIN32
    voice = std::make_unique<Voice>(*this);
    voice->start();
#else
    throw std::runtime_error("Native Wyoming listener currently requires POSIX sockets");
#endif
}
void ConsumerService::Impl::stop_voice() {
#ifndef _WIN32
    if (voice)
        voice->stop();
#endif
}
} // namespace lemon
