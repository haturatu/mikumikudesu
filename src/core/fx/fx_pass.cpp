#include "core/fx/fx_pass.hpp"

#include "core/log.hpp"

#include <cctype>
#include <stdexcept>

namespace dayo::core::fx {
namespace {

std::string trimCopy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string lowerCopy(std::string_view value) {
    std::string out(value);
    for (auto& ch : out)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

std::string categoryKey(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : lowerCopy(trimCopy(value))) {
        if (ch == '_' || ch == '-' || ch == ' ')
            continue;
        out += ch;
    }
    return out;
}

} // namespace

const char* toString(FxCategory category) noexcept {
    switch (category) {
    case FxCategory::deform:
        return "deform";
    case FxCategory::render:
        return "render";
    case FxCategory::postprocess:
        return "postprocess";
    }
    return "render";
}

FxCategory fxCategoryFromString(std::string_view name) {
    const std::string key = categoryKey(name);
    if (key == "deform")
        return FxCategory::deform;
    if (key == "render")
        return FxCategory::render;
    if (key == "postprocess" || key == "post" || key == "postproc")
        return FxCategory::postprocess;
    log::error("fx pass: unknown category '", std::string(name), "'");
    throw std::runtime_error("unknown fx category: " + std::string(name));
}

const char* toString(RasterModelTarget target) noexcept {
    switch (target) {
    case RasterModelTarget::all:
        return "all";
    case RasterModelTarget::self:
        return "self";
    case RasterModelTarget::other:
        return "other";
    case RasterModelTarget::buffer:
        return "buffer";
    }
    return "all";
}

RasterModelTarget resolveRasterModelTarget(std::string_view semantic) {
    const std::string key = lowerCopy(trimCopy(semantic));
    if (key == "all" || key == "scene" || key == "*")
        return RasterModelTarget::all;
    if (key == "self" || key == "this")
        return RasterModelTarget::self;
    if (key == "other" || key == "others")
        return RasterModelTarget::other;
    if (key == "buffer" || key == "meshbuffer" || key == "mesh_buffer")
        return RasterModelTarget::buffer;
    log::error("fx pass: unknown raster model target '", std::string(semantic), "'");
    throw std::runtime_error("unknown raster model target: " + std::string(semantic));
}

EffectPassType toEffectPassType(const FxPassOp& op) noexcept {
    return std::visit(
        [](const auto& concrete) -> EffectPassType {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, FxRasterOp>)
                return EffectPassType::rasterizer;
            else if constexpr (std::is_same_v<T, FxPostProcessOp>)
                return EffectPassType::postprocess;
            else if constexpr (std::is_same_v<T, FxComputeOp>)
                return EffectPassType::compute;
            else if constexpr (std::is_same_v<T, FxRayTracingOp>)
                return EffectPassType::raytracing;
            else
                return EffectPassType::unknown;
        },
        op);
}

FxPassOp fxPassOpFromEffectPassType(EffectPassType type) {
    switch (type) {
    case EffectPassType::rasterizer:
        return FxPassOp{FxRasterOp{}};
    case EffectPassType::postprocess:
        return FxPassOp{FxPostProcessOp{}};
    case EffectPassType::compute:
        return FxPassOp{FxComputeOp{}};
    case EffectPassType::raytracing:
        return FxPassOp{FxRayTracingOp{}};
    case EffectPassType::unknown:
        throw std::runtime_error("unsupported unknown FX pass type");
    }
    throw std::runtime_error("unsupported FX pass type");
}

const char* fxPassOpTypeName(const FxPassOp& op) noexcept {
    return std::visit(
        [](const auto& concrete) -> const char* {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, FxRasterOp>)
                return "raster";
            else if constexpr (std::is_same_v<T, FxPostProcessOp>)
                return "postprocess";
            else if constexpr (std::is_same_v<T, FxComputeOp>)
                return "compute";
            else if constexpr (std::is_same_v<T, FxRayTracingOp>)
                return "raytracing";
            else if constexpr (std::is_same_v<T, FxCopyOp>)
                return "copy";
            else if constexpr (std::is_same_v<T, FxClearRtvOp>)
                return "clearRtv";
            else if constexpr (std::is_same_v<T, FxClearUavOp>)
                return "clearUav";
            else if constexpr (std::is_same_v<T, FxMipmapGenOp>)
                return "mipmapGen";
            else if constexpr (std::is_same_v<T, FxOidnOp>)
                return "oidn";
            else
                return "unknown";
        },
        op);
}

