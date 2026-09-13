#pragma once

#include "core/image_hdr.hpp"
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

struct EnvironmentGpuResult {
    handles::TextureHandle cubemap{};
    handles::TextureHandle prefiltered{};
    std::array<float, 27> sphericalHarmonics{};
    std::uint64_t skywalkerVersion{};
};

struct EnvironmentPassBindings {
    handles::PipelineHandle equirectToCubePipeline{};
    handles::DescriptorSetLayoutHandle equirectToCubeLayout{};
    handles::PipelineHandle prefilterPipeline{};
    handles::DescriptorSetLayoutHandle prefilterLayout{};

    [[nodiscard]] bool valid() const noexcept {
        return equirectToCubePipeline.valid() && equirectToCubeLayout.valid() && prefilterPipeline.valid() &&
               prefilterLayout.valid();
    }
};

struct NativeEnvironmentPushConstants {
    std::uint32_t faceSize{};
    std::uint32_t mipLevels{};
    std::uint32_t reserved[2]{};
};
static_assert(sizeof(NativeEnvironmentPushConstants) == 16);

[[nodiscard]] DescriptorSetLayoutDesc nativeEnvironmentPassLayout() noexcept;

class IEnvironmentBackend {
  public:
    virtual ~IEnvironmentBackend() = default;
    virtual void regenerate(const EnvironmentDesc& desc) = 0;
    virtual EnvironmentGpuResult regenerateEx(const EnvironmentDesc& desc) {
        regenerate(desc);
        return {};
    }
    virtual void record(CommandList&) const {}
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
        return typedResult_.skywalkerVersion == 0 ? skywalkerVersion_ : typedResult_.skywalkerVersion;
    }

    // Test/host hook: publish the GPU handles produced by regeneration.
    void setHandles(TextureHandle cubemap, TextureHandle prefiltered, std::uint64_t skywalkerVersion) noexcept;
    void setGpuResult(EnvironmentGpuResult result) noexcept;
    void record(CommandList& commands) const;
    [[nodiscard]] const EnvironmentGpuResult& gpuResult() const noexcept {
        return typedResult_;
    }

  private:
    IEnvironmentBackend* backend_{nullptr};
    EnvironmentDesc cached_;
    bool ready_{false};
    std::uint64_t generations_{0};
    TextureHandle cubemap_{0};
    TextureHandle prefiltered_{0};
    std::array<float, 27> sphericalHarmonics_{};
    std::uint64_t skywalkerVersion_{0};
    EnvironmentGpuResult typedResult_;
    mutable bool recordPending_{};
};

// Typed environment backend. It owns the source/equirectangular texture and
// the generated cubemap resources; compute pipelines and their layouts remain
// renderer-owned and are supplied through EnvironmentPassBindings.
class NativeEnvironmentBackend final : public IEnvironmentBackend {
  public:
    NativeEnvironmentBackend(Device& device, EnvironmentPassBindings bindings)
        : device_(&device), bindings_(bindings) {}
    ~NativeEnvironmentBackend() override;

    void regenerate(const EnvironmentDesc& desc) override;
    [[nodiscard]] EnvironmentGpuResult regenerateEx(const EnvironmentDesc& desc) override;
    [[nodiscard]] EnvironmentGpuResult regenerateImage(const EnvironmentDesc& desc, const core::ImageData& image);
    void record(CommandList& commands) const override;
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && resources_.cubemap.valid() && resources_.prefiltered.valid() &&
               resources_.source.valid();
    }
    [[nodiscard]] const EnvironmentGpuResult& result() const noexcept {
        return result_;
    }
    [[nodiscard]] handles::TextureHandle sourceTexture() const noexcept {
        return resources_.source;
    }
    [[nodiscard]] std::uint32_t faceSize() const noexcept {
        return faceSize_;
    }

  private:
    struct Resources {
        handles::TextureHandle source{};
        handles::TextureHandle cubemap{};
        handles::TextureHandle prefiltered{};
        handles::DescriptorSetHandle equirectToCubeSet{};
        handles::DescriptorSetHandle prefilterSet{};
    };

    [[nodiscard]] EnvironmentGpuResult regenerateLinear(const EnvironmentDesc& desc,
                                                        const core::ImageData& image);
    Device* device_{};
    EnvironmentPassBindings bindings_;
    Resources resources_;
    EnvironmentGpuResult result_;
    std::uint32_t faceSize_{};
    std::uint32_t mipLevels_{};
};

} // namespace dayo::graphics
