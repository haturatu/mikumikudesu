#include "core/subayai_hair.hpp"

#include <stdexcept>
#include <string>

namespace dayo::core {
namespace {

float scalar(const MaterialParameterBlock& parameters, std::string_view name, float fallback) {
    const auto* value = parameters.find(name);
    if (value == nullptr)
        return fallback;
    if (const auto* floating = std::get_if<float>(value); floating != nullptr)
        return *floating;
    if (const auto* integer = std::get_if<std::int32_t>(value); integer != nullptr)
        return static_cast<float>(*integer);
    throw std::runtime_error("Subayai hair parameter is not scalar: " + std::string(name));
}

std::array<float, 2> ior(const MaterialParameterBlock& parameters) {
    const auto* value = parameters.find("IOR");
    if (value == nullptr)
        return {1.5F, 0.0F};
    if (const auto* pair = std::get_if<std::array<float, 2>>(value); pair != nullptr)
        return *pair;
    if (const auto* floating = std::get_if<float>(value); floating != nullptr)
        return {*floating, 0.0F};
    throw std::runtime_error("Subayai hair parameter is not a two-component IOR");
}

} // namespace

SubayaiHairMaterial makeSubayaiHairMaterial(const MaterialParameterBlock& parameters) {
    return {
        .anisotropy = scalar(parameters, "Anisotropy", 0.0F),
        .ior = ior(parameters),
        .autoNormal = scalar(parameters, "AutoNormal", 0.0F),
    };
}

SubayaiHairMaterial loadSubayaiHairMaterial(const std::filesystem::path& path, const SubayaiMaterialSchema& schema) {
    return makeSubayaiHairMaterial(loadSubayaiMaterialPreset(path, schema).parameters);
}

} // namespace dayo::core
