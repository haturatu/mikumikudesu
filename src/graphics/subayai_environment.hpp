#pragma once

#include "graphics/device.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace dayo::graphics {

// Persistent host-side environment state: cubemap, prefiltered mips, SH and
// Skywalker parameters. Regeneration runs only when the descriptor changes so
// an unchanged environment costs no GPU work. RT-incapable GPUs keep Preview.
struct EnvironmentDesc {
    std::string source;
    float exposure{1.0F};
    std::uint64_t version{0};

    [[nodiscard]] bool operator==(const EnvironmentDesc& other) const noexcept {
        return source == other.source && exposure == other.exposure && version == other.version;
    }
    [[nodiscard]] bool operator!=(const EnvironmentDesc& other) const noexcept {
        return !(*this == other);
    }
};

class IEnvironmentBackend {
  public:
    virtual ~IEnvironmentBackend() = default;
    virtual void regenerate(const EnvironmentDesc& desc) = 0;
};

class EnvironmentService {
  public:
    explicit EnvironmentService(IEnvironmentBackend* backend) : backend_(backend) {}

    // Returns true when regeneration ran, false when the cached environment
    // was reused.
    bool update(const EnvironmentDesc& desc);

    [[nodiscard]] bool ready() const noexcept {
        return ready_;
    }
    [[nodiscard]] const EnvironmentDesc& cached() const noexcept {
        return cached_;
    }
    [[nodiscard]] std::uint64_t generationCount() const noexcept {
        return generations_;
    }
    [[nodiscard]] TextureHandle cubemap() const noexcept {
        return cubemap_;
    }
    [[nodiscard]] TextureHandle prefilteredMips() const noexcept {
        return prefiltered_;
    }
    [[nodiscard]] const std::array<float, 27>& sphericalHarmonics() const noexcept {
        return sphericalHarmonics_;
    }
    [[nodiscard]] std::uint64_t skywalkerVersion() const noexcept {
        return skywalkerVersion_;
    }

    // Test/host hook: publish the GPU handles produced by regeneration.
    void setHandles(TextureHandle cubemap, TextureHandle prefiltered, std::uint64_t skywalkerVersion) noexcept;

  private:
    IEnvironmentBackend* backend_{nullptr};
    EnvironmentDesc cached_;
    bool ready_{false};
    std::uint64_t generations_{0};
    TextureHandle cubemap_{0};
    TextureHandle prefiltered_{0};
    std::array<float, 27> sphericalHarmonics_{};
    std::uint64_t skywalkerVersion_{0};
};

} // namespace dayo::graphics
