#include "core/denoiser.hpp"
#include "core/log.hpp"

#include <algorithm>
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
      executeCount_(other.executeCount_), status_(std::move(other.status_)), staging_(std::move(other.staging_)) {
#if DAYO_HAS_OIDN
    device_ = std::exchange(other.device_, nullptr);
    filter_ = std::exchange(other.filter_, nullptr);
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
#if DAYO_HAS_OIDN
    device_ = std::exchange(other.device_, nullptr);
    filter_ = std::exchange(other.filter_, nullptr);
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
    if (device_ != nullptr) {
        oidnReleaseDevice(static_cast<OIDNDevice>(device_));
        device_ = nullptr;
    }
#endif
    initialized_ = false;
    available_ = false;
}

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
    const std::size_t pixels = static_cast<std::size_t>(args.width) * static_cast<std::size_t>(args.height);
    const std::size_t samples = pixels * 3U;
    if (args.width == 0 || args.height == 0) {
        log::error("Denoiser execute rejected empty extent");
        return false;
    }
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
    const bool fallback = !available_ || forceFallback_;
    if (fallback) {
        std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
        lastPath_ = DenoiserPath::cpu;
        ++executeCount_;
        log::debug("Denoiser CPU fallback copied beauty");
        return true;
    }
    if (!shareable_) {
        staging_.assign(args.beauty.begin(), args.beauty.end());
        std::copy(staging_.begin(), staging_.end(), args.output.begin());
        lastPath_ = DenoiserPath::staging;
        ++executeCount_;
        log::debug("Denoiser staging/readback fallback copied beauty");
        return true;
    }
#if DAYO_HAS_OIDN
    if (filter_ != nullptr) {
        auto* filter = static_cast<OIDNFilter>(filter_);
        oidnSetSharedFilterImage(filter, "color", const_cast<float*>(args.beauty.data()), OIDN_FORMAT_FLOAT3,
                                 args.width, args.height, 0, 0, 0);
        if (!args.albedo.empty()) {
            oidnSetSharedFilterImage(filter, "albedo", const_cast<float*>(args.albedo.data()), OIDN_FORMAT_FLOAT3,
                                     args.width, args.height, 0, 0, 0);
        }
        if (!args.normal.empty()) {
            oidnSetSharedFilterImage(filter, "normal", const_cast<float*>(args.normal.data()), OIDN_FORMAT_FLOAT3,
                                     args.width, args.height, 0, 0, 0);
        }
        oidnSetSharedFilterImage(filter, "output", args.output.data(), OIDN_FORMAT_FLOAT3, args.width, args.height, 0,
                                 0, 0);
        oidnCommitFilter(filter);
        const char* message = nullptr;
        if (oidnGetDeviceError(static_cast<OIDNDevice>(device_), &message) != OIDN_ERROR_NONE) {
            std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
            lastPath_ = DenoiserPath::staging;
            ++executeCount_;
            log::warn("Denoiser commit failed; staging fallback copied beauty");
            return true;
        }
        oidnExecuteFilter(filter);
        if (oidnGetDeviceError(static_cast<OIDNDevice>(device_), &message) != OIDN_ERROR_NONE) {
            std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
            lastPath_ = DenoiserPath::staging;
            ++executeCount_;
            log::warn("Denoiser execute failed; staging fallback copied beauty");
            return true;
        }
        lastPath_ = DenoiserPath::zeroCopy;
        ++executeCount_;
        log::debug("Denoiser zero-copy execute finished");
        return true;
    }
#endif
    std::copy(args.beauty.begin(), args.beauty.end(), args.output.begin());
    lastPath_ = DenoiserPath::zeroCopy;
    ++executeCount_;
    log::debug("Denoiser shared execute copied beauty");
    return true;
}

} // namespace dayo::core
