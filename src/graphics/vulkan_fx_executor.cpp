#include "graphics/fx_executor.hpp"

#include "core/log.hpp"

namespace dayo::graphics {

VulkanFxExecutor::Stats VulkanFxExecutor::execute(const dayo::fx::FxFramePlan& plan, CommandList& commands,
                                                  const dayo::fx::FxFrameContext& context,
                                                  const FxExecutionResources& resources) const {
    Stats stats;
    dayo::log::info("VulkanFxExecutor executing ", plan.ordered.size(), " passes (", context.renderWidth, "x",
                    context.renderHeight, ")");
    const auto resolve = [&](const dayo::fx::FxDispatch::ResourceUse& resource) -> TextureHandle {
        if (resource.name.empty() || !resources.resolveTexture)
            throw std::logic_error("VulkanFxExecutor: pass resource has no backend binding: " + resource.name);
        const auto handle = resources.resolveTexture(resource.name);
        if (!handle.has_value())
            throw std::logic_error("VulkanFxExecutor: pass resource is unavailable: " + resource.name);
        return *handle;
    };
    for (const auto& dispatch : plan.ordered) {
        dayo::log::debug("VulkanFxExecutor pass ", dispatch.name, " kind ", dayo::fx::toString(dispatch.kind));
        switch (dispatch.kind) {
        case dayo::fx::FxOpKind::raster:
            commands.draw(static_cast<std::uint32_t>(context.clonedVertexCount), context.cloneCount);
            ++stats.raster;
            break;
        case dayo::fx::FxOpKind::postprocess:
            commands.draw(3, 1);
            ++stats.postprocess;
            break;
        case dayo::fx::FxOpKind::compute:
            commands.dispatch((context.renderWidth + 7U) / 8U, (context.renderHeight + 7U) / 8U, 1);
            ++stats.compute;
            break;
        case dayo::fx::FxOpKind::copy:
            if (dispatch.resources.size() < 2 || dispatch.resources[0].write || !dispatch.resources[1].write)
                throw std::logic_error("VulkanFxExecutor: copy pass requires read source and write destination");
            commands.copyTexture(resolve(dispatch.resources[0]), resolve(dispatch.resources[1]));
            ++stats.copy;
            break;
        case dayo::fx::FxOpKind::clear:
            if (dispatch.resources.size() != 1 || !dispatch.resources[0].write)
                throw std::logic_error("VulkanFxExecutor: clear pass requires one write target");
            commands.clearTexture(resolve(dispatch.resources[0]));
            ++stats.clear;
            break;
        case dayo::fx::FxOpKind::mipmap:
            if (dispatch.resources.size() != 1)
                throw std::logic_error("VulkanFxExecutor: mipmap pass requires one target");
            commands.generateMipmaps(resolve(dispatch.resources[0]));
            ++stats.mipmap;
            break;
        case dayo::fx::FxOpKind::raytracing:
            dayo::log::error("VulkanFxExecutor raytracing unsupported in pass ", dispatch.name);
            throw FxRaytracingUnsupported(dispatch.name);
        }
    }
    static_cast<void>(device_);
    return stats;
}

} // namespace dayo::graphics
