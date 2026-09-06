#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

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

// Additive host runtime for OIDN. Keeps a persistent device+filter and runs
// beauty/albedo/normal -> output. Zero-copy is used only when the shared path
// is available; otherwise staging/readback -> CPU OIDN -> upload is used. If
// OIDN itself is unavailable, execute() reports an explicit passthrough.
// No CUDA premise: HIP is runtime-optional. Pass input/output compatibility
// is checked only at execution time.
enum class DenoiserPath { unavailable, zeroCopy, staging, cpu, passthrough };

struct DenoiserExecuteArgs {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::span<const float> beauty;
    std::span<const float> albedo;
    std::span<const float> normal;
    std::span<float> output;
};

class DenoiserRuntime {
  public:
    DenoiserRuntime();
    ~DenoiserRuntime();
    DenoiserRuntime(const DenoiserRuntime&) = delete;
    DenoiserRuntime& operator=(const DenoiserRuntime&) = delete;
    DenoiserRuntime(DenoiserRuntime&&) noexcept;
    DenoiserRuntime& operator=(DenoiserRuntime&&) noexcept;

    // (Re)creates the persistent device+filter when dimensions change.
    // Returns false when denoising is unavailable (CPU fallback path).
    bool ensure(std::uint32_t width, std::uint32_t height);
    // Validates oidnPass input/output compatibility at execution time only.
    bool execute(const DenoiserExecuteArgs& args);

    [[nodiscard]] bool available() const noexcept {
        return available_;
    }
    [[nodiscard]] DenoiserPath lastPath() const noexcept {
        return lastPath_;
    }
    [[nodiscard]] bool usedFallback() const noexcept {
        return lastPath_ != DenoiserPath::zeroCopy;
    }
    [[nodiscard]] bool denoised() const noexcept {
        return lastPath_ == DenoiserPath::zeroCopy || lastPath_ == DenoiserPath::staging ||
               lastPath_ == DenoiserPath::cpu;
    }
    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }
    [[nodiscard]] std::uint64_t executeCount() const noexcept {
        return executeCount_;
    }
    // Test/host hooks. shareable=false forces the staging/readback fallback;
    // forceFallback=true forces the CPU copy path even when OIDN is ready.
    void setShareable(bool shareable) noexcept {
        shareable_ = shareable;
    }
    void setForceFallback(bool force) noexcept {
        forceFallback_ = force;
    }

  private:
    void release() noexcept;

    std::uint32_t width_{0};
    std::uint32_t height_{0};
    bool initialized_{false};
    bool available_{false};
    bool shareable_{true};
    bool forceFallback_{false};
    DenoiserPath lastPath_{DenoiserPath::unavailable};
    std::uint64_t executeCount_{0};
    DenoiserStatus status_;
    std::vector<float> staging_;
    std::vector<float> stagingAlbedo_;
    std::vector<float> stagingNormal_;
    std::vector<float> stagingOutput_;
#if DAYO_HAS_OIDN
    bool ensureCpuFilter();
    bool executeOidn(void* device, void* filter, std::span<const float> beauty, std::span<const float> albedo,
                     std::span<const float> normal, std::span<float> output);
    void* device_{nullptr};
    void* filter_{nullptr};
    void* cpuDevice_{nullptr};
    void* cpuFilter_{nullptr};
#endif
};

} // namespace dayo::core
