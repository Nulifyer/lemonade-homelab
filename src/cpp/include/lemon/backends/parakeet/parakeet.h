#pragma once
#include "lemon/backends/backend_descriptor.h"
namespace lemon { namespace backends { namespace parakeet {
inline const BackendDescriptor descriptor = [] {
    BackendDescriptor d;
    d.recipe = "parakeet";
    d.display_name = "Parakeet Redux C++ CPU";
    d.binary = "parakeet-server";
    d.default_device = DEVICE_CPU;
    d.support = {{"cpu", {"linux"}, {{"cpu", {"x86_64"}}}, "x86_64 CPU"}};
    d.supported_modes = {"transcription"};
    d.experimental = true;
    d.bin_variants = {"cpu"};
    d.options = {{"parakeet_threads", "--parakeet-threads", 4, "SIZE", "CPU transcription threads", "Parakeet Options"}};
    return d;
}();
}}}
