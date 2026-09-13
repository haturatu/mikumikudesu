#pragma once

#include "graphics/device.hpp"

#include <array>
#include <cstdint>

namespace dayo::graphics {

// These descriptor sets are the stable ABI used by the upstream
// resources.hlsli file. FX-local declarations are placed after this reserved
// range so an effect cannot accidentally alias a native scene resource.
enum class NativeSceneDescriptorSet : std::uint32_t {
    frame = 0,
    textures = 1,
    vertexBuffers = 2,
    indexBuffers = 3,
    materials = 4,
    faces = 5,
    materialFaces = 6,
    faceWalkers = 7,
    previousVertices = 8,
    rawVertices = 9,
};

inline constexpr std::uint32_t kNativeSceneDescriptorSetCount = 10;
inline constexpr std::uint32_t kNativeFxResourceSet = kNativeSceneDescriptorSetCount;

enum class NativeSceneRegisterClass : std::uint8_t {
    uav,
    sampled,
    sampler,
    uniform,
};

// glslc's HLSL frontend needs disjoint binding ranges for register classes.
// Keep this mapping in one place so descriptor layouts and generated shaders
// use the same Vulkan binding convention.
[[nodiscard]] constexpr std::uint32_t nativeSceneBinding(NativeSceneRegisterClass registerClass,
                                                          std::uint32_t registerIndex) noexcept {
    constexpr std::uint32_t uavBase = 0;
    constexpr std::uint32_t sampledBase = 16;
    constexpr std::uint32_t samplerBase = 32;
    constexpr std::uint32_t uniformBase = 48;
    const auto base = registerClass == NativeSceneRegisterClass::uav
                          ? uavBase
                          : registerClass == NativeSceneRegisterClass::sampled
                                ? sampledBase
                                : registerClass == NativeSceneRegisterClass::sampler ? samplerBase : uniformBase;
    return base + registerIndex;
}

struct NativeSceneDescriptorCounts {
    std::uint32_t textures{1};
    std::uint32_t vertexBuffers{1};
    std::uint32_t indexBuffers{1};
    std::uint32_t materials{1};
    std::uint32_t faces{1};
    std::uint32_t materialFaces{1};
    std::uint32_t faceWalkers{1};
    std::uint32_t previousVertices{1};
    std::uint32_t rawVertices{1};
};

[[nodiscard]] DescriptorSetLayoutDesc
nativeSceneDescriptorLayout(NativeSceneDescriptorSet set,
                            const NativeSceneDescriptorCounts& counts = {}) noexcept;

[[nodiscard]] std::array<DescriptorSetLayoutDesc, kNativeSceneDescriptorSetCount>
nativeSceneDescriptorLayouts(const NativeSceneDescriptorCounts& counts = {}) noexcept;

[[nodiscard]] constexpr std::uint32_t nativeFxResourceSet() noexcept {
    return kNativeFxResourceSet;
}

} // namespace dayo::graphics
