#pragma once

#include "fx/fx_shader_compiler.hpp"
#include "graphics/native_scene_resource_runtime.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
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

enum class DayoEnvironmentPass : std::uint8_t {
    skyLuminance,
    skyLuminanceRow,
    skyWalkin,
    skyWalkinRow,
    shComboX,
    shComboY,
};

struct DayoEnvironmentDispatch {
    DayoEnvironmentPass pass{};
    std::array<std::uint32_t, 3> groups{};
};

[[nodiscard]] std::vector<DayoEnvironmentDispatch> buildDayoEnvironmentDispatchPlan(Extent3D extent,
                                                                                    bool buildSkyboxSampler);

// Executes the pinned MikuMikuDayo 1.30 system/skyboxPDF.hlsl and
// system/skyboxSH.hlsl passes. It owns the output/work buffers and borrows the
// environment texture produced by NativeEnvironmentBackend.
class NativeDayoEnvironmentRuntime {
  public:
    NativeDayoEnvironmentRuntime() = default;
    ~NativeDayoEnvironmentRuntime();

    NativeDayoEnvironmentRuntime(const NativeDayoEnvironmentRuntime&) = delete;
    NativeDayoEnvironmentRuntime& operator=(const NativeDayoEnvironmentRuntime&) = delete;

    [[nodiscard]] bool sync(Device& device, CommandList& commands, handles::TextureHandle skybox, Extent3D extent,
                            const std::filesystem::path& source, std::uint64_t sourceVersion,
                            const std::filesystem::path& hlslDirectory, bool buildSkyboxSampler,
                            std::string* error = nullptr);
    void apply(NativeSceneResourceBindings& bindings) const noexcept;
    void reset() noexcept;

    [[nodiscard]] bool ready() const noexcept {
        return device_ != nullptr && skybox_.valid() && resources_.skywalker.valid() &&
               resources_.skywalkerRow.valid() && resources_.skyboxSh.valid();
    }
    [[nodiscard]] handles::TextureHandle skybox() const noexcept {
        return skybox_;
    }
    [[nodiscard]] handles::BufferHandle skywalker() const noexcept {
        return resources_.skywalker;
    }
    [[nodiscard]] handles::BufferHandle skywalkerRow() const noexcept {
        return resources_.skywalkerRow;
    }
    [[nodiscard]] handles::BufferHandle skyboxSh() const noexcept {
        return resources_.skyboxSh;
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

  private:
    struct Resources {
        handles::BufferHandle skywalker{};
        handles::BufferHandle skywalkerRow{};
        handles::BufferHandle skyboxSh{};
        handles::BufferHandle worker{};
        handles::BufferHandle workerRow{};
        handles::BufferHandle skyLum{};
        handles::BufferHandle skyLumRow{};
        handles::BufferHandle skyboxShX{};
        handles::DescriptorSetHandle pdfSet{};
        handles::DescriptorSetHandle shSet{};
    };

    [[nodiscard]] bool ensurePipelines(Device& device, const std::filesystem::path& hlslDirectory, std::string* error);
    [[nodiscard]] bool createResources(Device& device, handles::TextureHandle skybox, Extent3D extent,
                                       bool buildSkyboxSampler, Resources& result, std::string* error);
    void destroyResources(Device* device, Resources& resources) noexcept;
    void destroyPipelines(Device* device) noexcept;
    void setError(std::string* error, std::string value) const;

    Device* device_{};
    handles::TextureHandle skybox_{};
    Resources resources_;
    handles::DescriptorSetLayoutHandle descriptorLayout_{};
    handles::PipelineLayoutHandle pipelineLayout_{};
    std::array<handles::ShaderHandle, 6> shaders_{};
    std::array<handles::PipelineHandle, 6> pipelines_{};
    std::filesystem::path source_;
    std::filesystem::path hlslDirectory_;
    Extent3D extent_{};
    std::uint64_t sourceVersion_{};
    bool buildSkyboxSampler_{};
    std::uint64_t generation_{};
};

} // namespace dayo::graphics
