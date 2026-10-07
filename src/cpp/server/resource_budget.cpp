#include "lemon/resource_budget.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace lemon {
namespace {
using json = nlohmann::json;
using Fields = std::map<std::string, double>;
constexpr double gib = 1024.0 * 1024.0 * 1024.0;

Fields read_fields(const std::filesystem::path& path) {
    Fields fields;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string name, unit;
        double value;
        if (!(stream >> name >> value) || value < 0) continue;
        if (!name.empty() && name.back() == ':') name.pop_back();
        stream >> unit;
        if (unit == "kB" || unit == "KiB") value *= 1024;
        fields[name] = value;
    }
    return fields;
}

json bytes_field(const Fields& fields, const std::string& key) {
    auto it = fields.find(key);
    return it == fields.end() ? json(nullptr) : json(it->second / gib);
}

json file_number(const std::filesystem::path& path) {
    std::ifstream file(path);
    double value;
    return (file >> value) && value >= 0 ? json(value / gib) : json(nullptr);
}

json process_resources(const json& model, const std::filesystem::path& proc, long ticks) {
    json result;
    for (const char* key : {"model_name", "recipe", "device", "pid"}) {
        if (model.contains(key)) result[key] = model[key];
    }
    result["cpu_seconds"] = nullptr;
    result["rss_gib"] = nullptr;
    result["pss_gib"] = nullptr;
    result["gpu_resident_gib"] = nullptr;
    result["npu_client"] = false;
    const int pid = model.value("pid", -1);
    if (pid <= 0) return result;
    auto directory = proc / std::to_string(pid);
    const auto status = read_fields(directory / "status");
    result["rss_gib"] = bytes_field(status, "VmRSS");
    result["pss_gib"] = bytes_field(read_fields(directory / "smaps_rollup"), "Pss");
    std::ifstream stat(directory / "stat");
    std::string line;
    if (std::getline(stat, line)) {
        const auto end = line.rfind(')');
        if (end != std::string::npos && ticks > 0) {
            std::istringstream stream(line.substr(end + 1));
            std::string field;
            unsigned long long user = 0, system = 0;
            bool valid = true;
            for (int number = 3; number <= 15; ++number) {
                if (!(stream >> field)) { valid = false; break; }
                try {
                    if (number == 14) user = std::stoull(field);
                    if (number == 15) system = std::stoull(field);
                } catch (...) { valid = false; break; }
            }
            if (valid) result["cpu_seconds"] = (double(user) + double(system)) / ticks;
        }
    }
    std::set<std::string> clients;
    double resident = 0;
    bool measured = false;
    std::error_code error;
    auto entries = std::filesystem::directory_iterator(directory / "fdinfo", error);
    for (auto end = std::filesystem::directory_iterator(); entries != end && !error; entries.increment(error)) {
        const auto& entry = *entries;
        std::ifstream file(entry.path());
        std::map<std::string, std::string> identity;
        while (std::getline(file, line)) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto name = line.substr(0, colon);
            auto value = line.substr(colon + 1);
            if (name == "drm-driver" || name == "drm-client-id" || name == "drm-pdev") identity[name] = value;
        }
        if (identity["drm-driver"].find("amdxdna") != std::string::npos) result["npu_client"] = true;
        if (identity["drm-driver"].find("amdgpu") == std::string::npos || identity["drm-client-id"].empty()) continue;
        const auto key = identity["drm-pdev"] + ":" + identity["drm-client-id"];
        // Duplicated descriptors describe one DRM client and must count once.
        if (!clients.insert(key).second) continue;
        auto fields = read_fields(entry.path());
        for (const auto* pool : {"vram", "gtt"}) {
            auto it = fields.find(std::string("drm-resident-") + pool);
            if (it == fields.end()) it = fields.find(std::string("drm-memory-") + pool);
            if (it != fields.end()) { resident += it->second; measured = true; }
        }
    }
    if (measured) result["gpu_resident_gib"] = resident / gib;
    return result;
}
}

nlohmann::json collect_linux_resource_budget(const json& models,
    const std::filesystem::path& proc, const std::filesystem::path& cgroup, long ticks) {
    const auto memory = read_fields(proc / "meminfo");
    json result = {
        {"available", true}, {"unit", "GiB"},
        {"host_total_gib", bytes_field(memory, "MemTotal")},
        {"host_available_gib", bytes_field(memory, "MemAvailable")},
        {"container_current_gib", file_number(cgroup / "memory.current")},
        {"container_peak_gib", file_number(cgroup / "memory.peak")},
        {"container_limit_gib", file_number(cgroup / "memory.max")},
        {"container_cpu_limit_cores", nullptr},
        {"models", json::array()},
        {"accounting", "GPU/GTT, process PSS and cgroup memory overlap. Do not sum them. Host available RAM is headroom; GPU compute may still limit speed. PSS excludes some driver allocations. NPU utilization is device-wide."}
    };
    std::ifstream quota(cgroup / "cpu.max");
    double limit, period;
    if ((quota >> limit >> period) && limit > 0 && period > 0)
        result["container_cpu_limit_cores"] = limit / period;
    for (const auto& model : models) {
        result["models"].push_back(process_resources(model, proc, ticks));
    }
    return result;
}
}
