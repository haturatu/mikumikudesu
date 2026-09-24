#include "core/fx_debug.hpp"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <string_view>

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
                                    .type = {},
                                    .sizeBase = texture.size.base,
                                    .conditions = texture.conditions,
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
                                    .type = {},
                                    .sizeBase = texture.size.base,
                                    .conditions = texture.conditions,
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
                                    .type = buffer.type,
                                    .sizeBase = buffer.size.base,
                                    .conditions = buffer.conditions,
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
                                    .type = {},
                                    .sizeBase = {},
                                    .conditions = {},
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
        const auto addAccess = [&debugPass](std::string_view name, bool write) {
            if (name.empty() || std::ranges::any_of(debugPass.resources, [name, write](const auto& access) {
                    return access.name == name && access.write == write;
                }))
                return;
            debugPass.resources.push_back({.name = std::string(name), .write = write});
        };
        for (const auto& input : pass.inputs)
            addAccess(input.name, false);
        for (const auto& target : pass.renderTargets)
            addAccess(target.name, true);
        for (const auto& target : pass.unorderedAccess)
            addAccess(target.name, true);
        if (!pass.depth.name.empty())
            addAccess(pass.depth.name, true);
        if (!pass.rasterVertexBuffer.empty())
            addAccess(pass.rasterVertexBuffer, false);
        if (!pass.rasterIndexBuffer.empty())
            addAccess(pass.rasterIndexBuffer, false);
        if (!pass.functional.source.empty())
            addAccess(pass.functional.source, false);
        if (!pass.functional.destination.empty())
            addAccess(pass.functional.destination, true);
        if (!pass.functional.target.empty())
            addAccess(pass.functional.target, true);
        if (!pass.oidnInput.empty())
            addAccess(pass.oidnInput, false);
        if (!pass.oidnAlbedo.empty())
            addAccess(pass.oidnAlbedo, false);
        if (!pass.oidnNormal.empty())
            addAccess(pass.oidnNormal, false);
        if (!pass.oidnOutput.empty())
            addAccess(pass.oidnOutput, true);
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
