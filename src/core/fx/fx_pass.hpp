#pragma once

#include "core/effect.hpp"
#include "core/fx/fx_condition.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dayo::core::fx {

// Category (when a pass runs) is orthogonal to the op (what it does).
// EffectPassType stays untouched; this layer converts to/from it.
enum class FxCategory { deform, render, postprocess };

enum class RasterModelTarget { all, self, other, buffer };

struct FxRasterOp {
    std::string vertexShader;
    std::string pixelShader;
    RasterModelTarget target{RasterModelTarget::all};
    std::vector<std::string> renderTargets;
    std::string depth;
};

struct FxPostProcessOp {
    std::string pixelShader;
    std::vector<std::string> inputs;
    std::string output;
};

struct FxComputeOp {
    std::string computeShader;
    std::vector<std::string> resources;
};

struct FxRayTracingOp {
    std::string rayGenerationShader;
    std::vector<std::string> missShaders;
    std::uint32_t maxPayloadSize{};
    std::uint32_t maxAttributeSize{};
    std::uint32_t maxRecursionDepth{1};
};

struct FxCopyOp {
    std::string source;
    std::string destination;
};

struct FxClearRtvOp {
    std::string target;
    bool clear{true};
};

struct FxClearUavOp {
    std::string target;
};

struct FxMipmapGenOp {
    std::string texture;
};

struct FxOidnOp {
    std::string input;
    std::string output;
};

using FxPassOp = std::variant<FxRasterOp, FxPostProcessOp, FxComputeOp, FxRayTracingOp, FxCopyOp, FxClearRtvOp,
                              FxClearUavOp, FxMipmapGenOp, FxOidnOp>;

struct FxPass {
    std::string name;
    FxCategory category{FxCategory::render};
    FxPassOp op{FxRasterOp{}};
    std::vector<std::string> conditions;
    FxEventMask eventMask{kFxEventNone};
};

[[nodiscard]] const char* toString(FxCategory category) noexcept;
[[nodiscard]] FxCategory fxCategoryFromString(std::string_view name);
[[nodiscard]] const char* toString(RasterModelTarget target) noexcept;
// Resolves model-target semantics ("all", "self", "other", "buffer",
// case-insensitive). Throws std::runtime_error on unknown semantics.
[[nodiscard]] RasterModelTarget resolveRasterModelTarget(std::string_view semantic);

// EffectPassType interop: raster ops map to rasterizer, postprocess ops to
// postprocess, compute to compute, and raytracing to raytracing. Utility ops
// have no legacy equivalent and are rejected when converted back.
[[nodiscard]] EffectPassType toEffectPassType(const FxPassOp& op) noexcept;
[[nodiscard]] FxPassOp fxPassOpFromEffectPassType(EffectPassType type);
[[nodiscard]] const char* fxPassOpTypeName(const FxPassOp& op) noexcept;
[[nodiscard]] FxCategory defaultCategoryForOp(const FxPassOp& op) noexcept;

// EffectPass interop. Category must be supplied explicitly so the legacy
// graph category string never silently becomes a new category.
[[nodiscard]] FxPass fxPassFromEffectPass(const EffectPass& pass, FxCategory category);
[[nodiscard]] FxPass fxPassFromEffectPass(const EffectPass& pass, std::string_view categoryName);
[[nodiscard]] EffectPass effectPassFromFxPass(const FxPass& pass);

} // namespace dayo::core::fx
