#include "core/fx_debug.hpp"

#include <sstream>

namespace dayo::core {
namespace {

const char* functionalKindName(EffectFunctionalPassKind kind) noexcept {
    switch (kind) {
    case EffectFunctionalPassKind::none:
        return "none";
    case EffectFunctionalPassKind::copy:
        return "copy";
    case EffectFunctionalPassKind::clearRtv:
        return "clearRTV";
    case EffectFunctionalPassKind::clearUav:
        return "clearUAV";
    case EffectFunctionalPassKind::mipmapGen:
        return "mipmapGen";
    }
    return "unknown";
}

} // namespace

FxRuntimeDebugSnapshot FxRuntimeInspector::snapshot(const EffectGraph& graph, std::uint64_t frame) {
    FxRuntimeDebugSnapshot result;
    result.frame = frame;
    result.passCount = static_cast<std::uint32_t>(graph.passes.size());
    result.materialCount = graph.materialDescriptor.has_value() ? 1U : 0U;
    result.backend = "source-graph";
    result.globalVarSize = graph.globalVarSize;
    result.memos = graph.memos;
    result.passNames.reserve(graph.passes.size());
    result.passes.reserve(graph.passes.size());
    for (const auto& pass : graph.passes)
        result.passNames.push_back(pass.name);
    for (const auto& texture : graph.textures) {
        result.resources.push_back({.name = texture.name,
                                    .kind = "Texture2D",
                                    .format = texture.format,
                                    .view = texture.view,
                                    .shared = texture.shared,
                                    .filename = texture.filename,
                                    .sizeBase = texture.size.base,
                                    .width = texture.size.width,
                                    .height = texture.size.height,
                                    .depth = 1,
                                    .absoluteSize = texture.size.absolute,
                                    .mipmapped = texture.mipmap});
    }
    for (const auto& texture : graph.textures3D) {
        result.resources.push_back({.name = texture.name,
                                    .kind = "Texture3D",
                                    .format = texture.format,
                                    .view = texture.view,
                                    .shared = texture.shared,
                                    .filename = texture.filename,
                                    .sizeBase = texture.size.base,
                                    .width = texture.size.width,
                                    .height = texture.size.height,
                                    .depth = texture.size.depth,
                                    .absoluteSize = texture.size.absolute,
                                    .mipmapped = texture.mipmap});
    }
    for (const auto& buffer : graph.buffers) {
        result.resources.push_back({.name = buffer.name,
                                    .kind = "Buffer",
                                    .format = buffer.format,
                                    .view = buffer.view,
                                    .shared = buffer.shared,
                                    .filename = {},
                                    .sizeBase = buffer.size.base,
                                    .width = buffer.size.width,
                                    .height = 1,
                                    .depth = 1,
                                    .elementSize = buffer.elementSize,
                                    .absoluteSize = buffer.size.absolute});
    }
    for (const auto& sampler : graph.samplers) {
        result.resources.push_back({.name = sampler.name,
                                    .kind = "Sampler",
                                    .format = sampler.filter,
                                    .view = sampler.addressU + "," + sampler.addressV + "," + sampler.addressW,
                                    .shared = {},
                                    .filename = {},
                                    .sizeBase = {},
                                    .width = 0,
                                    .height = 1,
                                    .depth = 1,
                                    .elementSize = 0,
                                    .absoluteSize = false,
                                    .mipmapped = false});
    }
    for (const auto& pass : graph.passes) {
        FxDebugPassSnapshot debugPass{.name = pass.name,
                                      .type = toString(pass.type),
                                      .functionalKind = functionalKindName(pass.functionalKind),
                                      .resources = {},
                                      .conditions = pass.conditions};
        for (const auto& input : pass.inputs)
            debugPass.resources.push_back({.name = input.name, .write = false});
        for (const auto& target : pass.renderTargets)
            debugPass.resources.push_back({.name = target.name, .write = true});
        for (const auto& target : pass.unorderedAccess)
            debugPass.resources.push_back({.name = target.name, .write = true});
        if (!pass.depth.name.empty())
            debugPass.resources.push_back({.name = pass.depth.name, .write = true});
        if (!pass.rasterVertexBuffer.empty())
            debugPass.resources.push_back({.name = pass.rasterVertexBuffer, .write = false});
        if (!pass.rasterIndexBuffer.empty())
            debugPass.resources.push_back({.name = pass.rasterIndexBuffer, .write = false});
        if (!pass.functional.source.empty())
            debugPass.resources.push_back({.name = pass.functional.source, .write = false});
        if (!pass.functional.destination.empty())
            debugPass.resources.push_back({.name = pass.functional.destination, .write = true});
        if (!pass.functional.target.empty())
            debugPass.resources.push_back({.name = pass.functional.target, .write = true});
        if (!pass.oidnInput.empty())
            debugPass.resources.push_back({.name = pass.oidnInput, .write = false});
        if (!pass.oidnAlbedo.empty())
            debugPass.resources.push_back({.name = pass.oidnAlbedo, .write = false});
        if (!pass.oidnNormal.empty())
            debugPass.resources.push_back({.name = pass.oidnNormal, .write = false});
        if (!pass.oidnOutput.empty())
            debugPass.resources.push_back({.name = pass.oidnOutput, .write = true});
        result.passes.push_back(std::move(debugPass));
    }
    result.controllers.reserve(graph.controllers.size());
    for (const auto& controller : graph.controllers)
        result.controllers.push_back({.name = controller.name,
                                      .controller = controller.controllerName,
                                      .item = controller.item,
                                      .type = controller.type,
                                      .description = controller.description,
                                      .slider = controller.slider});
    result.resourceCount = static_cast<std::uint32_t>(result.resources.size());
    if (graph.passes.empty())
        result.warnings.emplace_back("no passes compiled");
    return result;
}

FxRuntimeDebugSnapshot FxRuntimeInspector::empty(std::uint64_t frame) {
    FxRuntimeDebugSnapshot result;
    result.frame = frame;
    result.warnings.emplace_back("no effect bound");
    return result;
}

std::string FxRuntimeInspector::format(const FxRuntimeDebugSnapshot& snapshot) {
    std::ostringstream line;
    line << "fx frame=" << snapshot.frame << " passes=" << snapshot.passCount << " resources=" << snapshot.resourceCount
         << " materials=" << snapshot.materialCount << " backend=" << snapshot.backend;
    return line.str();
}

} // namespace dayo::core
