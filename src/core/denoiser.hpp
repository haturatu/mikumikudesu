#pragma once

#include <string>

namespace dayo::core {

enum class DenoiserBackend { unavailable, cpu, hip };

struct DenoiserStatus {
    DenoiserBackend backend{DenoiserBackend::unavailable};
    std::string detail;
};

// Tries AMD HIP first and always falls back to the OIDN CPU device.
// Device/filter lifetime remains owned by the renderer once denoising is wired
// into a render target; this probe deliberately releases its temporary device.
[[nodiscard]] DenoiserStatus selectDenoiser();

} // namespace dayo::core
