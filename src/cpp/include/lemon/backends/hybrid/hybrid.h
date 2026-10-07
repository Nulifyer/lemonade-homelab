#pragma once
#include "lemon/backends/llamacpp/llamacpp.h"

namespace lemon { namespace backends { namespace hybrid {
inline const BackendDescriptor descriptor = [] {
    BackendDescriptor d = llamacpp::descriptor;
    d.recipe = "hybrid";
    d.display_name = "Native XDNA2 prefill / Vulkan decode";
    d.binary = "hybrid-server";
    d.default_device = DEVICE_GPU | DEVICE_NPU;
    d.slot_policy = SlotPolicy::ExclusiveNpu;
    d.selectable_backend = false;
    d.support = {{"npu", {"linux"}, {{"amd_npu", {"XDNA2"}}, {"amd_gpu", {"gfx1150"}}}, "HX 370 XDNA2 NPU and Radeon 890M"}};
    d.experimental = true;
    d.web_display_name = "Native XDNA2 hybrid";
    d.rocm_channels.clear();
    d.rocm_requires_cwsr_fix = false;
    d.arg_variants.clear();
    d.bin_variants = {"npu"};
    d.config_extra = {{"prefer_system", false}};
    d.options[0].default_value = "system";
    d.options.push_back({"hybrid_copy_gib", "--hybrid-copy-gib", 8, "SIZE", "Maximum prepared NPU weight copies in GiB", "Hybrid Options"});
    return d;
}();
}}}
