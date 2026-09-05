#pragma once

#include "core/effect.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace dayo::core {

enum class SubayaiParameterType { integer, floating, texture };

struct SubayaiParameterDefinition {
    SubayaiParameterType type{SubayaiParameterType::floating};
    std::uint8_t components{1};
};

struct SubayaiMaterialSchema {
    std::unordered_map<std::string, SubayaiParameterDefinition> parameters;
    std::unordered_map<std::string, std::unordered_map<std::string, std::int32_t>> enumerations;
    MaterialParameterBlock defaults;
};

struct SubayaiMaterialPreset {
    std::filesystem::path sourcePath;
    MaterialParameterBlock parameters;
};

[[nodiscard]] SubayaiMaterialSchema loadSubayaiMaterialSchema(const std::filesystem::path& path);

[[nodiscard]] SubayaiMaterialPreset loadSubayaiMaterialPreset(const std::filesystem::path& path,
                                                              const SubayaiMaterialSchema& schema);

[[nodiscard]] SubayaiMaterialPreset loadSubayaiMaterialPreset(const std::filesystem::path& path);

} // namespace dayo::core
