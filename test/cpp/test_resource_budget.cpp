#include <lemon/resource_budget.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace lemon;
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("lemonade-budget-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    auto write = [&](const std::string& path, const std::string& content) {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path) << content;
    };
    int failures = 0;
    auto check = [&](bool condition, const char* name) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    write("proc/meminfo", "MemTotal: 8388608 kB\nMemAvailable: 4194304 kB\n");
    write("cgroup/memory.current", "1073741824");
    write("cgroup/memory.peak", "2147483648");
    write("cgroup/memory.max", "8589934592");
    write("cgroup/cpu.max", "200000 100000");
    write("proc/7/status", "VmRSS: 1048576 kB\n");
    write("proc/7/smaps_rollup", "Pss: 524288 kB\n");
    write("proc/7/stat", "7 (model with spaces) S 1 0 0 0 0 0 0 0 0 0 200 100 0 0\n");
    const std::string drm = "drm-driver: amdgpu\ndrm-client-id: 13\ndrm-pdev: 0000:c7:00.0\ndrm-resident-gtt: 1048576 KiB\ndrm-resident-vram: 1048576 KiB\ndrm-memory-gtt: 1048576 KiB\n";
    write("proc/7/fdinfo/3", drm);
    write("proc/7/fdinfo/4", drm);
    write("proc/7/fdinfo/5", "drm-driver: amdxdna\ndrm-client-id: 7\n");
    auto models = nlohmann::json::array({{{"model_name", "Agent"}, {"pid", 7}, {"device", "gpu|npu"}}, {{"model_name", "Stopped"}, {"pid", 9}}});
    auto result = collect_linux_resource_budget(models, root / "proc", root / "cgroup", 100);
    check(result["host_total_gib"] == 8 && result["host_available_gib"] == 4, "host RAM is converted from KiB");
    check(result["container_current_gib"] == 1 && result["container_limit_gib"] == 8 && result["container_cpu_limit_cores"] == 2, "cgroup budgets use declared units");
    const auto& model = result["models"][0];
    check(model["rss_gib"] == 1 && model["pss_gib"] == 0.5 && model["cpu_seconds"] == 3, "CPU accounting accepts process names with spaces");
    check(model["gpu_resident_gib"] == 2, "duplicate DRM descriptors and alternate field names count once");
    check(model["npu_client"] == true && !model.contains("npu_percent"), "NPU ownership does not invent per-model utilization");
    check(result["models"][1]["pss_gib"].is_null() && result["models"][1]["cpu_seconds"].is_null(), "exited processes report missing values instead of zero");
    check(!result.contains("total_allocated_gib"), "overlapping memory is never summed");
    write("cgroup/memory.max", "max");
    write("cgroup/cpu.max", "max 100000");
    result = collect_linux_resource_budget(models, root / "proc", root / "cgroup", 100);
    check(result["container_limit_gib"].is_null() && result["container_cpu_limit_cores"].is_null(), "unlimited cgroups remain unknown limits");
    fs::remove_all(root);
    return failures ? 1 : 0;
}
