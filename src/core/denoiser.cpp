#include "core/denoiser.hpp"

#if DAYO_HAS_OIDN
#include <OpenImageDenoise/oidn.h>
#endif

namespace dayo::core {

DenoiserStatus selectDenoiser() {
#if DAYO_HAS_OIDN
    auto tryDevice = [](OIDNDeviceType type, DenoiserBackend backend, const char* name) -> DenoiserStatus {
        OIDNDevice device = oidnNewDevice(type);
        if (device == nullptr) return { DenoiserBackend::unavailable, std::string(name) + " unavailable" };
        oidnCommitDevice(device);
        const char* message = nullptr;
        const auto error = oidnGetDeviceError(device, &message);
        const std::string detail = error == OIDN_ERROR_NONE
            ? std::string(name) + " ready"
            : std::string(name) + ": " + (message == nullptr ? "initialization failed" : message);
        oidnReleaseDevice(device);
        return { error == OIDN_ERROR_NONE ? backend : DenoiserBackend::unavailable, detail };
    };

#ifdef OIDN_DEVICE_TYPE_HIP
    auto hip = tryDevice(OIDN_DEVICE_TYPE_HIP, DenoiserBackend::hip, "OIDN HIP");
    if (hip.backend == DenoiserBackend::hip) return hip;
#endif
    auto cpu = tryDevice(OIDN_DEVICE_TYPE_CPU, DenoiserBackend::cpu, "OIDN CPU");
    if (cpu.backend == DenoiserBackend::cpu) return cpu;
    return { DenoiserBackend::unavailable, "OIDN installed but no usable HIP or CPU device" };
#else
    return { DenoiserBackend::unavailable, "OIDN not installed; denoising disabled" };
#endif
}

} // namespace dayo::core

