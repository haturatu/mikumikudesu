#pragma once

#include "core/image_hdr.hpp"
#include "graphics/native_scene_resource_runtime.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace dayo::graphics {

// Exact StructuredBuffer element from MikuMikuDayo 1.30 yrztypes.hlsli.
struct DayoWalkerAlias {
    std::uint32_t pair{UINT32_MAX};
    float probability{1.0F};
    float pdf{1.0F};
};
static_assert(sizeof(DayoWalkerAlias) == 12);

// Exact StructuredBuffer element from MikuMikuDayo 1.30 yrztypes.hlsli.
struct alignas(16) DayoSphericalHarmonics {
    std::array<std::array<float, 4>, 9> coefficients{};
};
static_assert(sizeof(DayoSphericalHarmonics) == 144);

struct DayoEnvironmentCpuResources {
    std::vector<DayoWalkerAlias> skywalker;
    std::vector<DayoWalkerAlias> skywalkerRow;
    DayoSphericalHarmonics skyboxSh;
};

// CPU oracle helpers follow the pinned 1.30 system/skyboxPDF.hlsl and
// system/skyboxSH.hlsl algorithms. Input pixels must already be linear.
[[nodiscard]] DayoEnvironmentCpuResources buildDayoEnvironmentCpuResources(const core::ImageData& linearImage,
                                                                           bool buildSkyboxSampler);

// Owns the GPU storage buffers backing the canonical Dayo::Skybox,
// Skywalker, SkywalkerRow, and SkyboxSH host resources. The Skybox texture
// remains owned by NativeEnvironmentBackend and is only borrowed here.
class NativeDayoEnvironmentRuntime {
  public:
    NativeDayoEnvironmentRuntime() = default;
    ~NativeDayoEnvironmentRuntime();

    NativeDayoEnvironmentRuntime(const NativeDayoEnvironmentRuntime&) = delete;
    NativeDayoEnvironmentRuntime& operator=(const NativeDayoEnvironmentRuntime&) = delete;

    [[nodiscard]] bool sync(Device& device, handles::TextureHandle skybox, const std::filesystem::path& source,
                            std::uint64_t sourceVersion, bool buildSkyboxSampler, std::string* error = nullptr);
    void apply(NativeSceneResourceBindings& bindings) const noexcept;
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && skybox_.valid() && skywalker_.valid() && skywalkerRow_.valid() &&
               skyboxSh_.valid();
    }
    [[nodiscard]] handles::TextureHandle skybox() const noexcept {
        return skybox_;
    }
    [[nodiscard]] handles::BufferHandle skywalker() const noexcept {
        return skywalker_;
    }
    [[nodiscard]] handles::BufferHandle skywalkerRow() const noexcept {
        return skywalkerRow_;
    }
    [[nodiscard]] handles::BufferHandle skyboxSh() const noexcept {
        return skyboxSh_;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

  private:
    Device* device_{};
    handles::TextureHandle skybox_{};
    handles::BufferHandle skywalker_{};
    handles::BufferHandle skywalkerRow_{};
    handles::BufferHandle skyboxSh_{};
    std::filesystem::path source_;
    std::uint64_t sourceVersion_{};
    bool buildSkyboxSampler_{};
    std::uint64_t generation_{};
};

} // namespace dayo::graphics
