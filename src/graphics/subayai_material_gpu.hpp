#pragma once

#include "core/subayai_hair.hpp"

#include <cstdint>

namespace dayo::graphics {

// Native Subayai ABI. Every field is grouped into 16-byte lanes so the
// structure can be uploaded unchanged to an HLSL StructuredBuffer. Preview's
// legacy material ABI remains separate and is not modified by this type.
struct alignas(16) SubayaiMaterialGpu {
    float baseColor[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float emission[4]{};
    float specular[4]{};
    float hair[4]{};                          // anisotropy, IOR real, IOR imaginary, AutoNormal
    float surface[4]{0.5F, 0.0F, 0.0F, 0.0F}; // roughness, metallic, transmission, reserved
    std::uint32_t flags[4]{};
};
static_assert(sizeof(SubayaiMaterialGpu) == 96);
static_assert(alignof(SubayaiMaterialGpu) == 16);

[[nodiscard]] SubayaiMaterialGpu linkSubayaiMaterial(const core::MaterialParameterBlock& parameters);

} // namespace dayo::graphics
