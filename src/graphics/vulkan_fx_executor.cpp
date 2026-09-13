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
            throw std::logic_error("VulkanFxExecutor: pass resource has no legacy backend binding: " + resource.name);
        const auto handle = resources.resolveTexture(resource.name);
        if (!handle.has_value())
            throw std::logic_error("VulkanFxExecutor: pass resource is unavailable: " + resource.name);
        return *handle;
    };
    const auto resolveTyped = [&](const dayo::fx::FxDispatch::ResourceUse& resource) -> handles::TextureHandle {
        if (resource.name.empty())
            throw std::logic_error("VulkanFxExecutor: pass resource has an empty typed binding");
        if (resources.resolveTypedTexture) {
            const auto handle = resources.resolveTypedTexture(resource.name);
            if (handle.has_value())
                return *handle;
        }
        if (resources.resolveTypedResource) {
            const auto binding = resources.resolveTypedResource(resource.name);
            if (binding.has_value() && binding->texture.valid())
                return binding->texture;
        }
        throw std::logic_error("VulkanFxExecutor: typed pass resource is unavailable: " + resource.name);
    };
    const auto resolveTypedWriteTarget = [&](const dayo::fx::FxDispatch& dispatch) -> handles::TextureHandle {
        for (const auto& resource : dispatch.resources) {
            if (resource.write)
                return resolveTyped(resource);
        }
        throw std::logic_error("VulkanFxExecutor: graphics pass has no writable color target: " + dispatch.name);
    };
    const auto prepareResources = [&](const dayo::fx::FxDispatch& dispatch) {
        std::unordered_set<TextureHandle> transitioned;
        std::unordered_set<handles::TextureHandle> transitionedTyped;
        std::vector<DescriptorBinding> bindings;
        bindings.reserve(dispatch.resources.size());
        for (std::size_t index = 0; index < dispatch.resources.size(); ++index) {
            const auto& resource = dispatch.resources[index];
            if (resources.resolveTypedResource) {
                const auto binding = resources.resolveTypedResource(resource.name);
                if (!binding.has_value() || !binding->valid())
                    throw std::logic_error("VulkanFxExecutor: typed pass resource is unavailable: " + resource.name);
                if (binding->texture.valid() && transitionedTyped.insert(binding->texture).second)
                    commands.transitionEx(binding->texture);
            } else if (resources.resolveTypedTexture) {
                const auto texture = resolveTyped(resource);
                if (transitionedTyped.insert(texture).second)
                    commands.transitionEx(texture);
            } else if (resources.resolveTexture) {
                const auto texture = resolve(resource);
                if (transitioned.insert(texture).second)
                    commands.transition(texture);
                if (!resources.resolveDescriptorSet) {
                    if (resources.resolveBinding) {
                        const auto binding =
                            resources.resolveBinding(resource.name, resource.write, static_cast<std::uint32_t>(index));
                        if (!binding.has_value())
                            throw std::logic_error("VulkanFxExecutor: descriptor binding is unavailable: " +
                                                   resource.name);
                        bindings.push_back(*binding);
                    } else {
                        bindings.push_back(DescriptorBinding{static_cast<std::uint32_t>(index), 0, texture, 0});
                    }
                }
            } else if (!resources.resolveDescriptorSet) {
                throw std::logic_error("VulkanFxExecutor: pass resource has no backend binding: " + resource.name);
            }
        }
        if (!bindings.empty() && !resources.resolveDescriptorSet)
            commands.bindResources(std::span<const DescriptorBinding>(bindings.data(), bindings.size()));
        if (resources.resolveDescriptorSets) {
            const auto descriptorSets = resources.resolveDescriptorSets(dispatch);
            for (const auto& descriptor : descriptorSets) {
                if (!descriptor.set.valid())
                    throw std::logic_error("VulkanFxExecutor: typed descriptor set is unavailable: " + dispatch.name);
                commands.bindDescriptorSetEx(descriptor.set, descriptor.setIndex);
            }
        } else if (resources.resolveDescriptorSet) {
            const auto descriptorSet = resources.resolveDescriptorSet(dispatch);
            if (!descriptorSet.has_value())
                throw std::logic_error("VulkanFxExecutor: typed descriptor set is unavailable: " + dispatch.name);
            commands.bindDescriptorSetEx(*descriptorSet);
        }
    };
    const auto prepareShaderPass = [&](const dayo::fx::FxDispatch& dispatch, bool beginRendering) {
        if (!dispatch.conditions.empty()) {
            if (!resources.evaluateConditions)
                throw std::logic_error("VulkanFxExecutor: pass conditions have no evaluator: " + dispatch.name);
            if (!resources.evaluateConditions(
                    std::span<const std::string>(dispatch.conditions.data(), dispatch.conditions.size()), context))
                return false;
        }
        prepareResources(dispatch);
        if (resources.resolveTypedPipeline) {
            if (beginRendering)
                commands.beginRenderingEx(resolveTypedWriteTarget(dispatch));
            const auto pipeline = resources.resolveTypedPipeline(dispatch);
            if (!pipeline.has_value())
                throw std::logic_error("VulkanFxExecutor: typed pipeline is unavailable: " + dispatch.name);
            commands.bindPipelineEx(*pipeline);
            if (resources.makePushConstants) {
                const auto constants = resources.makePushConstants(dispatch, context);
                if (!constants.empty())
                    commands.pushConstantsEx(std::span<const std::byte>(constants.data(), constants.size()));
            }
        } else {
            if (!resources.resolvePipeline)
                throw std::logic_error("VulkanFxExecutor: shader pass has no pipeline resolver: " + dispatch.name);
            const auto pipeline = resources.resolvePipeline(dispatch);
            if (!pipeline.has_value())
                throw std::logic_error("VulkanFxExecutor: pipeline is unavailable: " + dispatch.name);
            commands.bindPipeline(*pipeline);
            if (resources.makePushConstants) {
                const auto constants = resources.makePushConstants(dispatch, context);
                if (!constants.empty())
                    commands.pushConstants(std::span<const std::byte>(constants.data(), constants.size()));
            }
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
    const auto evaluateConditions = [&](const dayo::fx::FxDispatch& dispatch) {
        if (dispatch.conditions.empty())
            return true;
        if (!resources.evaluateConditions)
            throw std::logic_error("VulkanFxExecutor: pass conditions have no evaluator: " + dispatch.name);
        return resources.evaluateConditions(
            std::span<const std::string>(dispatch.conditions.data(), dispatch.conditions.size()), context);
    };
    for (const auto& dispatch : plan.ordered) {
        dayo::log::debug("VulkanFxExecutor pass ", dispatch.name, " kind ", dayo::fx::toString(dispatch.kind));
        switch (dispatch.kind) {
        case dayo::fx::FxOpKind::raster:
            if (!prepareShaderPass(dispatch, true))
                break;
            commands.draw(static_cast<std::uint32_t>(context.clonedVertexCount), context.cloneCount);
            if (resources.resolveTypedPipeline)
                commands.endRenderingEx();
            ++stats.raster;
            break;
        case dayo::fx::FxOpKind::postprocess:
            if (!prepareShaderPass(dispatch, true))
                break;
            commands.draw(3, 1);
            if (resources.resolveTypedPipeline)
                commands.endRenderingEx();
            ++stats.postprocess;
            break;
        case dayo::fx::FxOpKind::compute:
            if (!prepareShaderPass(dispatch, false))
                break;
            commands.dispatch((context.renderWidth + 7U) / 8U, (context.renderHeight + 7U) / 8U, 1);
            ++stats.compute;
            break;
        case dayo::fx::FxOpKind::copy:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() < 2 || dispatch.resources[0].write || !dispatch.resources[1].write)
                throw std::logic_error("VulkanFxExecutor: copy pass requires read source and write destination");
            if (resources.resolveTypedTexture || resources.resolveTypedResource) {
                commands.copyTextureEx(resolveTyped(dispatch.resources[0]), resolveTyped(dispatch.resources[1]));
            } else {
                commands.copyTexture(resolve(dispatch.resources[0]), resolve(dispatch.resources[1]));
            }
            ++stats.copy;
            break;
        case dayo::fx::FxOpKind::clear:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() != 1 || !dispatch.resources[0].write)
                throw std::logic_error("VulkanFxExecutor: clear pass requires one write target");
            if (resources.resolveTypedTexture || resources.resolveTypedResource)
                commands.clearTextureEx(resolveTyped(dispatch.resources[0]));
            else
                commands.clearTexture(resolve(dispatch.resources[0]));
            ++stats.clear;
            break;
        case dayo::fx::FxOpKind::mipmap:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() != 1)
                throw std::logic_error("VulkanFxExecutor: mipmap pass requires one target");
            if (resources.resolveTypedTexture || resources.resolveTypedResource)
                commands.generateMipmapsEx(resolveTyped(dispatch.resources[0]));
            else
                commands.generateMipmaps(resolve(dispatch.resources[0]));
            ++stats.mipmap;
            break;
        case dayo::fx::FxOpKind::raytracing:
            if (!evaluateConditions(dispatch))
                break;
            if (!resources.resolveTypedPipeline || !resources.resolveShaderBindingTable)
                throw FxRaytracingUnsupported(dispatch.name);
            {
                const auto pipeline = resources.resolveTypedPipeline(dispatch);
                if (!pipeline.has_value())
                    throw std::logic_error("VulkanFxExecutor: typed RT pipeline is unavailable: " + dispatch.name);
                const auto sbt = resources.resolveShaderBindingTable(dispatch);
                if (!sbt.has_value())
                    throw std::logic_error("VulkanFxExecutor: shader binding table is unavailable: " + dispatch.name);
                prepareResources(dispatch);
                commands.bindPipelineEx(*pipeline);
                if (resources.makePushConstants) {
                    const auto constants = resources.makePushConstants(dispatch, context);
                    if (!constants.empty())
                        commands.pushConstantsEx(std::span<const std::byte>(constants.data(), constants.size()));
                }
                commands.traceRaysEx(*pipeline, *sbt, context.renderWidth, context.renderHeight, 1);
                ++stats.rayTracing;
            }
            break;
        }
    }
    return stats;
}

} // namespace dayo::graphics
