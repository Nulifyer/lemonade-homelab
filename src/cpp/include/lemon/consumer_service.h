#pragma once

#include <atomic>
#include <functional>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace lemon {

// Shared deployment policy. Stored in config.json, never in client UI settings.
struct ConsumerConfig {
    nlohmann::json value;
    static nlohmann::json defaults();
    static ConsumerConfig parse(const nlohmann::json &value);
};

// Uses the manager's handlers and Router. No HTTP proxy or second model owner.
class ConsumerService {
  public:
    struct Manager {
        nlohmann::json openapi;
        std::function<void()> failed;
        std::function<void(const std::string &)> release_image;
        std::function<double()> available_memory_gib;
        std::function<void(const std::string &, const httplib::Request &,
                           httplib::Response &)>
            invoke;
        std::function<nlohmann::json()> health;
        std::function<nlohmann::json()> voices;
        std::function<nlohmann::json(const std::string &)> metadata;
        std::function<void(const std::string &, std::atomic<bool> &)> ensure_critical;
    };
    ConsumerService(ConsumerConfig config, Manager manager, std::string api_key = {});
    ~ConsumerService();
    void start();
    void stop();
    void handle(const httplib::Request &request, httplib::Response &response);
    nlohmann::json readiness() const;
    nlohmann::json configuration() const;
    void reconcile();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lemon
