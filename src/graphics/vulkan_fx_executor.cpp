#include "graphics/fx_executor.hpp"

#include "core/log.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace dayo::graphics {

VulkanFxExecutor::Stats VulkanFxExecutor::execute(const dayo::fx::FxFramePlan& plan, CommandList& commands,
                                                  const dayo::fx::FxFrameContext& context,
                                                  const FxExecutionResources& resources) const {
    Stats stats;
    if (device_ == nullptr)
        throw std::logic_error("VulkanFxExecutor: device is not available");
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
    const auto prepareResources = [&](const dayo::fx::FxDispatch& dispatch) {
        std::unordered_set<TextureHandle> transitioned;
        std::vector<DescriptorBinding> bindings;
        bindings.reserve(dispatch.resources.size());
        for (std::size_t index = 0; index < dispatch.resources.size(); ++index) {
            const auto& resource = dispatch.resources[index];
            const auto texture = resolve(resource);
            if (transitioned.insert(texture).second)
                commands.transition(texture);
            if (resources.resolveBinding) {
                const auto binding =
                    resources.resolveBinding(resource.name, resource.write, static_cast<std::uint32_t>(index));
                if (!binding.has_value())
                    throw std::logic_error("VulkanFxExecutor: descriptor binding is unavailable: " + resource.name);
                bindings.push_back(*binding);
            } else {
                bindings.push_back(DescriptorBinding{static_cast<std::uint32_t>(index), 0, texture, 0});
            }
        }
        if (!bindings.empty())
            commands.bindResources(std::span<const DescriptorBinding>(bindings.data(), bindings.size()));
    };
    const auto prepareShaderPass = [&](const dayo::fx::FxDispatch& dispatch) {
        if (!dispatch.conditions.empty()) {
            if (!resources.evaluateConditions)
                throw std::logic_error("VulkanFxExecutor: pass conditions have no evaluator: " + dispatch.name);
            if (!resources.evaluateConditions(
                    std::span<const std::string>(dispatch.conditions.data(), dispatch.conditions.size()), context))
                return false;
        }
        if (!resources.resolvePipeline)
            throw std::logic_error("VulkanFxExecutor: shader pass has no pipeline resolver: " + dispatch.name);
        const auto pipeline = resources.resolvePipeline(dispatch);
        if (!pipeline.has_value())
            throw std::logic_error("VulkanFxExecutor: pipeline is unavailable: " + dispatch.name);
        prepareResources(dispatch);
        commands.bindPipeline(*pipeline);
        if (resources.makePushConstants) {
            const auto constants = resources.makePushConstants(dispatch, context);
            if (!constants.empty())
                commands.pushConstants(std::span<const std::byte>(constants.data(), constants.size()));
        }
        return true;
    };
    const auto prepareUtilityPass = [&](const dayo::fx::FxDispatch& dispatch) {
        if (!dispatch.conditions.empty()) {
            if (!resources.evaluateConditions)
                throw std::logic_error("VulkanFxExecutor: pass conditions have no evaluator: " + dispatch.name);
            if (!resources.evaluateConditions(
                    std::span<const std::string>(dispatch.conditions.data(), dispatch.conditions.size()), context))
                return false;
        }
        prepareResources(dispatch);
        return true;
    };
    for (const auto& dispatch : plan.ordered) {
        dayo::log::debug("VulkanFxExecutor pass ", dispatch.name, " kind ", dayo::fx::toString(dispatch.kind));
        switch (dispatch.kind) {
        case dayo::fx::FxOpKind::raster:
            if (!prepareShaderPass(dispatch))
                break;
            commands.draw(static_cast<std::uint32_t>(context.clonedVertexCount), context.cloneCount);
            ++stats.raster;
            break;
        case dayo::fx::FxOpKind::postprocess:
            if (!prepareShaderPass(dispatch))
                break;
            commands.draw(3, 1);
            ++stats.postprocess;
            break;
        case dayo::fx::FxOpKind::compute:
            if (!prepareShaderPass(dispatch))
                break;
            commands.dispatch((context.renderWidth + 7U) / 8U, (context.renderHeight + 7U) / 8U, 1);
            ++stats.compute;
            break;
        case dayo::fx::FxOpKind::copy:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() < 2 || dispatch.resources[0].write || !dispatch.resources[1].write)
                throw std::logic_error("VulkanFxExecutor: copy pass requires read source and write destination");
            commands.copyTexture(resolve(dispatch.resources[0]), resolve(dispatch.resources[1]));
            ++stats.copy;
            break;
        case dayo::fx::FxOpKind::clear:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() != 1 || !dispatch.resources[0].write)
                throw std::logic_error("VulkanFxExecutor: clear pass requires one write target");
            commands.clearTexture(resolve(dispatch.resources[0]));
            ++stats.clear;
            break;
        case dayo::fx::FxOpKind::mipmap:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() != 1)
                throw std::logic_error("VulkanFxExecutor: mipmap pass requires one target");
            commands.generateMipmaps(resolve(dispatch.resources[0]));
            ++stats.mipmap;
            break;
        case dayo::fx::FxOpKind::raytracing:
            if (!prepareShaderPass(dispatch))
                break;
            dayo::log::error("VulkanFxExecutor raytracing unsupported in pass ", dispatch.name);
            throw FxRaytracingUnsupported(dispatch.name);
        }
    }
    return stats;
}

} // namespace dayo::graphics
