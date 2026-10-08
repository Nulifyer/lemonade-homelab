#include "lemon/consumer_mcp.h"
#include "lemon/consumer_service.h"
#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"
#include "lemon/wrapped_server.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
using json = nlohmann::json;
using lemon::ConsumerService;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename F> void invalid(F fn) {
    bool rejected = false;
    try {
        fn();
    } catch (...) {
        rejected = true;
    }
    check(rejected, "Invalid configuration accepted");
}
httplib::Request request(std::string path, json body = json()) {
    httplib::Request r;
    r.path = path;
    r.target = path;
    r.method = body.is_null() ? "GET" : "POST";
    if (!body.is_null()) {
        r.body = body.dump();
        r.set_header("Content-Type", "application/json");
    }
    return r;
}
#ifndef _WIN32
int free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
          "Test port allocation failed");
    socklen_t n = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &n);
    int port = ntohs(address.sin_port);
    close(fd);
    return port;
}
void send_all(int fd, const std::string &b) {
    size_t offset = 0;
    while (offset < b.size()) {
        auto n = send(fd, b.data() + offset, b.size() - offset, MSG_NOSIGNAL);
        check(n > 0, "Voice send failed");
        offset += n;
    }
}
json event(int fd, std::string &payload) {
    std::string line;
    char c;
    do {
        check(recv(fd, &c, 1, 0) == 1, "Voice receive failed");
        if (c != '\n')
            line += c;
    } while (c != '\n');
    auto data = json::parse(line);
    payload.resize(data.value("payload_length", 0));
    for (size_t i = 0; i < payload.size();) {
        auto n = recv(fd, payload.data() + i, payload.size() - i, 0);
        check(n > 0, "Voice payload failed");
        i += n;
    }
    return data;
}
std::string make_wav(bool floating = false, bool nonfinite = false) {
    std::string b = "RIFF";
    auto put = [&](uint32_t n, int count) {
        for (int i = 0; i < count; ++i)
            b += static_cast<char>(n >> (8 * i));
    };
    put(36 + (floating ? 8 : 4), 4);
    b += "WAVEfmt ";
    put(16, 4);
    put(floating ? 3 : 1, 2);
    put(1, 2);
    put(24000, 4);
    put(floating ? 96000 : 48000, 4);
    put(floating ? 4 : 2, 2);
    put(floating ? 32 : 16, 2);
    b += "data";
    put(UINT32_MAX, 4); // Kokoro streaming WAV sentinel
    if (floating) {
        put(nonfinite ? 0x7fc00000 : 0x3f000000, 4);
        put(0xbf000000, 4);
    } else {
        put(42, 2);
        put(0, 2);
    }
    return b;
}
#endif
namespace lemon {
class IdentityTestServer : public WrappedServer {
  public:
    explicit IdentityTestServer(const std::string &name)
        : WrappedServer("stub", "error", nullptr, nullptr) {
        set_model_metadata(name, "", ModelType::LLM, DEVICE_CPU, RecipeOptions());
        set_state(ModelState::READY);
        set_pinned(true);
    }
    void load(const std::string &, const ModelInfo &, const RecipeOptions &, bool) override {}
    void unload() override {}
    bool is_backend_alive() const override { return true; }
    std::string get_backend_health_state() const override { return "busy"; }
};
struct RoutingHelperTestHook {
    static void add_server(Router &router, const std::string &name) {
        std::lock_guard lock(router.load_mutex_);
        router.loaded_servers_.push_back(std::make_unique<IdentityTestServer>(name));
    }
};
} // namespace lemon
void model_identity_contract() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("consumer_identity_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    lemon::utils::set_cache_dir(root.string());
    lemon::utils::set_config_dir(root.string());
    lemon::utils::set_models_dir(root.string());
    {
        std::ofstream(root / "user_models.json") << json{
            {"HA-Test", {{"checkpoint", "org/test:model.gguf"}, {"recipe", "llamacpp"}}}
        }.dump();
        lemon::ModelManager models;
        const auto canonical = models.resolve_model_name("HA-Test");
        check(canonical == "user.HA-Test", "Actual registry namespace not exercised");
        check(models.get_public_model_name(canonical) == "HA-Test",
              "Actual display-name mapping not exercised");
        lemon::RuntimeConfig runtime({{"max_loaded_models", 3}, {"log_level", "error"}});
        lemon::RuntimeConfig::set_global(&runtime);
        {
            lemon::Router router(&runtime, &models, nullptr);
            lemon::RoutingHelperTestHook::add_server(router, canonical);
            auto loaded = router.get_all_loaded_models();
            check(loaded.size() == 1 && loaded[0]["model_name"] == "HA-Test" &&
                      loaded[0]["model_id"] == canonical,
                  "Router health lost canonical model identity");
            ConsumerService::Manager manager;
            manager.health = [&] { return router.get_all_loaded_models(); };
            manager.metadata = [&](const std::string &) {
                return json{{"id", "user.HA-Test"},
                            {"runtime_name", models.resolve_model_name("HA-Test")}};
            };
            ConsumerService service(lemon::ConsumerConfig::parse(
                {{"critical_models", {"small-task"}}}), manager);
            check(service.readiness()["ready"].get<bool>(),
                  "Readiness disagrees with actual registry/Router identity");
            loaded[0]["model_id"] = "user.Other-Test";
            manager.health = [&] { return loaded; };
            ConsumerService mismatched(lemon::ConsumerConfig::parse(
                {{"critical_models", {"small-task"}}}), manager);
            check(!mismatched.readiness()["ready"].get<bool>(),
                  "Display-name match concealed a different canonical model");
        }
        lemon::RuntimeConfig::set_global(nullptr);
    }
    fs::remove_all(root);
}
int main() {
    int reads = 0;
    const json documents = {
        {"architecture", "Reviewed fixture, no credentials"}};
    auto read = [&](const std::string &path) {
        ++reads;
        return json{{"path", path}};
    };
    auto rpc = [&](const std::string &method, json params = json::object()) {
        return *lemon::consumer_mcp({{"jsonrpc", "2.0"},
                                     {"id", 7},
                                     {"method", method},
                                     {"params", params}},
                                    documents, read);
    };
    auto tools = rpc("tools/list")["result"]["tools"];
    check(tools.size() == 4 &&
              tools[3]["inputSchema"]["properties"]["name"]["enum"][0] ==
                  "architecture",
          "Runbook catalog missing");
    auto doc = rpc("tools/call", {{"name", "read_runbook"},
                                  {"arguments", {{"name", "architecture"}}}});
    check(json::parse(
              doc["result"]["content"][0]["text"].get<std::string>())["text"] ==
              documents["architecture"],
          "Runbook read failed");
    check(rpc("tools/call", {{"name", "read_runbook"},
                             {"arguments", {{"name", "../../etc/passwd"}}}})
                  .contains("error") &&
              reads == 0,
          "Arbitrary file read accepted");
    check(rpc("tools/call", {{"name", "model_services"},
                             {"arguments", {{"command", "stop"}}}})
                  .contains("error") &&
              reads == 0,
          "Unexpected tool arguments accepted");
    check(rpc("tools/call", {{"name", "load_model"}}).contains("error") &&
              reads == 0,
          "Lifecycle tool exposed");
    check(!lemon::consumer_mcp(
               {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
               documents, read)
               .has_value(),
          "Notification must have no RPC response");
    auto image = [](const json &) {
        return json{{"data", json::array({{{"b64_json", "cG5n"}}})}};
    };
    auto image_tools = *lemon::consumer_mcp(
        {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, documents,
        read, image);
    check(image_tools["result"]["tools"].size() == 1 &&
              !image_tools["result"]["tools"][0]["annotations"]["readOnlyHint"]
                   .get<bool>(),
          "Image tool scope was mixed with read-only tools");
    auto image_call = *lemon::consumer_mcp(
        {{"jsonrpc", "2.0"},
         {"id", 1},
         {"method", "tools/call"},
         {"params",
          {{"name", "generate_image"}, {"arguments", {{"prompt", "A cube"}}}}}},
        documents, read, image);
    check(image_call["result"]["content"].size() == 2 &&
              image_call["result"]["content"][0]["type"] == "text" &&
              image_call["result"]["content"][0]["text"].get<std::string>().find(
                  "Image generated successfully") != std::string::npos &&
              !image_call["result"]["isError"].get<bool>(),
          "MCP image result must tell text-only tool planners it succeeded");
    check(image_call["result"]["content"][1]["type"] == "image" &&
              image_call["result"]["content"][1]["mimeType"] == "image/png" &&
              image_call["result"]["content"][1]["data"] == "cG5n",
          "MCP image block missing");
    const auto defaults = lemon::ConsumerConfig::defaults();
    check(defaults["critical_models"].size() == 3, "Wrong critical defaults");
    for (const json &input :
         {json{{"unknown", true}}, json{{"port", 0}}, json{{"port", 10300}},
          json{{"documents", {{"bad/path", "text"}}}},
          json{{"documents", {{"oversized", std::string(16385, 'x')}}}},
          json{{"enabled", "yes"}}, json{{"critical_models", nullptr}},
          json{{"critical_models", {"small-task", "small-task"}}},
          json{{"public_url", "http://user:secret@host"}},
          json{{"presets", {{"small-task", {{"min_p", 2}}}}}}})
        invalid([&] { lemon::ConsumerConfig::parse(input); });
    auto partial = lemon::ConsumerConfig::parse(
        {{"presets", {{"small-task", {{"temperature", 0.2}}}}}});
    check(partial.value["presets"]["small-task"]["top_k"] == 20,
          "Partial presets lost defaults");
    lemon::RuntimeConfig runtime({{"consumer", defaults}});
    runtime.set({{"consumer", {{"presets", {{"small-task", {{"temperature", 0.3}}}}}}}});
    check(lemon::ConsumerConfig::parse(runtime.snapshot()["consumer"])
                  .value["presets"]["small-task"]["temperature"] == 0.3,
          "Native config setter failed");
    invalid([&] { runtime.set({{"consumer", {{"port", -1}}}}); });
    std::mutex mutex;
    json health = json::array();
    json captured;
    std::string captured_path;
    std::vector<std::string> ensured;
    bool fail = false, streaming = false;
#ifndef _WIN32
    std::atomic<bool> float_audio{false}, invalid_audio{false};
#endif
    std::mutex voice_mutex;
    std::string selected_voice;
    ConsumerService::Manager manager;
    manager.metadata = [](const std::string &role) {
        return json{{"id", "user.target-" + role},
                    {"runtime_name", "user.target-" + role},
                    {"labels", {"tool_calling"}},
                    {"context_length", 8192}};
    };
    manager.voices = [] {
        return json{"af_heart", "am_adam", "bf_emma", "jf_alpha", "alloy"};
    };
    manager.health = [&] {
        std::lock_guard lock(mutex);
        return health;
    };
    manager.ensure_critical = [&](const std::string &role, std::atomic<bool> &cancel) {
        if (cancel)
            return;
        ensured.push_back(role);
        if (role == "speech-stt" && fail)
            throw std::runtime_error("Fault fixture");
    };
    manager.invoke = [&](const std::string &path, const httplib::Request &req,
                         httplib::Response &res) {
        captured_path = path;
#ifndef _WIN32
        if (path.find("transcriptions") != std::string::npos) {
            check(req.form.get_field("model") == "speech-stt",
                  "Voice transcription alias missing");
            check(req.form.get_file("file").content.substr(0, 4) == "RIFF",
                  "Voice WAV packing failed");
            res.status = 200;
            res.set_content("{\"text\":\"Turn on the light.\"}", "application/json");
            return;
        }
        if (path.find("audio/speech") != std::string::npos) {
            {
                std::lock_guard lock(voice_mutex);
                selected_voice = json::parse(req.body).at("voice");
            }
            res.status = 200;
            res.set_content(make_wav(float_audio, invalid_audio), "audio/wav");
            return;
        }
#endif
        captured = json::parse(req.body);
        if (streaming)
            res.set_chunked_content_provider("text/event-stream",
                                             [](size_t, httplib::DataSink &sink) {
                                                 sink.write("data: [DONE]\n\n", 14);
                                                 sink.done();
                                                 return true;
                                             });
        else {
            res.status = 200;
            res.set_content("{}", "application/json");
        }
    };
    ConsumerService service(lemon::ConsumerConfig::parse({{"max_inflight", 1}}), manager);
    for (const auto &prefix : {"/api/v0/", "/api/v1/", "/v0/", "/v1/"}) {
        httplib::Response res;
        service.handle(request(std::string(prefix) + "models"), res);
        auto body = json::parse(res.body);
        check(res.status == 200 && body["data"].size() == 6 &&
                  body["data"][0].contains("alias_of"),
              "Role discovery failed");
    }
    httplib::Response voices_response;
    service.handle(request("/v1/audio/voices"), voices_response);
    auto voice_catalog = json::parse(voices_response.body);
    check(voice_catalog["voices"].size() == 3 &&
              voice_catalog["voices"][0]["description"] !=
                  "Local compiled speech service",
          "Voice discovery missing usable descriptions");
    auto call = [&](std::string path, json body) {
        httplib::Response res;
        service.handle(request(path, body), res);
        return res.status;
    };
    check(call("/v1/chat/completions", {{"model", "small-task"},
                                        {"messages", json::array()},
                                        {"temperature", 0.4},
                                        {"min_p", 0.1}}) == 200,
          "Chat rejected");
    const auto llm_captured_path = captured_path;
    const std::string exact_prompt = "  An adult art study; literal punctuation: \"red & blue\".\nSecond line.  ";
    json dispatch = {{"model", "image-generation"},
        {"messages", json::array({{{"role", "system"}, {"content", "Rewrite every prompt"}},
                                   {{"role", "user"}, {"content", exact_prompt}}})}};
    dispatch["tools"] = json::parse(R"([{"type":"function","function":{
        "name":"generate_image_mcp_local-images","parameters":{
        "type":"object","properties":{"prompt":{"type":"string"}}}}}])");
    for (const auto &prefix : {"/api/v0/", "/api/v1/", "/v0/", "/v1/"}) {
        captured_path.clear();
        httplib::Response res;
        service.handle(request(std::string(prefix) + "chat/completions", dispatch), res);
        check(res.status == 200 && captured_path.empty(), "Image dispatch invoked a chat LLM");
        const auto message = json::parse(res.body)["choices"][0]["message"];
        const auto tool = message["tool_calls"][0];
        check(json::parse(tool["function"]["arguments"].get<std::string>())["prompt"] == exact_prompt,
              "Direct image prompt was rewritten or guarded");
        auto done = dispatch;
        done["messages"].push_back(message);
        done["messages"].push_back({{"role", "tool"}, {"tool_call_id", tool["id"]},
                                      {"content", "Image generated successfully. The image is attached."}});
        httplib::Response completed;
        service.handle(request(std::string(prefix) + "chat/completions", done), completed);
        check(json::parse(completed.body)["choices"][0]["message"]["content"] == "Image ready.",
              "Successful image tool result not completed");
        auto projected = done;
        projected["messages"].back()["content"] =
            "Tool response is included in the next message as a Human message";
        projected["messages"].push_back({{"role", "user"}, {"content", json::array({
            {{"type", "text"}, {"text", "Image generated successfully. The image is attached to this tool result."}},
            {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,cG5n"}}}}})}});
        projected["messages"].push_back({{"role", "user"}, {"content",
            "System notice: this turn has about 2 more tool-calling rounds left before it is cut off."}});
        httplib::Response projection;
        service.handle(request(std::string(prefix) + "chat/completions", projected), projection);
        check(projection.status == 200 &&
                  json::parse(projection.body)["choices"][0]["message"]["content"] == "Image ready." &&
                  !json::parse(projection.body)["choices"][0]["message"].contains("tool_calls"),
              "LibreChat artifact or budget notice repeated an image job");
        auto next = done;
        next["messages"].push_back({{"role", "assistant"}, {"content", "Image ready."}});
        next["messages"].push_back({{"role", "user"}, {"content", "A new scene"}});
        httplib::Response next_response;
        service.handle(request(std::string(prefix) + "chat/completions", next), next_response);
        check(next_response.status == 200 &&
                  json::parse(json::parse(next_response.body)["choices"][0]["message"]["tool_calls"][0]
                                  ["function"]["arguments"].get<std::string>())["prompt"] == "A new scene",
              "An old image tool result swallowed a new prompt");
        done["messages"].back()["content"] = "Image generation failed";
        httplib::Response failure;
        service.handle(request(std::string(prefix) + "chat/completions", done), failure);
        check(json::parse(failure.body)["choices"][0]["message"]["content"] != "Image ready.",
              "Failed image job reported success");
    }
    auto disabled_dispatch = dispatch;
    disabled_dispatch["tool_choice"] = "none";
    check(call("/v1/chat/completions", disabled_dispatch) == 400,
          "Image dispatcher accepted a disabled tool");
    disabled_dispatch.erase("tools");
    check(call("/v1/chat/completions", disabled_dispatch) == 400,
          "Image dispatcher accepted a missing tool");
    auto array_dispatch = dispatch;
    array_dispatch["messages"].back()["content"] = json::array({{{"type", "text"}, {"text", exact_prompt}}});
    httplib::Response array_response;
    service.handle(request("/v1/chat/completions", array_dispatch), array_response);
    check(array_response.status == 200 &&
              json::parse(json::parse(array_response.body)["choices"][0]["message"]["tool_calls"][0]
                            ["function"]["arguments"].get<std::string>())["prompt"] == exact_prompt,
          "LangChain text block prompt changed");
    auto ambiguous_dispatch = dispatch;
    ambiguous_dispatch["tools"].push_back(dispatch["tools"][0]);
    check(call("/v1/chat/completions", ambiguous_dispatch) == 400,
          "Image dispatcher accepted ambiguous tools");
    auto long_dispatch = dispatch;
    long_dispatch["messages"].back()["content"] = std::string(2001, 'x');
    check(call("/v1/chat/completions", long_dispatch) == 400,
          "Image dispatcher accepted an oversized prompt");
    auto stream_dispatch = dispatch;
    stream_dispatch["stream"] = true;
    httplib::Response stream_response;
    service.handle(request("/v1/chat/completions", stream_dispatch), stream_response);
    std::string sse;
    httplib::DataSink sink;
    sink.write = [&](const char* p, size_t n) { sse.append(p, n); return true; };
    sink.done = [] {};
    check(stream_response.status == 200 && stream_response.content_provider_(0, 0, sink) &&
              sse.find("chat.completion.chunk") != std::string::npos &&
              sse.find("data: [DONE]") != std::string::npos,
          "Image tool SSE failed");
    captured_path = llm_captured_path;
    int image_releases = 0;
    double headroom = 32;
    bool image_entered = false, image_continue = false;
    std::mutex image_mutex;
    std::condition_variable image_wake;
    auto image_manager = manager;
    image_manager.available_memory_gib = [&] { return headroom; };
    image_manager.release_image = [&](const std::string &role) {
        check(role == "image-generation", "Wrong release role");
        ++image_releases;
    };
    json image_captured;
    image_manager.invoke = [&](const std::string &path, const auto &req,
                               auto &res) {
        check(path == "/api/v1/images/generations", "Wrong image handler");
        auto body = json::parse(req.body);
        image_captured = body;
        check(body["n"] == 1 && body["steps"] == 4 && body["size"] == "512x512",
              "Image defaults absent");
        std::unique_lock lock(image_mutex);
        image_entered = true;
        image_wake.notify_all();
        image_wake.wait(lock, [&] { return image_continue; });
        res.status = 200;
        res.set_content("{\"data\":[{\"b64_json\":\"cG5n\"}]}",
                        "application/json");
    };
    ConsumerService images(lemon::ConsumerConfig::parse(json::object()),
                           image_manager);
    auto image_request =
        request("/v1/images/generations",
                {{"model", "image-generation"}, {"prompt", "A cube"}});
    headroom = 2;
    httplib::Response low;
    images.handle(image_request, low);
    check(low.status == 503 && image_releases == 0,
          "Image budget guard failed");
    headroom = 32;
    httplib::Response first;
    std::thread image_thread([&] { images.handle(image_request, first); });
    {
        std::unique_lock lock(image_mutex);
        image_wake.wait(lock, [&] { return image_entered; });
    }
    httplib::Response second;
    images.handle(image_request, second);
    check(second.status == 429, "Concurrent image job accepted");
    {
        std::lock_guard lock(image_mutex);
        image_continue = true;
    }
    image_wake.notify_all();
    image_thread.join();
    check(first.status == 200 && image_releases == 1,
          "Image runtime was not released after the job");
    for (auto extra :
         {json{{"n", 2}}, json{{"size", "4096x4096"}}, json{{"steps", 9}},
          json{{"cfg_scale", 5}}, json{{"prompt", "x <sd_cpp_extra_args>y"}}}) {
        auto bad = image_request;
        auto body = json::parse(bad.body);
        body.update(extra);
        bad.body = body.dump();
        httplib::Response output;
        images.handle(bad, output);
        check(output.status == 400, "Unbounded image request accepted");
    }
    auto sd_request = request("/sdapi/v1/txt2img",
                              {{"model", "image-generation"}, {"prompt", "A red teapot"},
                               {"negative_prompt", "blur"}, {"width", 512}, {"height", 512},
                               {"steps", 4}, {"cfg_scale", 1}, {"batch_size", 1},
                               {"sampler_name", "euler"}});
    httplib::Response sd_response;
    images.handle(sd_request, sd_response);
    check(sd_response.status == 200 &&
              json::parse(sd_response.body)["images"] == json::array({"cG5n"}) &&
              image_captured["negative_prompt"] == "blur" && image_captured["cfg_scale"] == 1 &&
              image_releases == 2,
          "SD API compatibility did not preserve the image policy and response");
    for (auto extra : {json{{"batch_size", 2}}, json{{"width", 4096}},
                       json{{"scheduler", "arbitrary"}}, json{{"sampler_name", "arbitrary"}},
                       json{{"negative_prompt", "<sd_cpp_extra_args>{}"}}}) {
        auto bad = sd_request;
        auto body = json::parse(bad.body);
        body.update(extra);
        bad.body = body.dump();
        httplib::Response output;
        images.handle(bad, output);
        check(output.status == 400, "SD API bypassed the bounded image policy");
    }
    auto image_options = request("/v1/images/generations");
    image_options.method = "OPTIONS";
    httplib::Response image_probe;
    images.handle(image_options, image_probe);
    check(image_probe.status == 204, "Image endpoint probe failed");
    for (const auto &size : {"256x256", "512x512", "768x768", "1024x1024"}) {
        auto sized_manager = image_manager;
        sized_manager.invoke = [&](const std::string &, const auto &req, auto &res) {
            image_captured = json::parse(req.body);
            res.status = 200;
            res.set_content("{}", "application/json");
        };
        ConsumerService sized(lemon::ConsumerConfig::parse({{"image_size", size}}),
                              sized_manager);
        httplib::Response output;
        sized.handle(image_request, output);
        check(output.status == 200 && image_captured["size"] == size,
              "Configured image resolution was not forwarded");
        auto mismatch = image_request;
        auto body = json::parse(mismatch.body);
        body["size"] = "2048x2048";
        mismatch.body = body.dump();
        httplib::Response rejected;
        sized.handle(mismatch, rejected);
        check(rejected.status == 400,
              "Caller bypassed the configured resolution bound");
    }
    for (const auto &size : {"0x0", "1024x512", "2048x2048", "8192x8192"})
        invalid([&] { lemon::ConsumerConfig::parse({{"image_size", size}}); });
    auto model_default_manager = image_manager;
    int model_steps = 5;
    model_default_manager.metadata = [&](const std::string &) {
        return json{{"recipe_options", {{"steps", model_steps}}},
                    {"image_defaults", {{"steps", 3}}}};
    };
    model_default_manager.invoke = [&](const std::string &, const auto &req, auto &res) {
        image_captured = json::parse(req.body);
        res.status = 200;
        res.set_content("{\"data\":[{\"b64_json\":\"cG5n\"}]}", "application/json");
    };
    ConsumerService model_defaults(lemon::ConsumerConfig::parse(json::object()),
                                   model_default_manager);
    httplib::Response selected_default;
    model_defaults.handle(image_request, selected_default);
    check(selected_default.status == 200 && image_captured["steps"] == 5,
          "Image API did not use the selected model's effective steps");
    auto explicit_steps = image_request;
    auto explicit_body = json::parse(explicit_steps.body);
    explicit_body["steps"] = 4;
    explicit_steps.body = explicit_body.dump();
    httplib::Response selected_override;
    model_defaults.handle(explicit_steps, selected_override);
    check(selected_override.status == 200 && image_captured["steps"] == 4,
          "Image API overwrote explicit caller steps");
    model_steps = 9;
    httplib::Response unbounded_default;
    model_defaults.handle(image_request, unbounded_default);
    check(unbounded_default.status == 400,
          "Model defaults bypassed the consumer image step limit");
    model_default_manager.metadata = [](const std::string &) {
        return json{{"image_defaults", {{"steps", 3}}}};
    };
    ConsumerService image_defaults(lemon::ConsumerConfig::parse(json::object()),
                                   model_default_manager);
    httplib::Response selected_image_default;
    image_defaults.handle(image_request, selected_image_default);
    check(selected_image_default.status == 200 && image_captured["steps"] == 3,
          "Image API ignored model image_defaults when recipe steps were absent");
    check(captured["temperature"] == 0.4 && captured["min_p"] == 0.1 &&
              captured["max_tokens"] == 256,
          "Explicit sampling overwritten");
    check(captured_path == "/api/v1/chat/completions", "Native dispatch changed");
    check(call("/api/chat", {{"model", "small-task:latest"},
                             {"temperature", 0.7},
                             {"options", {{"top_k", 5}}}}) == 200,
          "Ollama rejected");
    check(!captured["options"].contains("temperature") &&
              captured["options"]["top_k"] == 5 &&
              captured["options"]["num_predict"] == 256,
          "Ollama precedence changed");
    for (const auto &path : {"/v1/load", "/api/delete", "/internal/config"})
        check(call(path, {{"model", "small-task"}}) == 403, "Administration exposed");
    check(call("/v1/chat/completions", {{"model", "raw-checkpoint"}}) == 403,
          "Unapproved model exposed");
    check(call("/v1/chat/completions", {{"model", "speech-stt"}}) == 400,
          "Speech accepted as LLM");
    auto duplicate = request("/v1/chat/completions", {{"model", "small-task"}});
    duplicate.body = "{\"model\":\"small-task\",\"model\":\"agent-work\"}";
    httplib::Response rejected;
    service.handle(duplicate, rejected);
    check(rejected.status == 400, "Ambiguous JSON accepted");
    auto query = request("/v1/models");
    query.target += "?a=b";
    httplib::Response query_response;
    service.handle(query, query_response);
    check(query_response.status == 400, "Query accepted");
    ConsumerService secured(lemon::ConsumerConfig::parse(json::object()), manager,
                            "test-secret");
    httplib::Response unauthorized;
    secured.handle(request("/v1/models"), unauthorized);
    check(unauthorized.status == 401, "API key ignored");
    auto authorized = request("/v1/models");
    authorized.set_header("Authorization", "Bearer test-secret");
    httplib::Response okay;
    secured.handle(authorized, okay);
    check(okay.status == 200, "Valid key rejected");
    check(!service.readiness()["ready"].get<bool>(), "Empty manager declared ready");
    for (const auto &role : defaults["critical_models"])
        health.push_back({{"model_name", "target-" + role.get<std::string>()},
                          {"model_id", "user.target-" + role.get<std::string>()},
                          {"loaded", true},
                          {"backend_alive", true},
                          {"backend_health", "busy"},
                          {"pinned", true}});
    check(service.readiness()["ready"].get<bool>(),
          "Busy critical models declared unready");
    for (size_t i = 0; i < health.size(); ++i) {
        health[i]["backend_alive"] = false;
        check(!service.readiness()["ready"].get<bool>(),
              "Dead critical backend declared ready");
        health[i]["backend_alive"] = true;
        health[i]["pinned"] = false;
        check(!service.readiness()["ready"].get<bool>(),
              "Unpinned critical backend declared ready");
        health[i]["pinned"] = true;
    }
    fail = true;
    service.reconcile();
    check(ensured.size() == 3 && ensured.back() == "speech-tts",
          "Partial startup failure skipped another critical role");
    for (const auto &role : ensured)
        check(role != "agent-work" && role != "chat-roleplay",
              "On-demand model loaded at startup");
    streaming = true;
    {
        httplib::Response stream;
        service.handle(
            request("/v1/chat/completions", {{"model", "small-task"}, {"stream", true}}),
            stream);
        check(bool(stream.content_provider_), "Stream lost its provider");
        check(call("/v1/chat/completions", {{"model", "small-task"}}) == 429,
              "Streaming admission released before completion");
    }
    streaming = false;
    check(call("/v1/chat/completions", {{"model", "small-task"}}) == 200,
          "Streaming admission leaked");
#ifndef _WIN32
    const int http_port = free_port(), voice_port = free_port();
    std::atomic<bool> image_running{false}, image_aborted{false};
    std::atomic<int> cancelled_image_releases{0};
    auto network_manager = manager;
    network_manager.release_image = [&](const std::string &) { ++cancelled_image_releases; };
    network_manager.invoke = [&, invoke = manager.invoke](const auto &path, const auto &req,
                                                          auto &res) {
        if (path == "/api/v1/images/generations") {
            image_running = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline &&
                   !(req.is_connection_closed && req.is_connection_closed()))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            image_aborted = req.is_connection_closed && req.is_connection_closed();
            res.status = 499;
            res.set_content("{}", "application/json");
            return;
        }
        invoke(path, req, res);
    };
    ConsumerService network(lemon::ConsumerConfig::parse({{"enabled", true},
                                                          {"host", "127.0.0.1"},
                                                          {"port", http_port},
                                                          {"wyoming_host", "127.0.0.1"},
                                                          {"wyoming_port", voice_port},
                                                          {"critical_models", json::array()}}),
                            network_manager);
    network.start();
    httplib::Client http("127.0.0.1", http_port);
    http.set_connection_timeout(2);
    auto live = http.Get("/ready");
    check(live && live->status == 200, "Network readiness failed");
    auto image_ping = http.Options("/v1/images/generations");
    check(image_ping && image_ping->status == 204, "Network image discovery failed");
    auto denied_options = http.Options("/internal/config");
    check(denied_options && denied_options->status == 403,
          "Network OPTIONS exposed administration");
    const auto initialize_body =
        json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}.dump();
    auto init_a = http.Post("/mcp/images", initialize_body, "application/json");
    auto init_b = http.Post("/mcp/images", initialize_body, "application/json");
    check(init_a && init_b && init_a->status == 200 && init_b->status == 200,
          "Image MCP initialization failed");
    const auto session_a = init_a->get_header_value("Mcp-Session-Id");
    const auto session_b = init_b->get_header_value("Mcp-Session-Id");
    check(!session_a.empty() && session_a != session_b, "MCP clients share a session");
    const auto call_body =
        json{{"jsonrpc", "2.0"},
             {"id", 2},
             {"method", "tools/call"},
             {"params", {{"name", "generate_image"}, {"arguments", {{"prompt", "A cube"}}}}}}
            .dump();
    auto missing_session = http.Post("/mcp/images", call_body, "application/json");
    check(missing_session && missing_session->status == 400, "Sessionless image MCP call accepted");
    httplib::Result cancelled_call;
    std::thread image_rpc([&] {
        httplib::Client client("127.0.0.1", http_port);
        cancelled_call = client.Post("/mcp/images", {{"Mcp-Session-Id", session_a}}, call_body,
                                     "application/json");
    });
    for (int i = 0; i < 100 && !image_running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto cancellation = json{
        {"jsonrpc", "2.0"},
        {"method", "notifications/cancelled"},
        {"params",
         {{"requestId", 2}}}}.dump();
    auto foreign_cancel =
        http.Post("/mcp/images", {{"Mcp-Session-Id", session_b}}, cancellation, "application/json");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool isolated = image_running && !image_aborted;
    auto duplicate_rpc =
        http.Post("/mcp/images", {{"Mcp-Session-Id", session_a}}, call_body, "application/json");
    auto cancel_rpc =
        http.Post("/mcp/images", {{"Mcp-Session-Id", session_a}}, cancellation, "application/json");
    image_rpc.join();
    check(foreign_cancel && foreign_cancel->status == 202 && isolated,
          "Foreign MCP cancellation reached another client's job");
    check(duplicate_rpc && duplicate_rpc->status == 409, "Duplicate active MCP ID accepted");
    check(cancel_rpc && cancel_rpc->status == 202 && image_aborted && cancelled_call &&
              cancelled_call->status == 202 && cancelled_image_releases == 1,
          "MCP cancellation did not abort and release the image runtime");
    auto terminated = http.Delete("/mcp/images", httplib::Headers{{"Mcp-Session-Id", session_a}});
    auto expired =
        http.Post("/mcp/images", {{"Mcp-Session-Id", session_a}}, cancellation, "application/json");
    check(terminated && terminated->status == 204 && expired && expired->status == 404,
          "MCP session termination failed");
    auto denied_origin = http.Post("/mcp/images", {{"Origin", "https://foreign.invalid"}},
                                   initialize_body, "application/json");
    check(denied_origin && denied_origin->status == 403, "Foreign browser MCP origin accepted");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(voice_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0,
          "Voice connect failed");
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    send_all(fd, "{\"type\":\"describe\"}\n");
    std::string payload;
    auto info = event(fd, payload);
    check(info["data"]["asr"][0]["models"][0]["name"] == "speech-stt",
          "Wyoming discovery failed");
    check(info["data"]["tts"][0]["voices"].size() == 3, "Wyoming voice list incomplete");
    for (bool use_float : {false, true}) {
        float_audio = use_float;
        send_all(fd, "{\"type\":\"synthesize\",\"data\":{\"text\":\"hello\"}}\n");
        check(event(fd, payload)["type"] == "audio-start", "Speech start failed");
        auto chunk = event(fd, payload);
        check(chunk["type"] == "audio-chunk" && payload.size() == 4,
              "PCM conversion failed");
        check(event(fd, payload)["type"] == "audio-stop", "Speech stop failed");
    }
    send_all(fd, "{\"type\":\"synthesize\",\"data\":{\"text\":\"hello\","
                 "\"voice\":{\"name\":\"bf_emma\"}}}\n");
    check(event(fd, payload)["type"] == "audio-start", "Selected voice rejected");
    event(fd, payload);
    event(fd, payload);
    {
        std::lock_guard lock(voice_mutex);
        check(selected_voice == "bf_emma", "Selected voice did not reach backend");
    }
    send_all(fd, "{\"type\":\"synthesize\",\"data\":{\"text\":\"hello\","
                 "\"voice\":{\"name\":\"unknown\"}}}\n");
    check(event(fd, payload)["type"] == "error", "Unsupported voice accepted");
    invalid_audio = true;
    send_all(fd, "{\"type\":\"synthesize\",\"data\":{\"text\":\"hello\"}}\n");
    check(event(fd, payload)["type"] == "error", "Nonfinite backend audio accepted");
    invalid_audio = false;
    send_all(fd, "{\"type\":\"transcribe\"}\n{\"type\":\"audio-start\",\"data\":{"
                 "\"rate\":"
                 "16000,\"width\":2,\"channels\":1}}\n{\"type\":\"audio-chunk\","
                 "\"data\":{"
                 "\"rate\":16000,\"width\":2,\"channels\":1},\"payload_length\":4}\n");
    send_all(fd, std::string(4, '\0') + "{\"type\":\"audio-stop\"}\n");
    check(event(fd, payload)["data"]["text"] == "Turn on the light.",
          "Voice transcription failed");
    // Stop with an idle open voice socket; all listener/session threads must
    // exit.
    network.stop();
    close(fd);
#endif
    model_identity_contract();
    std::cout << "Native consumer configuration, defaults, discovery, admission, "
                 "readiness, recovery and Wyoming passed\n";
}
