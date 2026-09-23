#include "graphics/fx_executor.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
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
    const auto resolveTypedName = [&](std::string_view name) -> handles::TextureHandle {
        if (name.empty())
            throw std::logic_error("VulkanFxExecutor: pass resource has an empty typed binding");
        if (resources.resolveTypedTexture) {
            const auto handle = resources.resolveTypedTexture(name);
            if (handle.has_value())
                return *handle;
        }
        if (resources.resolveTypedResource) {
            const auto binding = resources.resolveTypedResource(name);
            if (binding.has_value() && binding->texture.valid())
                return binding->texture;
        }
        throw std::logic_error("VulkanFxExecutor: typed pass resource is unavailable: " + std::string(name));
    };
    const auto resolveTyped = [&](const dayo::fx::FxDispatch::ResourceUse& resource) -> handles::TextureHandle {
        return resolveTypedName(resource.name);
    };
    const auto resolveTypedWriteTarget = [&](const dayo::fx::FxDispatch& dispatch) -> handles::TextureHandle {
        for (const auto& resource : dispatch.resources) {
            if (resource.write && resource.role == dayo::fx::FxResourceRole::colorAttachment)
                return resolveTyped(resource);
        }
        // Preserve the legacy synthetic Preview plans, whose resources predate
        // explicit roles. Real YRZFX programs always mark their RTVs above.
        for (const auto& resource : dispatch.resources) {
            if (resource.write)
                return resolveTyped(resource);
        }
        if (resources.defaultColorTarget.valid())
            return resources.defaultColorTarget;
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
    const auto beginTypedRendering = [&](const dayo::fx::FxDispatch& dispatch, dayo::fx::FxExtent3D outputExtent) {
        if (const auto* raster = std::get_if<dayo::fx::FxRasterDispatch>(&dispatch.executable);
            raster != nullptr && (!raster->colorAttachments.empty() || raster->depthAttachment.has_value())) {
            RenderingInfoEx info;
            info.extent = {outputExtent.width, outputExtent.height, outputExtent.depth};
            info.colors.reserve(raster->colorAttachments.size());
            for (const auto& attachment : raster->colorAttachments) {
                info.colors.push_back({.texture = resolveTypedName(attachment.name),
                                       .clear = attachment.clear,
                                       .clearColor = attachment.clearValue.color});
            }
            if (raster->depthAttachment.has_value()) {
                const auto& attachment = *raster->depthAttachment;
                info.depth = DepthAttachmentEx{.texture = resolveTypedName(attachment.name),
                                               .clear = attachment.clear,
                                               .clearDepth = attachment.clearValue.depth,
                                               .clearStencil = attachment.clearValue.stencil};
            }
            commands.beginRenderingEx(info);
            return;
        }
        if (const auto* postprocess = std::get_if<dayo::fx::FxPostProcessDispatch>(&dispatch.executable);
            postprocess != nullptr && !postprocess->colorAttachments.empty()) {
            RenderingInfoEx info;
            info.extent = {outputExtent.width, outputExtent.height, outputExtent.depth};
            info.colors.reserve(postprocess->colorAttachments.size());
            for (const auto& attachment : postprocess->colorAttachments) {
                info.colors.push_back({.texture = resolveTypedName(attachment.name),
                                       .clear = attachment.clear,
                                       .clearColor = attachment.clearValue.color});
            }
            commands.beginRenderingEx(info);
            return;
        }
        const auto target = resolveTypedWriteTarget(dispatch);
        const auto hasExplicitTarget = std::ranges::any_of(
            dispatch.resources, [](const dayo::fx::FxDispatch::ResourceUse& resource) { return resource.write; });
        if (!hasExplicitTarget && resources.defaultColorTarget.valid())
            commands.transitionEx(target);
        commands.beginRenderingEx(target);
    };
    const auto prepareShaderPass = [&](const dayo::fx::FxDispatch& dispatch, bool beginRendering,
                                       dayo::fx::FxExtent3D outputExtent) {
        if (!dispatch.conditions.empty()) {
            if (!resources.evaluateConditions)
                throw std::logic_error("VulkanFxExecutor: pass conditions have no evaluator: " + dispatch.name);
            if (!resources.evaluateConditions(
                    std::span<const std::string>(dispatch.conditions.data(), dispatch.conditions.size()), context))
                return false;
        }
        if (resources.beforePass)
            resources.beforePass(dispatch, commands);
        prepareResources(dispatch);
        if (resources.resolveTypedPipeline) {
            if (beginRendering)
                beginTypedRendering(dispatch, outputExtent);
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
        if (resources.beforePass)
            resources.beforePass(dispatch, commands);
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
    for (std::size_t passIndex = 0; passIndex < plan.ordered.size(); ++passIndex) {
        const auto& dispatch = plan.ordered[passIndex];
        const auto* resolved = passIndex < plan.resolved.size() ? &plan.resolved[passIndex] : nullptr;
        const auto outputExtent = resolved == nullptr ? dayo::fx::FxExtent3D{std::max(context.renderWidth, 1U),
                                                                             std::max(context.renderHeight, 1U), 1}
                                                      : resolved->outputExtent;
        dayo::log::debug("VulkanFxExecutor pass ", dispatch.name, " kind ", dayo::fx::toString(dispatch.kind));
        bool executed = false;
        switch (dispatch.kind) {
        case dayo::fx::FxOpKind::raster:
            if (!prepareShaderPass(dispatch, true, outputExtent))
                break;
            {
                const auto* raster = std::get_if<dayo::fx::FxRasterDispatch>(&dispatch.executable);
                if (raster != nullptr && raster->rasterSource != dayo::core::EffectRasterSource::scene) {
                    std::optional<FxExecutionResources::TypedResource> vertexBuffer;
                    std::optional<FxExecutionResources::TypedResource> indexBuffer;
                    if (!raster->vertexBuffer.empty()) {
                        if (!resources.resolveTypedResource)
                            throw std::logic_error(
                                "VulkanFxExecutor: FX vertex buffer has no typed resource resolver: " +
                                raster->vertexBuffer);
                        vertexBuffer = resources.resolveTypedResource(raster->vertexBuffer);
                        if (!vertexBuffer.has_value() || !vertexBuffer->buffer.valid())
                            throw std::logic_error("VulkanFxExecutor: FX vertex buffer is unavailable or unresolved: " +
                                                   raster->vertexBuffer);
                    }
                    if (!raster->indexBuffer.empty()) {
                        if (!resources.resolveTypedResource)
                            throw std::logic_error(
                                "VulkanFxExecutor: FX index buffer has no typed resource resolver: " +
                                raster->indexBuffer);
                        indexBuffer = resources.resolveTypedResource(raster->indexBuffer);
                        const auto indexCount =
                            resolved != nullptr && resolved->raster.has_value() ? resolved->raster->indexCount : 0U;
                        if (!indexBuffer.has_value() || !indexBuffer->buffer.valid() || indexCount == 0)
                            throw std::logic_error("VulkanFxExecutor: FX index buffer is unavailable or unresolved: " +
                                                   raster->indexBuffer);
                        if (vertexBuffer.has_value()) {
                            IndexedDrawEx draw{.vertexBuffer = vertexBuffer->buffer,
                                               .indexBuffer = indexBuffer->buffer,
                                               .indexCount = indexCount,
                                               .instanceCount = context.cloneCount};
                            if (raster->vertexLayout.bindings.empty()) {
                                draw.vertexBuffers.push_back({.binding = 0, .buffer = vertexBuffer->buffer});
                            } else {
                                for (const auto& binding : raster->vertexLayout.bindings)
                                    draw.vertexBuffers.push_back(
                                        {.binding = binding.binding, .buffer = vertexBuffer->buffer});
                            }
                            commands.drawIndexedEx(draw);
                        } else {
                            commands.drawIndexedBufferlessEx(indexBuffer->buffer, indexCount, context.cloneCount);
                        }
                        ++stats.indexedDraws;
                    } else if (vertexBuffer.has_value()) {
                        const auto vertexCount =
                            resolved != nullptr && resolved->raster.has_value() ? resolved->raster->vertexCount : 0U;
                        if (vertexCount == 0)
                            throw std::logic_error(
                                "VulkanFxExecutor: FX vertex buffer has no resolved element count: " +
                                raster->vertexBuffer);
                        VertexDrawEx draw{.vertexCount = vertexCount, .instanceCount = context.cloneCount};
                        if (raster->vertexLayout.bindings.empty()) {
                            draw.vertexBuffers.push_back({.binding = 0, .buffer = vertexBuffer->buffer});
                        } else {
                            for (const auto& binding : raster->vertexLayout.bindings)
                                draw.vertexBuffers.push_back(
                                    {.binding = binding.binding, .buffer = vertexBuffer->buffer});
                        }
                        commands.drawVertexBufferEx(draw);
                        ++stats.vertexBufferDraws;
                    } else {
                        commands.draw(static_cast<std::uint32_t>(context.clonedVertexCount), context.cloneCount);
                    }
                } else if (!resources.sceneDraws.empty()) {
                    const auto target =
                        raster == nullptr ? dayo::core::fx::RasterModelTarget::all : raster->graphics.modelTarget;
                    for (const auto& sceneDraw : resources.sceneDraws) {
                        if (!matchesRasterTarget(target, resources.rasterControllerModel, sceneDraw))
                            continue;
                        if (resources.updatePassConstants)
                            resources.updatePassConstants(commands, sceneDraw);
                        commands.drawIndexedEx({.vertexBuffer = sceneDraw.vertexBuffer,
                                                .indexBuffer = sceneDraw.indexBuffer,
                                                .firstIndex = sceneDraw.firstIndex,
                                                .indexCount = sceneDraw.indexCount,
                                                .vertexOffset = sceneDraw.vertexOffset,
                                                .firstInstance = sceneDraw.firstInstance,
                                                .instanceCount = sceneDraw.instanceCount,
                                                .modelIndex = sceneDraw.modelIndex,
                                                .materialIndex = sceneDraw.materialIndex});
                        ++stats.indexedDraws;
                    }
                } else {
                    commands.draw(static_cast<std::uint32_t>(context.clonedVertexCount), context.cloneCount);
                }
            }
            if (resources.resolveTypedPipeline)
                commands.endRenderingEx();
            ++stats.raster;
            executed = true;
            break;
        case dayo::fx::FxOpKind::postprocess:
            if (!prepareShaderPass(dispatch, true, outputExtent))
                break;
            commands.draw(3, 1);
            if (resources.resolveTypedPipeline)
                commands.endRenderingEx();
            ++stats.postprocess;
            executed = true;
            break;
        case dayo::fx::FxOpKind::compute:
            if (!prepareShaderPass(dispatch, false, outputExtent))
                break;
            if (resolved != nullptr) {
                commands.dispatch(resolved->dispatchGroups.width, resolved->dispatchGroups.height,
                                  resolved->dispatchGroups.depth);
            } else {
                const auto ceilDiv = [](std::uint32_t value, std::uint32_t divisor) {
                    if (divisor == 0)
                        throw std::logic_error("VulkanFxExecutor: compute numthreads component is zero");
                    return value / divisor + (value % divisor == 0 ? 0U : 1U);
                };
                const auto dimension = outputExtent.dimension >= 1 && outputExtent.dimension <= 3
                                           ? outputExtent.dimension
                                           : (outputExtent.depth > 1 ? 3U : (outputExtent.height > 1 ? 2U : 1U));
                auto threads = dispatch.numThreads;
                if (threads[0] == 0 && threads[1] == 0 && threads[2] == 0) {
                    threads = dimension == 1 ? std::array<std::uint32_t, 3>{1024, 1, 1}
                                             : (dimension == 2 ? std::array<std::uint32_t, 3>{16, 16, 1}
                                                               : std::array<std::uint32_t, 3>{8, 8, 8});
                } else {
                    for (auto& threadCount : threads)
                        threadCount = std::max(threadCount, 1U);
                }
                commands.dispatch(ceilDiv(outputExtent.width, threads[0]), ceilDiv(outputExtent.height, threads[1]),
                                  ceilDiv(outputExtent.depth, threads[2]));
            }
            ++stats.compute;
            executed = true;
            break;
        case dayo::fx::FxOpKind::copy:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() < 2 || dispatch.resources[0].write || !dispatch.resources[1].write)
                throw std::logic_error("VulkanFxExecutor: copy pass requires read source and write destination");
            if (resources.resolveTypedTexture || resources.resolveTypedResource) {
                if (resources.resolveTypedResource) {
                    const auto source = resources.resolveTypedResource(dispatch.resources[0].name);
                    const auto destination = resources.resolveTypedResource(dispatch.resources[1].name);
                    if (!source.has_value() || !destination.has_value())
                        throw std::logic_error("VulkanFxExecutor: typed copy resource is unavailable");
                    if (source->buffer.valid() && destination->buffer.valid())
                        commands.copyBufferEx(source->buffer, destination->buffer);
                    else if (source->texture.valid() && destination->texture.valid())
                        commands.copyTextureEx(source->texture, destination->texture);
                    else
                        throw std::logic_error("VulkanFxExecutor: copy source and destination kinds do not match");
                } else {
                    commands.copyTextureEx(resolveTyped(dispatch.resources[0]), resolveTyped(dispatch.resources[1]));
                }
            } else {
                commands.copyTexture(resolve(dispatch.resources[0]), resolve(dispatch.resources[1]));
            }
            ++stats.copy;
            executed = true;
            break;
        case dayo::fx::FxOpKind::clear:
            if (!prepareUtilityPass(dispatch))
                break;
            if (dispatch.resources.size() != 1 || !dispatch.resources[0].write)
                throw std::logic_error("VulkanFxExecutor: clear pass requires one write target");
            if (resources.resolveTypedResource) {
                const auto target = resources.resolveTypedResource(dispatch.resources[0].name);
                if (!target.has_value())
                    throw std::logic_error("VulkanFxExecutor: clear target is unavailable");
                if (target->buffer.valid()) {
                    commands.clearBufferEx(target->buffer, dispatch.functional.clearValue.color);
                } else if (target->texture.valid()) {
                    commands.clearTextureEx(target->texture, dispatch.functional.clearValue.color);
                } else {
                    throw std::logic_error("VulkanFxExecutor: clear target must be a texture or buffer");
                }
            } else if (resources.resolveTypedTexture) {
                commands.clearTextureEx(resolveTyped(dispatch.resources[0]), dispatch.functional.clearValue.color);
            } else {
                commands.clearTexture(resolve(dispatch.resources[0]));
            }
            ++stats.clear;
            executed = true;
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
            executed = true;
            break;
        case dayo::fx::FxOpKind::oidn:
            if (!prepareUtilityPass(dispatch))
                break;
            if (!resources.executeOidn && !resources.executeOidnWithResolver)
                throw std::logic_error("VulkanFxExecutor: OIDN pass has no host denoiser");
            {
                const auto* oidn = std::get_if<dayo::fx::FxOidnDispatch>(&dispatch.executable);
                if (oidn == nullptr)
                    throw std::logic_error("VulkanFxExecutor: OIDN pass has no typed dispatch: " + dispatch.name);
                const auto oidnExecuted =
                    resources.executeOidnWithResolver
                        ? resources.executeOidnWithResolver(*oidn, context, commands, resources.resolveTypedResource)
                        : resources.executeOidn(*oidn, context, commands);
                if (!oidnExecuted)
                    throw std::logic_error("VulkanFxExecutor: OIDN host execution failed: " + dispatch.name);
                ++stats.oidn;
                executed = true;
            }
            break;
        case dayo::fx::FxOpKind::raytracing:
            if (!evaluateConditions(dispatch))
                break;
            if (resources.beforePass)
                resources.beforePass(dispatch, commands);
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
                commands.traceRaysEx(*pipeline, *sbt, outputExtent.width, outputExtent.height, outputExtent.depth);
                ++stats.rayTracing;
                executed = true;
            }
            break;
        }
        const bool wroteResource =
            std::ranges::any_of(dispatch.resources, [](const auto& resource) { return resource.write; });
        if (executed && wroteResource && resources.resolveTypedPipeline)
            commands.memoryBarrierEx();
        if (executed) {
            if (resources.afterPass)
                resources.afterPass(dispatch, commands);
        }
    }
    return stats;
}

} // namespace dayo::graphics
