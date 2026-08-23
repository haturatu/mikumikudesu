#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace dayo::core {

enum class EffectPassType { rasterizer, postprocess, compute, raytracing, unknown };

struct EffectTexture {
    std::string name;
    std::string format;
    std::string view;
    float widthRatio { 1.0F };
    float heightRatio { 1.0F };
};

struct EffectSampler {
    std::string name;
    std::string filter;
    std::string addressU { "WRAP" };
    std::string addressV { "WRAP" };
};

struct EffectAttachment {
    std::string name;
    bool clear {};
};

struct EffectPass {
    std::string name;
    EffectPassType type { EffectPassType::unknown };
    std::string vertexShader;
    std::string pixelShader;
    std::string computeShader;
    std::string rayGenerationShader;
    std::vector<std::string> missShaders;
    std::vector<std::string> macros;
    std::vector<std::string> conditions;
    std::vector<EffectAttachment> renderTargets;
    std::vector<EffectAttachment> unorderedAccess;
    EffectAttachment depth;
};

struct EffectGraph {
    std::filesystem::path sourcePath;
    std::string category;
    std::vector<EffectTexture> textures;
    std::vector<EffectSampler> samplers;
    std::vector<EffectPass> passes;
    std::string generatedCode;
    std::string hlsl;
};

[[nodiscard]] EffectGraph loadEffectGraph(const std::filesystem::path& path);
[[nodiscard]] const char* toString(EffectPassType type) noexcept;

} // namespace dayo::core
