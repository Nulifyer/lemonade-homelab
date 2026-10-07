#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace lemon {
nlohmann::json collect_linux_resource_budget(
    const nlohmann::json& models,
    const std::filesystem::path& proc_root = "/proc",
    const std::filesystem::path& cgroup_root = "/sys/fs/cgroup",
    long ticks_per_second = 100);
}
