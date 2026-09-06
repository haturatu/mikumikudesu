#include "core/denoiser.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#if DAYO_HAS_OIDN
#include <OpenImageDenoise/oidn.h>
#endif

namespace dayo::core {

DenoiserStatus selectDenoiser() {
#if DAYO_HAS_OIDN
    auto tryDevice = [](OIDNDeviceType type, DenoiserBackend backend, const char* name) -> DenoiserStatus {
        OIDNDevice device = oidnNewDevice(type);
        if (device == nullptr)
            return {DenoiserBackend::unavailable, std::string(name) + " unavailable"};
        oidnCommitDevice(device);
        const char* message = nullptr;
        const auto error = oidnGetDeviceError(device, &message);
        const std::string detail =
            error == OIDN_ERROR_NONE
                ? std::string(name) + " ready"
                : std::string(name) + ": " + (message == nullptr ? "initialization failed" : message);
        oidnReleaseDevice(device);
        return {error == OIDN_ERROR_NONE ? backend : DenoiserBackend::unavailable, detail};
    };

#ifdef OIDN_DEVICE_TYPE_HIP
    auto hip = tryDevice(OIDN_DEVICE_TYPE_HIP, DenoiserBackend::hip, "OIDN HIP");
    if (hip.backend == DenoiserBackend::hip)
        return hip;
#endif
    auto cpu = tryDevice(OIDN_DEVICE_TYPE_CPU, DenoiserBackend::cpu, "OIDN CPU");
    if (cpu.backend == DenoiserBackend::cpu)
        return cpu;
    return {DenoiserBackend::unavailable, "OIDN installed but no usable HIP or CPU device"};
#else
    return {DenoiserBackend::unavailable, "OIDN not installed; denoising disabled"};
#endif
}

DenoiserRuntime::DenoiserRuntime() = default;

DenoiserRuntime::~DenoiserRuntime() {
    release();
}

DenoiserRuntime::DenoiserRuntime(DenoiserRuntime&& other) noexcept
    : width_(other.width_), height_(other.height_), initialized_(other.initialized_), available_(other.available_),
      shareable_(other.shareable_), forceFallback_(other.forceFallback_), lastPath_(other.lastPath_),
      executeCount_(other.executeCount_), status_(std::move(other.status_)), staging_(std::move(other.staging_)),
      stagingAlbedo_(std::move(other.stagingAlbedo_)), stagingNormal_(std::move(other.stagingNormal_)),
      stagingOutput_(std::move(other.stagingOutput_)) {
#if DAYO_HAS_OIDN
    device_ = std::exchange(other.device_, nullptr);
    filter_ = std::exchange(other.filter_, nullptr);
    cpuDevice_ = std::exchange(other.cpuDevice_, nullptr);
    cpuFilter_ = std::exchange(other.cpuFilter_, nullptr);
#endif
    other.width_ = 0;
    other.height_ = 0;
    other.initialized_ = false;
    other.available_ = false;
}

DenoiserRuntime& DenoiserRuntime::operator=(DenoiserRuntime&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    width_ = other.width_;
    height_ = other.height_;
    initialized_ = other.initialized_;
    available_ = other.available_;
    shareable_ = other.shareable_;
    forceFallback_ = other.forceFallback_;
    lastPath_ = other.lastPath_;
    executeCount_ = other.executeCount_;
    status_ = std::move(other.status_);
    staging_ = std::move(other.staging_);
    stagingAlbedo_ = std::move(other.stagingAlbedo_);
    stagingNormal_ = std::move(other.stagingNormal_);
    stagingOutput_ = std::move(other.stagingOutput_);
#if DAYO_HAS_OIDN
    device_ = std::exchange(other.device_, nullptr);
    filter_ = std::exchange(other.filter_, nullptr);
    cpuDevice_ = std::exchange(other.cpuDevice_, nullptr);
    cpuFilter_ = std::exchange(other.cpuFilter_, nullptr);
#endif
    other.width_ = 0;
    other.height_ = 0;
    other.initialized_ = false;
    other.available_ = false;
    return *this;
}

void DenoiserRuntime::release() noexcept {
#if DAYO_HAS_OIDN
    if (filter_ != nullptr) {
        oidnReleaseFilter(static_cast<OIDNFilter>(filter_));
        filter_ = nullptr;
    }
    if (cpuFilter_ != nullptr) {
        oidnReleaseFilter(static_cast<OIDNFilter>(cpuFilter_));
        cpuFilter_ = nullptr;
    }
    if (device_ != nullptr) {
        oidnReleaseDevice(static_cast<OIDNDevice>(device_));
        device_ = nullptr;
    }
    if (cpuDevice_ != nullptr) {
        oidnReleaseDevice(static_cast<OIDNDevice>(cpuDevice_));
        cpuDevice_ = nullptr;
    }
#endif
    initialized_ = false;
    available_ = false;
}

#if DAYO_HAS_OIDN
bool DenoiserRuntime::ensureCpuFilter() {
    if (cpuFilter_ != nullptr)
        return true;
    auto* device = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
    if (device == nullptr)
        return false;
    oidnCommitDevice(device);
    const char* message = nullptr;
    if (oidnGetDeviceError(device, &message) != OIDN_ERROR_NONE) {
        oidnReleaseDevice(device);
        return false;
    }
    auto* filter = oidnNewFilter(device, "RT");
    if (filter == nullptr) {
        oidnReleaseDevice(device);
        return false;
    }
    cpuDevice_ = device;
    cpuFilter_ = filter;
    return true;
}

bool DenoiserRuntime::executeOidn(void* deviceValue, void* filterValue, std::span<const float> beauty,
                                  std::span<const float> albedo, std::span<const float> normal,
                                  std::span<float> output) {
    auto* device = static_cast<OIDNDevice>(deviceValue);
    auto* filter = static_cast<OIDNFilter>(filterValue);
    oidnSetSharedFilterImage(filter, "color", const_cast<float*>(beauty.data()), OIDN_FORMAT_FLOAT3, width_, height_, 0,
                             0, 0);
    if (!albedo.empty())
        oidnSetSharedFilterImage(filter, "albedo", const_cast<float*>(albedo.data()), OIDN_FORMAT_FLOAT3, width_,
                                 height_, 0, 0, 0);
    if (!normal.empty())
        oidnSetSharedFilterImage(filter, "normal", const_cast<float*>(normal.data()), OIDN_FORMAT_FLOAT3, width_,
                                 height_, 0, 0, 0);
    oidnSetSharedFilterImage(filter, "output", output.data(), OIDN_FORMAT_FLOAT3, width_, height_, 0, 0, 0);
    oidnCommitFilter(filter);
    const char* message = nullptr;
    if (oidnGetDeviceError(device, &message) != OIDN_ERROR_NONE)
        return false;
    oidnExecuteFilter(filter);
    return oidnGetDeviceError(device, &message) == OIDN_ERROR_NONE;
}
#endif

bool DenoiserRuntime::ensure(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        log::error("Denoiser ensure rejected empty extent");
        return false;
    }
    if (initialized_ && width == width_ && height == height_) {
        return available_;
    }
    release();
    width_ = width;
    height_ = height;
    initialized_ = true;
    status_ = selectDenoiser();
    available_ = status_.backend != DenoiserBackend::unavailable;
#if DAYO_HAS_OIDN
    if (available_) {
        const auto type = status_.backend == DenoiserBackend::hip
#ifdef OIDN_DEVICE_TYPE_HIP
                              ? OIDN_DEVICE_TYPE_HIP
                              : OIDN_DEVICE_TYPE_CPU;
#else
                              ? OIDN_DEVICE_TYPE_CPU
                              : OIDN_DEVICE_TYPE_CPU;
#endif
        auto* device = oidnNewDevice(type);
        if (device == nullptr) {
            available_ = false;
            log::warn("Denoiser device creation failed; using CPU fallback");
            return false;
        }
        oidnCommitDevice(device);
        const char* message = nullptr;
        if (oidnGetDeviceError(device, &message) != OIDN_ERROR_NONE) {
            oidnReleaseDevice(device);
            available_ = false;
            log::warn("Denoiser device commit failed; using CPU fallback");
            return false;
        }
        auto* filter = oidnNewFilter(device, "RT");
        if (filter == nullptr) {
            oidnReleaseDevice(device);
            available_ = false;
            log::warn("Denoiser filter creation failed; using CPU fallback");
            return false;
        }
        device_ = device;
        filter_ = filter;
        log::info("Denoiser persistent filter ready: ", width, "x", height, " (", status_.detail, ")");
        return true;
    }
#endif
    if (available_) {
        log::info("Denoiser persistent filter ready: ", width, "x", height, " (", status_.detail, ")");
    } else {
        log::warn("Denoiser unavailable (", status_.detail, "); CPU fallback will copy beauty");
    }
    return available_;
}

