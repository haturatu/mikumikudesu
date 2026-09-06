#include "graphics/fx_executor.hpp"

#include "core/log.hpp"

namespace dayo::graphics {

VulkanFxExecutor::Stats VulkanFxExecutor::execute(const dayo::fx::FxFramePlan& plan, CommandList& commands,
                                                  const dayo::fx::FxFrameContext& context) const {
    Stats stats;
    dayo::log::info("VulkanFxExecutor executing ", plan.ordered.size(), " passes (", context.renderWidth, "x",
                    context.renderHeight, ")");
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
            // Skeleton handle: backend copy path. Real pipelines resolve the
            // source/destination views from the frame plan; here we forward
            // the call so mock backends can trace ordering.
            commands.copyTexture(TextureHandle{}, TextureHandle{});
            ++stats.copy;
            break;
        case dayo::fx::FxOpKind::clear:
            commands.clearTexture(TextureHandle{});
            ++stats.clear;
            break;
        case dayo::fx::FxOpKind::mipmap:
            commands.generateMipmaps(TextureHandle{});
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
