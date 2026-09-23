#include "fx/fx_compiler.hpp"

#include "core/fx/fx_size.hpp"
#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dayo::fx {

namespace {

class FrameExtentTable final : public core::fx::FxResourceTable {
  public:
    void add(std::string name, core::fx::FxExtent extent) {
        values_.insert_or_assign(std::move(name), extent);
    }

    [[nodiscard]] std::optional<core::fx::FxExtent> find(std::string_view name) const override {
        const auto found = values_.find(std::string(name));
        return found == values_.end() ? std::nullopt : std::optional<core::fx::FxExtent>{found->second};
    }

  private:
    std::unordered_map<std::string, core::fx::FxExtent> values_;
};

[[nodiscard]] std::int64_t fxSizeContextValue(std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("FX frame size context exceeds signed range");
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] core::fx::FxEvalContext makeSizeEvalContext(const FxFrameContext& context) {
    core::fx::FxEvalContext result;
    result.rtWidth = context.renderWidth;
    result.rtHeight = context.renderHeight;
    result.vertexCount = fxSizeContextValue(context.vertexCount);
    result.totalMaterial = fxSizeContextValue(context.totalMaterial);
    result.modelIndex = context.modelIndex;
    result.cloneCount = context.cloneCount;
    result.clonedVertexCount = fxSizeContextValue(context.clonedVertexCount);
    result.frameIndex = static_cast<std::int64_t>(context.frame);
    result.sampleIndex = fxSizeContextValue(static_cast<std::size_t>(context.sample));
    result.time = context.time;
    result.namedSymbols = context.expressionSymbols;
    return result;
}

[[nodiscard]] core::fx::FxExtent resolveEffectSize(const core::EffectSize& source, std::uint32_t defaultDimension,
                                                   bool defaultToScreen, const FxFrameContext& context,
                                                   const FrameExtentTable& table) {
    core::fx::FxSizeExpr expression;
    expression.base = source.absolute ? std::string{} : source.base;
    expression.dimension =
        source.dimension != 0 ? source.dimension : (!source.base.empty() && !source.absolute ? 0U : defaultDimension);
    expression.widthRatio = source.absolute ? 1.0F : source.widthRatio;
    expression.heightRatio = source.absolute ? 1.0F : source.heightRatio;
    expression.depthRatio = source.absolute ? 1.0F : source.depthRatio;
    if (source.absolute || source.rounding == "trunc")
        expression.rounding = core::fx::FxSizeExpr::Rounding::truncate;
    else if (source.rounding == "round")
        expression.rounding = core::fx::FxSizeExpr::Rounding::nearest;
    else if (source.rounding == "ceil")
        expression.rounding = core::fx::FxSizeExpr::Rounding::ceil;
    else
        throw std::invalid_argument("unsupported YRZFX size rounding mode: " + source.rounding);
    const auto conversion = [](std::string_view base, std::string_view conv) {
        if (conv == "one")
            return std::string{"1"};
        std::string result;
        const bool scalarBase = base == "VERTEXCOUNT" || base == "CLONEDVERTEXCOUNT" || base == "TOTALMATERIAL" ||
                                base == "TOTALMATERIALCOUNT";
        for (const char axis : std::array<char, 3>{'x', 'y', 'z'}) {
            const bool containsAxis = std::ranges::any_of(
                conv, [axis](unsigned char character) { return static_cast<char>(std::tolower(character)) == axis; });
            if (!containsAxis)
                continue;
            if (!result.empty())
                result += '*';
            if (scalarBase && axis == 'x')
                result += base;
            else if (scalarBase)
                result += '1';
            else
                result += std::string(base) + '.' + axis;
        }
        if (result.empty())
            throw std::invalid_argument("unsupported YRZFX size conversion: " + std::string(conv));
        return result;
    };
    if (source.absolute && source.width != 0)
        expression.xExpr = std::to_string(source.width);
    if (source.absolute && source.height != 0)
        expression.yExpr = std::to_string(source.height);
    if (source.absolute && source.depth != 0)
        expression.zExpr = std::to_string(source.depth);
    if (expression.base.empty() && expression.xExpr.empty()) {
        if (defaultToScreen) {
            expression.base = "DEFAULT_RTSIZE";
            expression.dimension = defaultDimension;
        } else {
            expression.xExpr = "1";
            if (expression.dimension >= 2)
                expression.yExpr = "1";
            if (expression.dimension >= 3)
                expression.zExpr = "1";
        }
    }
    if (!source.absolute) {
        auto base = expression.base;
        if (base.empty())
            base = defaultToScreen ? "DEFAULT_RTSIZE" : "";
        if (expression.xExpr.empty() && !base.empty())
            expression.xExpr = conversion(base, source.convX);
        if (expression.yExpr.empty() && (expression.dimension == 0 || expression.dimension >= 2) && !base.empty())
            expression.yExpr = conversion(base, source.convY);
        if (expression.zExpr.empty() && (expression.dimension == 0 || expression.dimension >= 3) && !base.empty())
            expression.zExpr = conversion(base, source.convZ);
    }
    return core::fx::FxSizeResolver{}.resolve(expression, makeSizeEvalContext(context), table);
}

[[nodiscard]] FxExtent3D fromFxExtent(core::fx::FxExtent extent) noexcept {
    return {.width = extent.x, .height = extent.y, .depth = extent.z, .dimension = extent.dimension};
}

[[nodiscard]] std::uint32_t ceilDiv(std::uint32_t value, std::uint32_t divisor) {
    if (divisor == 0)
        throw std::invalid_argument("FX numthreads component must be greater than zero");
    return value / divisor + (value % divisor == 0 ? 0U : 1U);
}

[[nodiscard]] std::optional<FxExtent3D> declaredExtent(const FxProgram& program, std::string_view name,
                                                       const FxFrameContext& context, const FrameExtentTable& table) {
    for (const auto& texture : program.textures)
        if (texture.name == name)
            return fromFxExtent(resolveEffectSize(texture.size, 2, true, context, table));
    for (const auto& texture : program.textures3D)
        if (texture.name == name)
            return fromFxExtent(resolveEffectSize(texture.size, 3, false, context, table));
    for (const auto& buffer : program.buffers) {
        if (buffer.name != name)
            continue;
        auto size = buffer.size;
        if (!size.absolute && size.base.empty()) {
            size.base = program.category == core::fx::FxCategory::deform ? "CLONEDVERTEXCOUNT" : "DEFAULT_RTSIZE";
            size.dimension = program.category == core::fx::FxCategory::deform ? 1U : 2U;
        }
        return fromFxExtent(resolveEffectSize(size, 1, false, context, table));
    }
    return std::nullopt;
}

[[nodiscard]] bool hasOutputSize(const core::EffectSize& size) noexcept {
    return size.absolute || !size.base.empty() || size.dimension != 0 || size.widthRatio != 1.0F ||
           size.heightRatio != 1.0F || size.depthRatio != 1.0F || size.convX != "x" || size.convY != "y" ||
           size.convZ != "z" || size.rounding != "trunc";
}

[[nodiscard]] std::optional<FxExtent3D> firstOutputExtent(const FxDispatch& dispatch, FxResourceRole role,
                                                          const FxProgram& program, const FxFrameContext& context,
                                                          const FrameExtentTable& table) {
    for (const auto& use : dispatch.resources) {
        if (!use.write || use.role != role)
            continue;
        if (const auto extent = declaredExtent(program, use.name, context, table); extent.has_value())
            return extent;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<FxResolvedRasterTarget> rasterTarget(const FxDispatch& dispatch) {
    FxResolvedRasterTarget target;
    if (const auto* raster = std::get_if<FxRasterDispatch>(&dispatch.executable)) {
        for (const auto& color : raster->colorAttachments)
            if (!color.name.empty())
                target.colors.push_back(color.name);
        if (raster->depthAttachment.has_value() && !raster->depthAttachment->name.empty())
            target.depth = raster->depthAttachment->name;
        if (!raster->vertexBuffer.empty())
            target.vertexBuffer = raster->vertexBuffer;
        if (!raster->indexBuffer.empty())
            target.indexBuffer = raster->indexBuffer;
    } else if (const auto* post = std::get_if<FxPostProcessDispatch>(&dispatch.executable)) {
        for (const auto& color : post->colorAttachments)
            if (!color.name.empty())
                target.colors.push_back(color.name);
    } else {
        return std::nullopt;
    }
    return target;
}

[[nodiscard]] std::uint32_t rasterBufferElementCount(const FxProgram& program, std::string_view name,
                                                     const FxFrameContext& context, const FrameExtentTable& table) {
    const auto extent = declaredExtent(program, name, context, table);
    if (!extent.has_value())
        throw std::invalid_argument("FX raster buffer is not declared: " + std::string(name));
    const auto count = static_cast<std::uint64_t>(extent->width) * extent->height * extent->depth;
    if (count == 0 || count > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("FX raster buffer element count is out of range: " + std::string(name));
    return static_cast<std::uint32_t>(count);
}

} // namespace

FxPassBindingPlan planPassBindings(const FxProgram& program, const FxDispatch& dispatch, std::uint32_t resourceSet) {
    FxPassBindingPlan result;
    // HLSL register namespaces are shared across descriptor kinds: t0 may
    // name either a Texture or StructuredBuffer, but never both in one pass.
    std::uint32_t sampledBinding = 0;
    std::uint32_t writableBinding = 0;
    std::uint32_t samplerBinding = 0;
    const auto isTexture = [&program](std::string_view name) {
        return std::ranges::any_of(program.textures, [name](const auto& resource) { return resource.name == name; }) ||
               std::ranges::any_of(program.textures3D, [name](const auto& resource) { return resource.name == name; });
    };
    const auto isBuffer = [&program](std::string_view name) {
        return std::ranges::any_of(program.buffers, [name](const auto& resource) { return resource.name == name; });
    };
    const auto isSampler = [&program](std::string_view name) {
        return std::ranges::any_of(program.samplers, [name](const auto& resource) { return resource.name == name; });
    };
    const auto useFor = [&dispatch](std::string_view name) -> const FxDispatch::ResourceUse* {
        const auto found =
            std::ranges::find_if(dispatch.resources, [name](const auto& use) { return use.name == name; });
        return found == dispatch.resources.end() ? nullptr : &*found;
    };
    const auto append = [&](std::string_view name, bool sampler) {
        if (name.empty())
            return;
        const auto* use = useFor(name);
        const bool writable = use != nullptr && use->write;
        if (use != nullptr &&
            (use->role == FxResourceRole::colorAttachment || use->role == FxResourceRole::depthAttachment))
            return;
        FxDescriptorClass descriptorClass{};
        if (sampler || isSampler(name)) {
            descriptorClass = FxDescriptorClass::sampler;
        } else if (isTexture(name)) {
            descriptorClass = writable ? FxDescriptorClass::storageImage : FxDescriptorClass::sampledImage;
        } else if (isBuffer(name)) {
            descriptorClass = FxDescriptorClass::storageBuffer;
        } else {
            // Host-owned resources have their own provider descriptor set and
            // must not silently enter the effect-local set.
            return;
        }
        const auto localBinding = descriptorClass == FxDescriptorClass::sampler
                                      ? samplerBinding++
                                      : (writable ? writableBinding++ : sampledBinding++);
        result.bindings.push_back({.resource = std::string(name),
                                   .descriptorClass = descriptorClass,
                                   .set = resourceSet,
                                   .binding = fxDescriptorBindingBaseForUse(descriptorClass, writable) + localBinding,
                                   .count = 1,
                                   .writable = writable});
    };
    for (const auto& texture : program.textures) {
        const auto* use = useFor(texture.name);
        if (use == nullptr || use->role != FxResourceRole::colorAttachment)
            append(texture.name, false);
    }
    for (const auto& texture : program.textures3D) {
        const auto* use = useFor(texture.name);
        if (use == nullptr || use->role != FxResourceRole::colorAttachment)
            append(texture.name, false);
    }
    for (const auto& buffer : program.buffers)
        append(buffer.name, false);
    for (const auto& sampler : program.samplers)
        append(sampler.name, true);
    if (program.materialDescriptor.has_value()) {
        if (resourceSet == std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("FX MatDesc secondary descriptor set index overflow");
        const auto appendMaterialBinding = [](std::string_view name, FxDescriptorClass descriptorClass,
                                              std::uint32_t set, std::uint32_t localBinding) {
            return FxLogicalBinding{.resource = std::string(name),
                                    .descriptorClass = descriptorClass,
                                    .set = set,
                                    .binding = fxDescriptorBindingBase(descriptorClass) + localBinding,
                                    .count = 1,
                                    .writable = false};
        };
        FxMaterialDescriptorPlan material;
        material.materialIndices =
            appendMaterialBinding("@matdesc/idx", FxDescriptorClass::storageBuffer, resourceSet, sampledBinding++);
        material.textureIndices2D =
            appendMaterialBinding("@matdesc/tex", FxDescriptorClass::storageBuffer, resourceSet, sampledBinding++);
        material.textureIndices3D =
            appendMaterialBinding("@matdesc/tex3D", FxDescriptorClass::storageBuffer, resourceSet, sampledBinding++);
        material.values =
            appendMaterialBinding("@matdesc/value", FxDescriptorClass::storageBuffer, resourceSet, sampledBinding++);
        material.textures2D =
            appendMaterialBinding("@matdesc/texture2D", FxDescriptorClass::sampledImage, resourceSet, sampledBinding++);
        material.textures3D =
            appendMaterialBinding("@matdesc/texture3D", FxDescriptorClass::sampledImage, resourceSet + 1U, 0);
        result.material = std::move(material);
    }
    return result;
}

const char* toString(FxOpKind kind) noexcept {
    switch (kind) {
    case FxOpKind::raster:
        return "raster";
    case FxOpKind::postprocess:
        return "postprocess";
    case FxOpKind::compute:
        return "compute";
    case FxOpKind::copy:
        return "copy";
    case FxOpKind::clear:
        return "clear";
    case FxOpKind::mipmap:
        return "mipmap";
    case FxOpKind::raytracing:
        return "raytracing";
    case FxOpKind::oidn:
        return "oidn";
    }
    return "raster";
}

FxOpKind fxOpFromPassType(core::EffectPassType type) {
    switch (type) {
    case core::EffectPassType::rasterizer:
        return FxOpKind::raster;
    case core::EffectPassType::postprocess:
        return FxOpKind::postprocess;
    case core::EffectPassType::compute:
        return FxOpKind::compute;
    case core::EffectPassType::raytracing:
        return FxOpKind::raytracing;
    case core::EffectPassType::copy:
        return FxOpKind::copy;
    case core::EffectPassType::clear:
        return FxOpKind::clear;
    case core::EffectPassType::mipmap:
        return FxOpKind::mipmap;
    case core::EffectPassType::oidn:
        return FxOpKind::oidn;
    case core::EffectPassType::unknown:
        throw std::runtime_error("unsupported FX pass type: unknown");
    }
    throw std::runtime_error("unsupported FX pass type");
}

core::EffectGraph FxCompiler::parse(const FxSourceDocument& document) const {
    if (document.raw.empty()) {
        throw std::runtime_error("fx source is empty: " + document.path.string());
    }
    // Parse the caller's immutable buffer. In particular, a watcher/editor
    // may have newer text than the path on disk.
    try {
        core::EffectGraph graph = core::loadEffectGraphFromText(document.path, document.raw);
        graph.sourcePath = document.path;
        return graph;
    } catch (const std::exception&) {
        if (!options_.allowSyntheticProgramForTests)
            throw;
        core::EffectGraph graph;
        graph.sourcePath = document.path;
        graph.category = "render";
        core::EffectPass pass;
        pass.name = document.path.stem().string();
        if (pass.name.empty())
            pass.name = "main";
        pass.type = core::EffectPassType::rasterizer;
        graph.passes.push_back(std::move(pass));
        dayo::log::debug("FxCompiler using explicit test-only synthetic pass for ", document.path.string());
        return graph;
    }
}

core::EffectGraph FxCompiler::link(const core::EffectGraph& graph, std::string* error) const {
    for (const auto& pass : graph.passes) {
        if (pass.name.empty()) {
            if (error != nullptr)
                *error = "fx link: pass with empty name";
            throw std::runtime_error("fx link: pass with empty name");
        }
    }
    return graph;
}

FxProgram FxCompiler::compile(const core::EffectGraph& graph) const {
    FxProgram program;
    program.label = graph.sourcePath.string();
    if (program.label.empty())
        program.label = graph.category.empty() ? "fx" : graph.category;
    program.generation = 1;
    if (!graph.category.empty())
        program.category = core::fx::fxCategoryFromString(graph.category);
    program.sourcePath = graph.sourcePath;
    program.materialDescriptor = graph.materialDescriptor;
    if (graph.materialDescriptor.has_value()) {
        const auto& descriptor = *graph.materialDescriptor;
        const auto templatePath = descriptor.templatePath.is_absolute()
                                      ? descriptor.templatePath
                                      : graph.sourcePath.parent_path() / descriptor.templatePath;
        auto schema =
            core::fx::loadMaterialTemplateSchema(templatePath, descriptor.name, graph.sourcePath.parent_path());
        if (!descriptor.defaultFile.empty()) {
            const auto defaultFile = descriptor.defaultFile.is_absolute()
                                         ? descriptor.defaultFile
                                         : graph.sourcePath.parent_path() / descriptor.defaultFile;
            core::fx::loadMaterialDefaultFile(schema, defaultFile);
        }
        program.materialSchema = std::move(schema);
    }
    program.hlslPrefix = graph.hlslPrefix;
    program.generatedCode = graph.generatedCode;
    program.hlsl = graph.hlsl;
    program.textures = graph.textures;
    program.textures3D = graph.textures3D;
    program.buffers = graph.buffers;
    program.samplers = graph.samplers;
    program.controllers = graph.controllers;
    program.meshCloneCount = graph.meshCloneCount;
    program.memos = graph.memos;
    program.globalVarSize = graph.globalVarSize;
    program.globalVarSizeSpecified = graph.globalVarSizeSpecified;
    program.rawYrzfx = graph.rawYrzfx;
    for (const auto& pass : graph.passes) {
        FxDispatch dispatch;
        dispatch.name = pass.name.empty() ? "pass" : pass.name;
        dispatch.kind = fxOpFromPassType(pass.type);
        dispatch.category = program.category;
        dispatch.conditions = pass.conditions;
        dispatch.macros = pass.macros;
        dispatch.numThreads = pass.numThreads;
        dispatch.outputSize = pass.outputSize;
        dispatch.outputWidthRatio = pass.outputWidthRatio;
        dispatch.outputHeightRatio = pass.outputHeightRatio;
        dispatch.functionalKind = pass.functionalKind;
        dispatch.functional = pass.functional;
        if (!pass.computeShader.empty())
            dispatch.shader = pass.computeShader;
        else if (!pass.pixelShader.empty())
            dispatch.shader = pass.pixelShader;
        else
            dispatch.shader = pass.vertexShader;
        switch (pass.type) {
        case core::EffectPassType::rasterizer: {
            FxRasterDispatch raster;
            raster.vertexShader = pass.vertexShader;
            raster.pixelShader = pass.pixelShader;
            raster.graphics = pass.graphics;
            raster.colorAttachments = pass.renderTargets;
            if (!pass.depth.name.empty())
                raster.depthAttachment = pass.depth;
            raster.rasterSource = pass.rasterSource;
            raster.vertexLayout = pass.vertexLayout;
            raster.vertexBuffer = pass.rasterVertexBuffer;
            raster.indexBuffer = pass.rasterIndexBuffer;
            dispatch.executable = std::move(raster);
            break;
        }
        case core::EffectPassType::postprocess: {
            FxPostProcessDispatch postprocess;
            postprocess.pixelShader = pass.pixelShader;
            postprocess.colorAttachments = pass.renderTargets;
            dispatch.executable = std::move(postprocess);
            break;
        }
        case core::EffectPassType::compute:
            dispatch.executable = FxComputeDispatch{pass.computeShader};
            break;
        case core::EffectPassType::raytracing: {
            FxRayTracingDispatch ray;
            ray.rayGenerationShader = pass.rayGenerationShader;
            ray.missShaders = pass.missShaders;
            ray.callableShaders = pass.callableShaders;
            for (const auto& group : pass.hitGroups) {
                ray.hitGroups.push_back(
                    {core::fx::rayTracingHitGroupType(group.type), group.closestHit, group.anyHit, group.intersection});
            }
            ray.maxPayloadSize = pass.maxPayloadSize;
            ray.maxAttributeSize = pass.maxAttributeSize;
            ray.maxRecursionDepth = pass.maxRecursionDepth;
            dispatch.executable = std::move(ray);
            break;
        }
        case core::EffectPassType::oidn: {
            FxOidnDispatch oidn;
            oidn.input = pass.oidnInput;
            oidn.albedo = pass.oidnAlbedo;
            oidn.normal = pass.oidnNormal;
            oidn.output = pass.oidnOutput;
            if (oidn.input.empty() && !pass.inputs.empty())
                oidn.input = pass.inputs.front().name;
            if (oidn.albedo.empty() && pass.inputs.size() > 1)
                oidn.albedo = pass.inputs[1].name;
            if (oidn.normal.empty() && pass.inputs.size() > 2)
                oidn.normal = pass.inputs[2].name;
            if (oidn.output.empty() && pass.renderTargets.size() == 1)
                oidn.output = pass.renderTargets.front().name;
            if (oidn.output.empty() && pass.unorderedAccess.size() == 1)
                oidn.output = pass.unorderedAccess.front().name;
            if (oidn.input.empty() || oidn.output.empty())
                throw std::runtime_error("FX OIDN pass requires an input and output: " + pass.name);
            dispatch.executable = std::move(oidn);
            break;
        }
        case core::EffectPassType::copy:
        case core::EffectPassType::clear:
        case core::EffectPassType::mipmap:
            dispatch.executable = FxUtilityDispatch{};
            break;
        case core::EffectPassType::unknown:
            break;
        }
        const auto appendInput = [&](const core::EffectAttachment& attachment) {
            if (!attachment.name.empty())
                dispatch.resources.push_back({attachment.name, false, FxResourceRole::sampled});
        };
        const auto appendOutput = [&](const core::EffectAttachment& attachment, FxResourceRole role) {
            if (!attachment.name.empty())
                dispatch.resources.push_back({attachment.name, true, role});
        };
        const auto appendUtilityTarget = [&](std::string_view kind) {
            std::size_t targetCount = 0;
            const core::EffectAttachment* target = nullptr;
            for (const auto& attachment : pass.renderTargets) {
                if (!attachment.name.empty()) {
                    ++targetCount;
                    target = &attachment;
                }
            }
            for (const auto& attachment : pass.unorderedAccess) {
                if (!attachment.name.empty()) {
                    ++targetCount;
                    target = &attachment;
                }
            }
            if (!pass.depth.name.empty()) {
                ++targetCount;
                target = &pass.depth;
            }
            if (targetCount != 1 || target == nullptr)
                throw std::runtime_error("FX " + std::string(kind) + " pass requires one write target");
            const auto role = pass.functionalKind == core::EffectFunctionalPassKind::clearRtv
                                  ? FxResourceRole::colorAttachment
                                  : (!pass.depth.name.empty() && target == &pass.depth ? FxResourceRole::depthAttachment
                                                                                       : FxResourceRole::storage);
            appendOutput(*target, role);
        };
        switch (pass.type) {
        case core::EffectPassType::copy:
            if (pass.inputs.size() != 1 || pass.inputs.front().name.empty())
                throw std::runtime_error("FX copy pass requires one input and one output");
            appendInput(pass.inputs.front());
            appendUtilityTarget("copy");
            break;
        case core::EffectPassType::clear:
            appendUtilityTarget("clear");
            break;
        case core::EffectPassType::mipmap:
            appendUtilityTarget("mipmap");
            break;
        case core::EffectPassType::rasterizer:
        case core::EffectPassType::postprocess:
        case core::EffectPassType::compute:
        case core::EffectPassType::raytracing:
            for (const auto& attachment : pass.inputs)
                appendInput(attachment);
            for (const auto& attachment : pass.renderTargets)
                appendOutput(attachment, FxResourceRole::colorAttachment);
            for (const auto& attachment : pass.unorderedAccess)
                appendOutput(attachment, FxResourceRole::storage);
            if (!pass.depth.name.empty())
                appendOutput(pass.depth, FxResourceRole::depthAttachment);
            break;
        case core::EffectPassType::oidn: {
            const auto* oidn = std::get_if<FxOidnDispatch>(&dispatch.executable);
            if (oidn == nullptr)
                throw std::runtime_error("FX OIDN pass has no typed dispatch: " + pass.name);
            const auto appendUnique = [&](std::string_view name, bool write) {
                if (name.empty())
                    return;
                const auto duplicate =
                    std::ranges::find_if(dispatch.resources, [&](const FxDispatch::ResourceUse& resource) {
                        return resource.name == name && resource.write == write;
                    });
                if (duplicate == dispatch.resources.end())
                    dispatch.resources.push_back(
                        {std::string(name), write, write ? FxResourceRole::storage : FxResourceRole::sampled});
            };
            appendUnique(oidn->input, false);
            appendUnique(oidn->albedo, false);
            appendUnique(oidn->normal, false);
            appendUnique(oidn->output, true);
            break;
        }
        case core::EffectPassType::unknown:
            throw std::runtime_error("unsupported FX pass '" + pass.name + "': unknown type");
        }
        program.passes.push_back(std::move(dispatch));
    }
    if (program.passes.empty())
        throw std::runtime_error("FX graph contains no passes");
    return program;
}

FxProgram FxCompiler::compileSource(const FxSourceDocument& document) const {
    auto graph = parse(document);
    auto linked = link(graph);
    FxProgram program = compile(linked);
    program.sourceVersion = document.version;
    return program;
}

std::uint64_t FxInstance::beginReloadRequest() {
    std::scoped_lock lock(mutex_);
    if (requestSequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("FX reload request sequence exhausted");
    const auto request = requestSequence_++;
    newestRequestSequence_ = request;
    return request;
}

FxFramePlan FxCompiler::plan(const FxProgram& program, const FxFrameContext& context) const {
    FxFramePlan framePlan;
    framePlan.ordered = program.passes;
    framePlan.programGeneration = program.generation;
    framePlan.renderWidth = context.renderWidth;
    framePlan.renderHeight = context.renderHeight;
    FrameExtentTable extents;
    const auto addDeclaredSize = [&](std::string_view name, const core::EffectSize& size, std::uint32_t dimension,
                                     bool screenDefault) {
        const auto extent = resolveEffectSize(size, dimension, screenDefault, context, extents);
        extents.add(std::string(name), extent);
    };
    for (const auto& texture : program.textures)
        addDeclaredSize(texture.name, texture.size, 2, true);
    for (const auto& texture : program.textures3D)
        addDeclaredSize(texture.name, texture.size, 3, false);
    for (const auto& buffer : program.buffers) {
        auto size = buffer.size;
        if (!size.absolute && size.base.empty()) {
            size.base = program.category == core::fx::FxCategory::deform ? "CLONEDVERTEXCOUNT" : "DEFAULT_RTSIZE";
            size.dimension = program.category == core::fx::FxCategory::deform ? 1U : 2U;
        }
        addDeclaredSize(buffer.name, size, 1, false);
    }

    framePlan.resolved.reserve(program.passes.size());
    for (std::size_t index = 0; index < program.passes.size(); ++index) {
        const auto& dispatch = program.passes[index];
        FxResolvedPass resolved;
        resolved.sourceIndex = index;
        resolved.bindings = planPassBindings(program, dispatch, 0);
        resolved.raster = rasterTarget(dispatch);
        if (resolved.raster.has_value()) {
            if (resolved.raster->vertexBuffer.has_value())
                resolved.raster->vertexCount =
                    rasterBufferElementCount(program, *resolved.raster->vertexBuffer, context, extents);
            if (resolved.raster->indexBuffer.has_value()) {
                const auto declaration = std::ranges::find_if(
                    program.buffers, [&](const auto& buffer) { return buffer.name == *resolved.raster->indexBuffer; });
                if (declaration == program.buffers.end())
                    throw std::invalid_argument("FX raster index buffer is not declared: " +
                                                *resolved.raster->indexBuffer);
                if (declaration->elementSize != sizeof(std::uint32_t))
                    throw std::invalid_argument("FX raster index buffer must use 32-bit elements: " +
                                                declaration->name);
                resolved.raster->indexCount =
                    rasterBufferElementCount(program, *resolved.raster->indexBuffer, context, extents);
            }
        }

        auto explicitSize = dispatch.outputSize;
        if (explicitSize.base == "RTV[0]" || explicitSize.base == "RTV") {
            const auto target = firstOutputExtent(dispatch, FxResourceRole::colorAttachment, program, context, extents);
            if (target.has_value()) {
                explicitSize.base.clear();
                explicitSize.absolute = true;
                explicitSize.width = target->width;
                explicitSize.height = target->height;
                explicitSize.depth = target->depth;
                explicitSize.dimension = target->dimension;
            }
        } else if (explicitSize.base == "DSV") {
            const auto target = firstOutputExtent(dispatch, FxResourceRole::depthAttachment, program, context, extents);
            if (target.has_value()) {
                explicitSize.base.clear();
                explicitSize.absolute = true;
                explicitSize.width = target->width;
                explicitSize.height = target->height;
                explicitSize.depth = target->depth;
                explicitSize.dimension = target->dimension;
            }
        } else if (explicitSize.base == "UAV[0]" || explicitSize.base == "UAV") {
            const auto target = firstOutputExtent(dispatch, FxResourceRole::storage, program, context, extents);
            if (target.has_value()) {
                explicitSize.base.clear();
                explicitSize.absolute = true;
                explicitSize.width = target->width;
                explicitSize.height = target->height;
                explicitSize.depth = target->depth;
                explicitSize.dimension = target->dimension;
            }
        }

        std::optional<FxExtent3D> output;
        if (hasOutputSize(explicitSize)) {
            if (explicitSize.widthRatio == 1.0F && dispatch.outputWidthRatio != 1.0F)
                explicitSize.widthRatio = dispatch.outputWidthRatio;
            if (explicitSize.heightRatio == 1.0F && dispatch.outputHeightRatio != 1.0F)
                explicitSize.heightRatio = dispatch.outputHeightRatio;
            if (!explicitSize.absolute && explicitSize.base.empty()) {
                explicitSize.base =
                    dispatch.category == core::fx::FxCategory::deform ? "CLONEDVERTEXCOUNT" : "DEFAULT_RTSIZE";
                if (explicitSize.dimension == 0)
                    explicitSize.dimension = dispatch.category == core::fx::FxCategory::deform ? 1U : 2U;
            }
            const auto defaultDimension = dispatch.category == core::fx::FxCategory::deform ? 1U : 2U;
            output = fromFxExtent(resolveEffectSize(explicitSize, defaultDimension, false, context, extents));
        }
        if (!output.has_value())
            output = firstOutputExtent(dispatch, FxResourceRole::colorAttachment, program, context, extents);
        if (!output.has_value())
            output = firstOutputExtent(dispatch, FxResourceRole::depthAttachment, program, context, extents);
        if (!output.has_value())
            output = firstOutputExtent(dispatch, FxResourceRole::storage, program, context, extents);
        if (!output.has_value() && dispatch.category == core::fx::FxCategory::deform) {
            const auto count = context.clonedVertexCount != 0 ? context.clonedVertexCount : context.vertexCount;
            output = FxExtent3D{.width = static_cast<std::uint32_t>(std::max<std::size_t>(count, 1)),
                                .height = 1,
                                .depth = 1,
                                .dimension = 1};
        }
        if (!output.has_value())
            output = FxExtent3D{.width = std::max(context.renderWidth, 1U),
                                .height = std::max(context.renderHeight, 1U),
                                .depth = 1,
                                .dimension = 2};
        resolved.outputExtent = *output;
        if (dispatch.kind == FxOpKind::compute) {
            const auto dimension = output->dimension >= 1 && output->dimension <= 3
                                       ? output->dimension
                                       : (output->depth > 1 ? 3U : (output->height > 1 ? 2U : 1U));
            auto threads = dispatch.numThreads;
            const bool unspecified = threads[0] == 0 && threads[1] == 0 && threads[2] == 0;
            if (unspecified) {
                if (dimension == 1)
                    threads = {1024, 1, 1};
                else if (dimension == 2)
                    threads = {16, 16, 1};
                else
                    threads = {8, 8, 8};
            } else {
                for (auto& threadCount : threads)
                    threadCount = std::max(threadCount, 1U);
            }
            const auto totalThreads = static_cast<std::uint64_t>(threads[0]) * threads[1] * threads[2];
            if (totalThreads > 1024)
                throw std::invalid_argument("FX numthreads exceeds 1024 threads per group: " + dispatch.name);
            resolved.numThreads = threads;
            resolved.dispatchGroups = {.width = ceilDiv(output->width, threads[0]),
                                       .height = ceilDiv(output->height, threads[1]),
                                       .depth = ceilDiv(output->depth, threads[2])};
            if (resolved.dispatchGroups.width > 65535 || resolved.dispatchGroups.height > 65535 ||
                resolved.dispatchGroups.depth > 65535)
                throw std::invalid_argument("FX dispatch exceeds the upstream 65535 group limit: " + dispatch.name);
        }
        framePlan.resolved.push_back(std::move(resolved));
    }
    return framePlan;
}

FxRequiredFeatures requiredFeatures(const FxProgram& program) noexcept {
    FxRequiredFeatures required;
    for (const auto& dispatch : program.passes) {
        if (dispatch.kind == FxOpKind::raster || dispatch.kind == FxOpKind::postprocess)
            required.descriptorIndexing = true;
        if (dispatch.kind != FxOpKind::raytracing)
            continue;
        required.accelerationStructure = true;
        required.rayTracingPipeline = true;
        required.rayQuery = true;
        if (const auto* ray = std::get_if<FxRayTracingDispatch>(&dispatch.executable); ray != nullptr) {
            for (const auto& group : ray->hitGroups) {
                if (group.type == core::fx::FxRayTracingHitGroupType::procedural)
                    required.fragmentShaderBarycentric = true;
            }
        }
    }
    return required;
}

bool FxCompiler::buildPipelines(const FxProgram& program, std::string* error) const {
    if (error != nullptr)
        error->clear();
    const auto fail = [&](std::string message) {
        if (error != nullptr)
            *error = std::move(message);
        dayo::log::error("FxCompiler pipeline validation failed: ", error == nullptr ? "invalid dispatch" : *error);
        return false;
    };
    std::unordered_set<std::string> names;
    for (const auto& dispatch : program.passes) {
        if (dispatch.name.empty())
            return fail("fx pipeline: dispatch with empty name");
        if (!names.insert(dispatch.name).second)
            return fail("fx pipeline: duplicate dispatch name: " + dispatch.name);
        switch (dispatch.kind) {
        case FxOpKind::raster: {
            const auto* raster = std::get_if<FxRasterDispatch>(&dispatch.executable);
            if (raster == nullptr || raster->vertexShader.empty() || raster->pixelShader.empty())
                return fail("fx pipeline: raster dispatch is missing vertex or pixel shader: " + dispatch.name);
            break;
        }
        case FxOpKind::postprocess: {
            const auto* postprocess = std::get_if<FxPostProcessDispatch>(&dispatch.executable);
            if (postprocess == nullptr || postprocess->pixelShader.empty())
                return fail("fx pipeline: postprocess dispatch is missing pixel shader: " + dispatch.name);
            break;
        }
        case FxOpKind::compute: {
            const auto* compute = std::get_if<FxComputeDispatch>(&dispatch.executable);
            if (compute == nullptr || compute->computeShader.empty())
                return fail("fx pipeline: compute dispatch is missing compute shader: " + dispatch.name);
            break;
        }
        case FxOpKind::raytracing: {
            const auto* ray = std::get_if<FxRayTracingDispatch>(&dispatch.executable);
            if (ray == nullptr || ray->rayGenerationShader.empty())
                return fail("fx pipeline: ray-tracing dispatch is missing raygen shader: " + dispatch.name);
            break;
        }
        case FxOpKind::oidn: {
            const auto* oidn = std::get_if<FxOidnDispatch>(&dispatch.executable);
            if (oidn == nullptr || oidn->input.empty() || oidn->output.empty())
                return fail("fx pipeline: OIDN dispatch is missing input or output: " + dispatch.name);
            break;
        }
        case FxOpKind::copy:
        case FxOpKind::clear:
        case FxOpKind::mipmap:
            break;
        }
        for (const auto& resource : dispatch.resources) {
            if (resource.name.empty())
                return fail("fx pipeline: dispatch resource has an empty name: " + dispatch.name);
            if (resource.write && resource.role == FxResourceRole::sampled)
                return fail("fx pipeline: writable resource is marked sampled: " + resource.name);
        }
    }
    return true;
}

FxInstance::FxInstance(FxProgram initial, FxCompilerOptions options)
    : active_(std::make_shared<const FxProgram>(std::move(initial))),
      nextGeneration_(active_->generation == std::numeric_limits<std::uint64_t>::max() ? active_->generation
                                                                                       : active_->generation + 1U),
      compiler_(options) {
    if (active_->sourceVersion != 0) {
        newestSourceVersion_ = active_->sourceVersion;
        hasSourceVersion_ = true;
    }
}

std::shared_ptr<const FxProgram> FxInstance::active() const {
    std::scoped_lock lock(mutex_);
    return active_;
}

bool FxInstance::tryHotReload(const FxSourceDocument& document, const FxFrameContext& contextForPlan,
                              std::string* error) {
    // parse -> link -> compile -> plan -> pipeline; swap only on success.
    if (error != nullptr)
        error->clear();
    std::uint64_t request = 0;
    try {
        request = beginReloadRequest();
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
    core::EffectGraph graph;
    try {
        graph = compiler_.parse(document);
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = std::string("fx parse: ") + exception.what();
        dayo::log::warn("FxInstance hot reload parse failed, keeping current: ", exception.what());
        return false;
    }
    try {
        graph = compiler_.link(graph, error);
    } catch (const std::exception& exception) {
        if (error != nullptr && error->empty())
            *error = std::string("fx link: ") + exception.what();
        dayo::log::warn("FxInstance hot reload link failed, keeping current: ", exception.what());
        return false;
    }
    FxProgram candidate;
    try {
        candidate = compiler_.compile(graph);
        candidate.sourceVersion = document.version;
        // Plan validation before pipeline creation catches size/context errors.
        static_cast<void>(compiler_.plan(candidate, contextForPlan));
        std::string pipelineError;
        if (!compiler_.buildPipelines(candidate, &pipelineError)) {
            if (error != nullptr)
                *error = pipelineError;
            dayo::log::warn("FxInstance hot reload pipeline failed, keeping current: ", pipelineError);
            return false;
        }
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = std::string("fx compile: ") + exception.what();
        dayo::log::warn("FxInstance hot reload compile failed, keeping current: ", exception.what());
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        if (request != newestRequestSequence_) {
            if (error != nullptr)
                *error = "fx reload request is stale";
            return false;
        }
        if (document.version != 0 && hasSourceVersion_ && document.version <= newestSourceVersion_) {
            if (error != nullptr)
                *error = "fx reload source version is stale";
            return false;
        }
        candidate.generation = nextGeneration_++;
        if (document.version != 0) {
            newestSourceVersion_ = document.version;
            hasSourceVersion_ = true;
        }
        pending_ = std::move(candidate);
    }
    return true;
}

bool FxInstance::stagePending(const FxSourceDocument& document, std::string* error) {
    if (error != nullptr)
        error->clear();
    std::uint64_t request = 0;
    try {
        request = beginReloadRequest();
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
    try {
        FxProgram candidate = compiler_.compileSource(document);
        std::string pipelineError;
        if (!compiler_.buildPipelines(candidate, &pipelineError)) {
            if (error != nullptr)
                *error = pipelineError;
            return false;
        }
        std::scoped_lock lock(mutex_);
        if (request != newestRequestSequence_) {
            if (error != nullptr)
                *error = "fx reload request is stale";
            return false;
        }
        if (candidate.sourceVersion != 0 && hasSourceVersion_ && candidate.sourceVersion <= newestSourceVersion_) {
            if (error != nullptr)
                *error = "fx reload source version is stale";
            return false;
        }
        candidate.generation = nextGeneration_++;
        if (candidate.sourceVersion != 0) {
            newestSourceVersion_ = candidate.sourceVersion;
            hasSourceVersion_ = true;
        }
        pending_ = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr)
            *error = exception.what();
        return false;
    }
}

bool FxInstance::commitPendingAtFrameBoundary() {
    std::scoped_lock lock(mutex_);
    if (!pending_)
        return false;
    auto previous = active_;
    auto next = std::make_shared<const FxProgram>(std::move(*pending_));
    pending_.reset();
    active_ = std::move(next);
    if (previous) {
        retired_.push_back({std::move(previous), nextTimelineValue_});
        dayo::log::info("FxInstance swapped program generation ", active_->generation, " at frame boundary");
    }
    return true;
}

bool FxInstance::hasPending() const noexcept {
    std::scoped_lock lock(mutex_);
    return pending_.has_value();
}

void FxInstance::retireCompleted(std::uint64_t timelineCompleted) noexcept {
    std::scoped_lock lock(mutex_);
    std::vector<Retired> alive;
    alive.reserve(retired_.size());
    for (auto& entry : retired_) {
        if (entry.retireTimeline > timelineCompleted)
            alive.push_back(std::move(entry));
    }
    retired_.swap(alive);
}

std::size_t FxInstance::retiredCount() const noexcept {
    std::scoped_lock lock(mutex_);
    return retired_.size();
}

} // namespace dayo::fx
