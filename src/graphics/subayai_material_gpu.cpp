#include "graphics/subayai_material_gpu.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dayo::graphics {
namespace {

float scalar(const core::MaterialParameterBlock& parameters, std::string_view name, float fallback) {
    const auto* value = parameters.find(name);
    if (value == nullptr)
        return fallback;
    if (const auto* floating = std::get_if<float>(value); floating != nullptr)
        return *floating;
    if (const auto* integer = std::get_if<std::int32_t>(value); integer != nullptr)
        return static_cast<float>(*integer);
    throw std::runtime_error("Subayai GPU parameter is not scalar: " + std::string(name));
}

std::array<float, 4> vector4(const core::MaterialParameterBlock& parameters, std::string_view name,
                             std::array<float, 4> fallback) {
    const auto* value = parameters.find(name);
    if (value == nullptr)
        return fallback;
    if (const auto* vector = std::get_if<std::array<float, 4>>(value); vector != nullptr)
        return *vector;
    if (const auto* vector = std::get_if<std::array<float, 3>>(value); vector != nullptr)
        return {(*vector)[0], (*vector)[1], (*vector)[2], fallback[3]};
    if (const auto* floating = std::get_if<float>(value); floating != nullptr)
        return {*floating, *floating, *floating, fallback[3]};
    throw std::runtime_error("Subayai GPU parameter is not a float vector: " + std::string(name));
}

} // namespace

SubayaiMaterialGpu linkSubayaiMaterial(const core::MaterialParameterBlock& parameters) {
    const auto hair = core::makeSubayaiHairMaterial(parameters);
    const auto base = vector4(parameters, "BaseColor", vector4(parameters, "Diffuse", {1.0F, 1.0F, 1.0F, 1.0F}));
    const auto emission = vector4(parameters, "Emission", {});
    const auto specular = vector4(parameters, "Specular", {});

    SubayaiMaterialGpu result;
    std::copy(base.begin(), base.end(), std::begin(result.baseColor));
    std::copy(emission.begin(), emission.end(), std::begin(result.emission));
    std::copy(specular.begin(), specular.end(), std::begin(result.specular));
    result.hair[0] = hair.anisotropy;
    result.hair[1] = hair.ior[0];
    result.hair[2] = hair.ior[1];
    result.hair[3] = hair.autoNormal;
    result.surface[0] = scalar(parameters, "Roughness", 0.5F);
    result.surface[1] = scalar(parameters, "Metallic", 0.0F);
    result.surface[2] = scalar(parameters, "Transmission", 0.0F);
    return result;
}

} // namespace dayo::graphics