FxCategory defaultCategoryForOp(const FxPassOp& op) noexcept {
    return std::visit(
        [](const auto& concrete) -> FxCategory {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, FxPostProcessOp> || std::is_same_v<T, FxMipmapGenOp> ||
                          std::is_same_v<T, FxOidnOp>)
                return FxCategory::postprocess;
            else
                return FxCategory::render;
        },
        op);
}

FxPass fxPassFromEffectPass(const EffectPass& pass, FxCategory category) {
    FxPass out;
    out.name = pass.name;
    out.category = category;
    out.conditions = pass.conditions;
    switch (pass.type) {
    case EffectPassType::rasterizer: {
        FxRasterOp op;
        op.vertexShader = pass.vertexShader;
        op.pixelShader = pass.pixelShader;
        op.target = RasterModelTarget::all;
        for (const auto& target : pass.renderTargets)
            op.renderTargets.push_back(target.name);
        op.depth = pass.depth.name;
        out.op = std::move(op);
        break;
    }
    case EffectPassType::postprocess: {
        FxPostProcessOp op;
        op.pixelShader = pass.pixelShader;
        for (const auto& target : pass.renderTargets)
            op.inputs.push_back(target.name);
        if (!pass.renderTargets.empty())
            op.output = pass.renderTargets.front().name;
        out.op = std::move(op);
        break;
    }
    case EffectPassType::compute: {
        FxComputeOp op;
        op.computeShader = pass.computeShader;
        for (const auto& target : pass.unorderedAccess)
            op.resources.push_back(target.name);
        out.op = std::move(op);
        break;
    }
    case EffectPassType::raytracing: {
        FxRayTracingOp op;
        op.rayGenerationShader = pass.rayGenerationShader;
        op.missShaders = pass.missShaders;
        op.maxPayloadSize = pass.maxPayloadSize;
        op.maxAttributeSize = pass.maxAttributeSize;
        op.maxRecursionDepth = pass.maxRecursionDepth;
        out.op = std::move(op);
        break;
    }
    case EffectPassType::unknown:
        throw std::runtime_error("unsupported unknown FX pass type: " + pass.name);
    }
    log::debug("fx pass '", out.name, "' category=", toString(out.category), " op=", fxPassOpTypeName(out.op));
    return out;
}

FxPass fxPassFromEffectPass(const EffectPass& pass, std::string_view categoryName) {
    return fxPassFromEffectPass(pass, fxCategoryFromString(categoryName));
}

EffectPass effectPassFromFxPass(const FxPass& pass) {
    const bool legacyEquivalent = std::visit(
        [](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            return std::is_same_v<T, FxRasterOp> || std::is_same_v<T, FxPostProcessOp> ||
                   std::is_same_v<T, FxComputeOp> || std::is_same_v<T, FxRayTracingOp>;
        },
        pass.op);
    if (!legacyEquivalent)
        throw std::runtime_error("utility FX pass has no legacy EffectPass representation");

    EffectPass out;
    out.name = pass.name;
    out.type = toEffectPassType(pass.op);
    out.conditions = pass.conditions;
    std::visit(
        [&](const auto& concrete) {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, FxRasterOp>) {
                out.vertexShader = concrete.vertexShader;
                out.pixelShader = concrete.pixelShader;
                for (const auto& target : concrete.renderTargets)
                    out.renderTargets.push_back(EffectAttachment{.name = target, .clear = false});
                if (!concrete.depth.empty())
                    out.depth.name = concrete.depth;
            } else if constexpr (std::is_same_v<T, FxPostProcessOp>) {
                out.pixelShader = concrete.pixelShader;
                if (!concrete.output.empty())
                    out.renderTargets.push_back(EffectAttachment{.name = concrete.output, .clear = false});
            } else if constexpr (std::is_same_v<T, FxComputeOp>) {
                out.computeShader = concrete.computeShader;
                for (const auto& resource : concrete.resources)
                    out.unorderedAccess.push_back(EffectAttachment{.name = resource, .clear = false});
            } else if constexpr (std::is_same_v<T, FxRayTracingOp>) {
                out.rayGenerationShader = concrete.rayGenerationShader;
                out.missShaders = concrete.missShaders;
                out.maxPayloadSize = concrete.maxPayloadSize;
                out.maxAttributeSize = concrete.maxAttributeSize;
                out.maxRecursionDepth = concrete.maxRecursionDepth;
            }
        },
        pass.op);
    return out;
}

} // namespace dayo::core::fx