bool DenoiserRuntime::execute(const DenoiserExecuteArgs& args) {
    if (args.width == 0 || args.height == 0) {
        log::error("Denoiser execute rejected empty extent");
        return false;
    }
    if (static_cast<std::size_t>(args.width) >
        std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(args.height)) {
        log::error("Denoiser execute pixel count overflow");
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(args.width) * static_cast<std::size_t>(args.height);
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U) {
        log::error("Denoiser execute sample count overflow");
        return false;
    }
    const std::size_t samples = pixels * 3U;
    if (!initialized_ || args.width != width_ || args.height != height_) {
        log::error("Denoiser execute extent ", args.width, "x", args.height, " mismatches filter ", width_, "x",
                   height_);
        return false;
    }
    if (args.beauty.size() != samples || args.output.size() != samples) {
        log::error("Denoiser execute beauty/output size mismatch for ", args.width, "x", args.height);
        return false;
    }
    if (!args.albedo.empty() && args.albedo.size() != samples) {
        log::error("Denoiser execute albedo size mismatch");
        return false;
    }
    if (!args.normal.empty() && args.normal.size() != samples) {
        log::error("Denoiser execute normal size mismatch");
        return false;
    }
    if (!available_) {
        std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
        lastPath_ = DenoiserPath::passthrough;
        ++executeCount_;
        log::warn("Denoiser unavailable; passed beauty through without denoising");
        return true;
    }
#if DAYO_HAS_OIDN
    if (!shareable_ || forceFallback_) {
        staging_.assign(args.beauty.begin(), args.beauty.end());
        stagingAlbedo_.assign(args.albedo.begin(), args.albedo.end());
        stagingNormal_.assign(args.normal.begin(), args.normal.end());
        stagingOutput_.resize(samples);
        if (!ensureCpuFilter() ||
            !executeOidn(cpuDevice_, cpuFilter_, staging_, stagingAlbedo_, stagingNormal_, stagingOutput_)) {
            std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
            lastPath_ = DenoiserPath::passthrough;
            ++executeCount_;
            log::warn("Denoiser CPU staging execution failed; passed beauty through");
            return true;
        }
        std::copy(stagingOutput_.begin(), stagingOutput_.end(), args.output.begin());
        lastPath_ = forceFallback_ ? DenoiserPath::cpu : DenoiserPath::staging;
        ++executeCount_;
        log::debug("Denoiser staging/readback -> CPU -> upload finished");
        return true;
    }
    if (filter_ != nullptr) {
        if (!executeOidn(device_, filter_, args.beauty, args.albedo, args.normal, args.output)) {
            std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
            lastPath_ = DenoiserPath::passthrough;
            ++executeCount_;
            log::warn("Denoiser shared execution failed; passed beauty through");
            return true;
        }
        lastPath_ = DenoiserPath::zeroCopy;
        ++executeCount_;
        log::debug("Denoiser zero-copy execute finished");
        return true;
    }
#endif
    std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
    lastPath_ = DenoiserPath::passthrough;
    ++executeCount_;
    log::warn("Denoiser has no executable filter; passed beauty through");
    return true;
}

} // namespace dayo::core
