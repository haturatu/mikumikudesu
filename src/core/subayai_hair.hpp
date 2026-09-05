#pragma once

#include "core/subayai_material.hpp"

#include <array>
#include <filesystem>

namespace dayo::core {

// Renderer-neutral values consumed by a future native Subayai hair pass.
// They intentionally mirror the bundled hair.txt annotation instead of
// changing Preview's original MMD shading contract.
struct SubayaiHairMaterial {
    float anisotropy{0.0F};
    std::array<float, 2> ior{1.5F, 0.0F};
    float autoNormal{0.0F};
};

[[nodiscard]] SubayaiHairMaterial makeSubayaiHairMaterial(const MaterialParameterBlock& parameters);

[[nodiscard]] SubayaiHairMaterial loadSubayaiHairMaterial(const std::filesystem::path& path,
                                                          const SubayaiMaterialSchema& schema);

} // namespace dayo::core
