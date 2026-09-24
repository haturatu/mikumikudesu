#include "graphics/fx_pipeline_runtime.hpp"

#include "fx/fx_shader_source.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dayo::graphics {
namespace {

void setError(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

std::string stageName(fx::FxShaderStage stage) {
    switch (stage) {
    case fx::FxShaderStage::vertex:
        return "vertex";
    case fx::FxShaderStage::fragment:
        return "fragment";
    case fx::FxShaderStage::compute:
        return "compute";
    case fx::FxShaderStage::rayGeneration:
        return "rayGeneration";
    case fx::FxShaderStage::miss:
        return "miss";
    case fx::FxShaderStage::closestHit:
        return "closestHit";
    case fx::FxShaderStage::anyHit:
        return "anyHit";
    case fx::FxShaderStage::intersection:
        return "intersection";
    case fx::FxShaderStage::callable:
        return "callable";
    }
    return "unknown";
}

ShaderStageMask deviceStage(fx::FxShaderStage stage) noexcept {
    switch (stage) {
    case fx::FxShaderStage::vertex:
        return ShaderStageMask::vertex;
    case fx::FxShaderStage::fragment:
        return ShaderStageMask::fragment;
    case fx::FxShaderStage::compute:
        return ShaderStageMask::compute;
    case fx::FxShaderStage::rayGeneration:
        return ShaderStageMask::rayGeneration;
    case fx::FxShaderStage::miss:
        return ShaderStageMask::miss;
    case fx::FxShaderStage::closestHit:
        return ShaderStageMask::closestHit;
    case fx::FxShaderStage::anyHit:
        return ShaderStageMask::anyHit;
    case fx::FxShaderStage::intersection:
        return ShaderStageMask::intersection;
    case fx::FxShaderStage::callable:
        return ShaderStageMask::callable;
    }
    return ShaderStageMask::none;
}

std::string macroKey(std::span<const std::string> macros) {
    std::ostringstream output;
    for (const auto& macro : macros)
        output << macro.size() << ':' << macro << ';';
    return output.str();
}

std::string passMacro(std::string_view name) {
    std::string result = "YRZ_PASS_";
    result.reserve(result.size() + name.size());
    for (const auto character : name) {
        const auto value = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(value) || character == '_' ? character : '_');
    }
    return result;
}

std::string includeDirectoryKey(std::span<const std::filesystem::path> directories) {
    std::ostringstream output;
    for (const auto& directory : directories)
        output << directory.lexically_normal().string().size() << ':' << directory.lexically_normal().string() << ';';
    return output.str();
}

std::string upper(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    return result;
}

PixelFormat textureFormat(std::string_view value) {
    const auto name = upper(value);
    if (name.empty() || name == "R8G8B8A8_UNORM")
        return PixelFormat::rgba8Unorm;
    if (name == "R8G8B8A8_SRGB")
        return PixelFormat::rgba8Srgb;
    if (name == "R16G16B16A16_FLOAT")
        return PixelFormat::rgba16Float;
    if (name == "R32G32B32A32_FLOAT")
        return PixelFormat::rgba32Float;
    if (name == "R8_UNORM")
        return PixelFormat::r8Unorm;
    if (name == "R16_FLOAT")
        return PixelFormat::r16Float;
    if (name == "R16G16_FLOAT")
        return PixelFormat::r16g16Float;
    if (name == "R32_FLOAT")
        return PixelFormat::r32Float;
    if (name == "R32G32_FLOAT")
        return PixelFormat::r32g32Float;
    if (name == "D32_FLOAT")
        return PixelFormat::depth32Float;
    if (name == "D24_UNORM_S8_UINT" || name == "D24S8")
        return PixelFormat::depth24Stencil8;
    throw std::invalid_argument("FX graphics target format is unsupported: " + std::string(value));
}

CullModeEx cullMode(core::EffectCullMode mode) {
    switch (mode) {
    case core::EffectCullMode::none:
        return CullModeEx::none;
    case core::EffectCullMode::front:
        return CullModeEx::front;
    case core::EffectCullMode::back:
        return CullModeEx::back;
    }
    throw std::invalid_argument("unsupported FX cull mode");
}

FrontFaceEx frontFace(core::EffectFrontFace face) {
    return face == core::EffectFrontFace::clockwise ? FrontFaceEx::clockwise : FrontFaceEx::counterClockwise;
}

CompareOpEx compareOp(core::EffectDepthFunc function) {
    switch (function) {
    case core::EffectDepthFunc::never:
        return CompareOpEx::never;
    case core::EffectDepthFunc::less:
        return CompareOpEx::less;
    case core::EffectDepthFunc::equal:
        return CompareOpEx::equal;
    case core::EffectDepthFunc::lessEqual:
        return CompareOpEx::lessOrEqual;
    case core::EffectDepthFunc::greater:
        return CompareOpEx::greater;
    case core::EffectDepthFunc::notEqual:
        return CompareOpEx::notEqual;
    case core::EffectDepthFunc::greaterEqual:
        return CompareOpEx::greaterOrEqual;
    case core::EffectDepthFunc::always:
        return CompareOpEx::always;
    }
    throw std::invalid_argument("unsupported FX depth compare operation");
}

CompareOpEx compareOp(core::FxCompareOp function) {
    switch (function) {
    case core::FxCompareOp::never:
        return CompareOpEx::never;
    case core::FxCompareOp::less:
        return CompareOpEx::less;
    case core::FxCompareOp::equal:
        return CompareOpEx::equal;
    case core::FxCompareOp::lessEqual:
        return CompareOpEx::lessOrEqual;
    case core::FxCompareOp::greater:
        return CompareOpEx::greater;
    case core::FxCompareOp::notEqual:
        return CompareOpEx::notEqual;
    case core::FxCompareOp::greaterEqual:
        return CompareOpEx::greaterOrEqual;
    case core::FxCompareOp::always:
        return CompareOpEx::always;
    }
    throw std::invalid_argument("unsupported FX stencil compare operation");
}

StencilOpEx stencilOp(core::FxStencilOp operation) {
    switch (operation) {
    case core::FxStencilOp::keep:
        return StencilOpEx::keep;
    case core::FxStencilOp::zero:
        return StencilOpEx::zero;
    case core::FxStencilOp::replace:
        return StencilOpEx::replace;
    case core::FxStencilOp::incrementClamp:
        return StencilOpEx::incrementClamp;
    case core::FxStencilOp::decrementClamp:
        return StencilOpEx::decrementClamp;
    case core::FxStencilOp::invert:
        return StencilOpEx::invert;
    case core::FxStencilOp::incrementWrap:
        return StencilOpEx::incrementWrap;
    case core::FxStencilOp::decrementWrap:
        return StencilOpEx::decrementWrap;
    }
    throw std::invalid_argument("unsupported FX stencil operation");
}

LogicOpEx logicOp(core::FxLogicOp operation) {
    return static_cast<LogicOpEx>(operation);
}

VertexInputRateEx vertexInputRate(core::EffectVertexInputRate rate) {
    switch (rate) {
    case core::EffectVertexInputRate::vertex:
        return VertexInputRateEx::vertex;
    case core::EffectVertexInputRate::instance:
        return VertexInputRateEx::instance;
    }
    throw std::invalid_argument("unsupported FX vertex input rate");
}

VertexInputFormatEx vertexInputFormat(core::EffectVertexFormat format) {
    switch (format) {
    case core::EffectVertexFormat::r32Float:
        return VertexInputFormatEx::r32Sfloat;
    case core::EffectVertexFormat::r32g32Float:
        return VertexInputFormatEx::r32g32Sfloat;
    case core::EffectVertexFormat::r32g32b32Float:
        return VertexInputFormatEx::r32g32b32Sfloat;
    case core::EffectVertexFormat::r32g32b32a32Float:
        return VertexInputFormatEx::r32g32b32a32Sfloat;
    }
    throw std::invalid_argument("unsupported FX vertex input format");
}

std::uint32_t vertexInputFormatSize(VertexInputFormatEx format) {
    switch (format) {
    case VertexInputFormatEx::r32Sfloat:
        return 4;
    case VertexInputFormatEx::r32g32Sfloat:
        return 8;
    case VertexInputFormatEx::r32g32b32Sfloat:
        return 12;
    case VertexInputFormatEx::r32g32b32a32Sfloat:
        return 16;
    }
    throw std::invalid_argument("unsupported FX vertex input format");
}

std::string compact(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        if (character != '_' && character != '-')
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

BlendFactorEx blendFactor(std::string_view value) {
    const auto key = compact(value);
    if (key.empty() || key == "one")
        return BlendFactorEx::one;
    if (key == "zero")
        return BlendFactorEx::zero;
    if (key == "srccolor")
        return BlendFactorEx::srcColor;
    if (key == "invsrccolor")
        return BlendFactorEx::oneMinusSrcColor;
    if (key == "destcolor")
        return BlendFactorEx::dstColor;
    if (key == "invdestcolor")
        return BlendFactorEx::oneMinusDstColor;
    if (key == "srcalpha")
        return BlendFactorEx::srcAlpha;
    if (key == "invsrcalpha")
        return BlendFactorEx::oneMinusSrcAlpha;
    if (key == "destalpha")
        return BlendFactorEx::dstAlpha;
    if (key == "invdestalpha")
        return BlendFactorEx::oneMinusDstAlpha;
    if (key == "srcalphasaturate")
        return BlendFactorEx::srcAlphaSaturate;
    throw std::invalid_argument("unsupported FX blend factor: " + std::string(value));
}

BlendOpEx blendOp(std::string_view value) {
    const auto key = compact(value);
    if (key.empty() || key == "add")
        return BlendOpEx::add;
    if (key == "subtract")
        return BlendOpEx::subtract;
    if (key == "revsubtract")
        return BlendOpEx::reverseSubtract;
    if (key == "min")
        return BlendOpEx::min;
    if (key == "max")
        return BlendOpEx::max;
    throw std::invalid_argument("unsupported FX blend operation: " + std::string(value));
}

const core::EffectTexture* findTexture(const fx::FxProgram& program, std::string_view name) {
    const auto found = std::find_if(program.textures.begin(), program.textures.end(),
                                    [name](const auto& declaration) { return declaration.name == name; });
    return found == program.textures.end() ? nullptr : &*found;
}

PixelFormat attachmentFormat(const fx::FxProgram& program, const core::EffectAttachment& attachment, bool depth) {
    const auto* texture = findTexture(program, attachment.name);
    if (texture != nullptr && !texture->format.empty())
        return textureFormat(texture->format);
    return depth ? PixelFormat::depth32Float : PixelFormat::rgba16Float;
}

std::vector<PixelFormat> graphicsTargetFormats(const fx::FxProgram& program, const fx::FxDispatch& dispatch) {
    std::vector<PixelFormat> result;
    if (const auto* raster = std::get_if<fx::FxRasterDispatch>(&dispatch.executable); raster != nullptr) {
        result.reserve(raster->colorAttachments.size());
        for (const auto& attachment : raster->colorAttachments)
            if (!attachment.name.empty())
                result.push_back(attachmentFormat(program, attachment, false));
    } else if (const auto* postprocess = std::get_if<fx::FxPostProcessDispatch>(&dispatch.executable);
               postprocess != nullptr) {
        result.reserve(postprocess->colorAttachments.size());
        for (const auto& attachment : postprocess->colorAttachments)
            if (!attachment.name.empty())
                result.push_back(attachmentFormat(program, attachment, false));
    }
    if (result.empty()) {
        for (const auto& resource : dispatch.resources) {
            if (!resource.write || resource.role != fx::FxResourceRole::colorAttachment)
                continue;
            const auto* texture = findTexture(program, resource.name);
            result.push_back(texture == nullptr || texture->format.empty() ? PixelFormat::rgba16Float
                                                                           : textureFormat(texture->format));
        }
    }
    return result;
}

std::optional<PixelFormat> graphicsDepthFormat(const fx::FxProgram& program, const fx::FxDispatch& dispatch) {
    if (const auto* raster = std::get_if<fx::FxRasterDispatch>(&dispatch.executable);
        raster != nullptr && raster->depthAttachment.has_value() && !raster->depthAttachment->name.empty())
        return attachmentFormat(program, *raster->depthAttachment, true);
    for (const auto& resource : dispatch.resources)
        if (resource.write && resource.role == fx::FxResourceRole::depthAttachment)
            return attachmentFormat(program, {.name = resource.name}, true);
    return std::nullopt;
}

GraphicsPipelineDescEx graphicsPipelineDescriptor(const fx::FxProgram& program, const fx::FxDispatch& dispatch,
                                                  handles::PipelineLayoutHandle layout,
                                                  std::vector<handles::ShaderHandle> shaders) {
    GraphicsPipelineDescEx descriptor;
    descriptor.layout = layout;
    descriptor.shaders = std::move(shaders);
    descriptor.colorFormats = graphicsTargetFormats(program, dispatch);
    descriptor.depthFormat = graphicsDepthFormat(program, dispatch);
    descriptor.depthOnly = descriptor.colorFormats.empty() && descriptor.depthFormat.has_value();
    if (descriptor.colorFormats.empty() && !descriptor.depthOnly &&
        (dispatch.kind == fx::FxOpKind::raster || dispatch.kind == fx::FxOpKind::postprocess))
        descriptor.colorFormats.push_back(PixelFormat::rgba16Float);
    const auto* graphics = std::get_if<fx::FxRasterDispatch>(&dispatch.executable);
    if (graphics != nullptr) {
        descriptor.vertexBindings.reserve(graphics->vertexLayout.bindings.size());
        for (const auto& binding : graphics->vertexLayout.bindings) {
            if (binding.stride == 0 || std::ranges::any_of(descriptor.vertexBindings, [&](const auto& candidate) {
                    return candidate.binding == binding.binding;
                }))
                throw std::invalid_argument("FX vertex input binding has a zero stride or duplicate slot: " +
                                            dispatch.name);
            descriptor.vertexBindings.push_back(
                {.binding = binding.binding, .stride = binding.stride, .rate = vertexInputRate(binding.rate)});
        }
        descriptor.vertexAttributes.reserve(graphics->vertexLayout.attributes.size());
        for (const auto& attribute : graphics->vertexLayout.attributes) {
            if (std::ranges::any_of(descriptor.vertexAttributes,
                                    [&](const auto& candidate) { return candidate.location == attribute.location; }))
                throw std::invalid_argument("FX vertex input attributes have duplicate locations: " + dispatch.name);
            const auto format = vertexInputFormat(attribute.format);
            const auto binding = std::ranges::find_if(descriptor.vertexBindings, [&](const auto& candidate) {
                return candidate.binding == attribute.binding;
            });
            if (binding == descriptor.vertexBindings.end())
                throw std::invalid_argument("FX vertex input attribute references an undeclared binding: " +
                                            dispatch.name);
            if (static_cast<std::uint64_t>(attribute.offset) + vertexInputFormatSize(format) > binding->stride)
                throw std::invalid_argument("FX vertex input attribute exceeds its binding stride: " + dispatch.name);
            descriptor.vertexAttributes.push_back({.location = attribute.location,
                                                   .binding = attribute.binding,
                                                   .format = format,
                                                   .offset = attribute.offset});
        }
        descriptor.rasterizer.cullMode = cullMode(graphics->graphics.rasterizer.cullMode);
        descriptor.rasterizer.frontFace = frontFace(graphics->graphics.rasterizer.frontFace);
        descriptor.depthStencil.depthTest =
            descriptor.depthFormat.has_value() && graphics->graphics.depthStencil.depthEnable;
        descriptor.depthStencil.depthWrite =
            descriptor.depthStencil.depthTest && graphics->graphics.depthStencil.depthWrite;
        descriptor.depthStencil.depthCompare = compareOp(graphics->graphics.depthStencil.depthFunc);
        descriptor.depthStencil.stencilTest = graphics->graphics.depthStencil.stencilEnable;
        descriptor.depthStencil.stencilReadMask = graphics->graphics.depthStencil.stencilReadMask;
        descriptor.depthStencil.stencilWriteMask = graphics->graphics.depthStencil.stencilWriteMask;
        const auto stencilState = [](const core::EffectDepthStencilState::StencilFace& source) {
            return StencilOpStateEx{.fail = stencilOp(source.fail),
                                    .pass = stencilOp(source.pass),
                                    .depthFail = stencilOp(source.depthFail),
                                    .compare = compareOp(source.compare)};
        };
        descriptor.depthStencil.front = stencilState(graphics->graphics.depthStencil.front);
        descriptor.depthStencil.back = stencilState(graphics->graphics.depthStencil.back);
        descriptor.alphaToCoverage = graphics->graphics.alphaToCoverage;
        descriptor.independentBlend = graphics->graphics.independentBlend;
        descriptor.logicOpEnable = graphics->graphics.logicOpEnable;
        descriptor.logicOp = logicOp(graphics->graphics.logicOp);
        if (graphics->graphics.blend.size() > descriptor.colorFormats.size())
            throw std::invalid_argument("FX blend attachment count exceeds color attachment count");
        descriptor.blendAttachments.resize(descriptor.colorFormats.size());
        for (std::size_t index = 0; index < graphics->graphics.blend.size(); ++index) {
            const auto& state = graphics->graphics.blend[index];
            descriptor.blendAttachments[index] = {.enabled = state.enabled,
                                                  .srcColor = blendFactor(state.srcColor),
                                                  .dstColor = blendFactor(state.dstColor),
                                                  .colorOp = blendOp(state.colorOp),
                                                  .srcAlpha = blendFactor(state.srcAlpha),
                                                  .dstAlpha = blendFactor(state.dstAlpha),
                                                  .alphaOp = blendOp(state.alphaOp)};
            descriptor.blendAttachments[index].colorWriteMask = state.colorWriteMask;
        }
    }
    return descriptor;
}

} // namespace

GraphicsPipelineDescEx makeGraphicsPipelineDescriptor(const fx::FxProgram& program, const fx::FxDispatch& dispatch,
                                                      handles::PipelineLayoutHandle layout,
                                                      std::vector<handles::ShaderHandle> shaders) {
    return graphicsPipelineDescriptor(program, dispatch, layout, std::move(shaders));
}

FxPipelineRuntime::~FxPipelineRuntime() {
    reset();
}

handles::ShaderHandle FxPipelineRuntime::compileShader(Device& device, const fx::FxProgram& program,
                                                       const fx::FxDispatch& dispatch,
                                                       const fx::FxResolvedPass& resolved, std::string_view entryPoint,
                                                       fx::FxShaderStage stage, const fx::FxShaderCompiler& compiler,
                                                       Entry& entry, std::uint32_t resourceSet,
                                                       const fx::FxNativeShaderSourceOptions& sourceOptions) {
    if (entryPoint.empty())
        throw std::invalid_argument("FX shader entry point is empty for pass " + dispatch.name);
    if (program.hlsl.empty())
        throw std::invalid_argument("FX program has no HLSL source for pass " + dispatch.name);

    fx::FxShaderKey key;
    fx::FxShaderCompileRequest request;
    // HLSL standalone globals become an implicit $Globals cbuffer whether or
    // not the effect explicitly declares globalVarSize. Effects using the
    // controller ABI bind the host global buffer at b2, so their shaders must
    // use DXC's explicit -fvk-bind-globals mapping. Keep glslc available for
    // synthetic/legacy effects that do not consume this ABI.
    request.requireDxcForNativeFxAbi =
        program.globalVarSizeSpecified || !program.controllers.empty() || !sourceOptions.controllerDeclarations.empty();
    request.macros = dispatch.macros;
    request.macros.push_back(passMacro(dispatch.name));
    const auto generatedSource = fx::makeNativeFxShaderSource(program, dispatch, resourceSet, sourceOptions, &resolved);
    key.sourceHash =
        program.sourcePath.string() + "@" + std::to_string(program.sourceVersion) + "@" + std::to_string(resourceSet);
    key.hlslHash = std::to_string(std::hash<std::string>{}(generatedSource));
    key.entryPoint = std::string(entryPoint);
    key.stage = stageName(stage);
    key.dxcVersion = compiler.executable().string();
    key.spirvTarget = "vulkan1.3";
    key.compatProfile = "fx-native";
    key.macros = macroKey(request.macros);

    // The compiler writes generated HLSL to a temporary directory. Add the
    // effect's directory explicitly so relative includes retain the same
    // resolution they have in the source tree.
    if (!program.sourcePath.empty()) {
        const auto sourceDirectory = program.sourcePath.parent_path();
        request.includeDirectories.push_back(sourceDirectory.empty() ? std::filesystem::path{"."} : sourceDirectory);
    }
    key.includeDirectories = includeDirectoryKey(request.includeDirectories);

    const auto sourceDirectory =
        program.sourcePath.empty() ? std::filesystem::path{"."} : program.sourcePath.parent_path();
    request.hlsl = fx::normalizeFxShaderIncludes(generatedSource, sourceDirectory.empty() ? std::filesystem::path{"."}
                                                                                          : sourceDirectory);
    request.sourcePath = program.sourcePath;
    request.entryPoint = std::string(entryPoint);
    request.stage = stage;
    const auto artifact = shaderCache_.compileOrGet(key, request, compiler);
    const auto shader = device.createShaderEx({
        .spirv = std::span<const std::uint32_t>(artifact.spirv.data(), artifact.spirv.size()),
        .entryPoint = std::string(entryPoint),
        .stage = deviceStage(stage),
    });
    entry.shaders.push_back(shader);
    return shader;
}

bool FxPipelineRuntime::build(Device& device, const fx::FxProgram& program, const fx::FxFramePlan& framePlan,
                              const fx::FxShaderCompiler& compiler, const LayoutResolver& resolveLayout,
                              std::string* error, std::uint32_t resourceSet,
                              const fx::FxNativeShaderSourceOptions& sourceOptions) {
    if (error != nullptr)
        error->clear();
    reset();
    if (!resolveLayout) {
        setError(error, "FX pipeline build requires a pipeline-layout resolver");
        return false;
    }
    if (framePlan.resolved.size() != program.passes.size()) {
        setError(error, "FX pipeline build requires one resolved pass per dispatch");
        return false;
    }
    device_ = &device;
    try {
        for (std::size_t passIndex = 0; passIndex < program.passes.size(); ++passIndex) {
            const auto& dispatch = program.passes[passIndex];
            const auto& resolved = framePlan.resolved[passIndex];
            if (dispatch.kind == fx::FxOpKind::copy || dispatch.kind == fx::FxOpKind::clear ||
                dispatch.kind == fx::FxOpKind::mipmap || dispatch.kind == fx::FxOpKind::oidn)
                continue;
            if (dispatch.name.empty())
                throw std::invalid_argument("FX pipeline pass has an empty name");
            if (entries_.contains(dispatch.name))
                throw std::invalid_argument("FX pipeline pass name is duplicated: " + dispatch.name);
            const auto layout = resolveLayout(dispatch);
            if (!layout.has_value() || !layout->valid())
                throw std::invalid_argument("FX pipeline layout is unavailable: " + dispatch.name);

            Entry entry;
            switch (dispatch.kind) {
            case fx::FxOpKind::raster: {
                const auto* raster = std::get_if<fx::FxRasterDispatch>(&dispatch.executable);
                if (raster == nullptr || raster->vertexShader.empty() || raster->pixelShader.empty())
                    throw std::invalid_argument("raster FX pass requires vertex and pixel shaders: " + dispatch.name);
                const auto vertex =
                    compileShader(device, program, dispatch, resolved, raster->vertexShader, fx::FxShaderStage::vertex,
                                  compiler, entry, resourceSet, sourceOptions);
                const auto pixel =
                    compileShader(device, program, dispatch, resolved, raster->pixelShader, fx::FxShaderStage::fragment,
                                  compiler, entry, resourceSet, sourceOptions);
                entry.pipeline = device.createGraphicsPipelineEx(
                    makeGraphicsPipelineDescriptor(program, dispatch, *layout, {vertex, pixel}));
                break;
            }
            case fx::FxOpKind::postprocess: {
                const auto* postprocess = std::get_if<fx::FxPostProcessDispatch>(&dispatch.executable);
                if (postprocess == nullptr || postprocess->pixelShader.empty())
                    throw std::invalid_argument("postprocess FX pass requires a pixel shader: " + dispatch.name);
                const auto fullscreenVertex = device.nativeFullscreenVertexShader();
                if (!fullscreenVertex.valid())
                    throw std::invalid_argument(
                        "postprocess FX pipeline needs a renderer-owned fullscreen vertex shader: " + dispatch.name);
                const auto pixel =
                    compileShader(device, program, dispatch, resolved, postprocess->pixelShader,
                                  fx::FxShaderStage::fragment, compiler, entry, resourceSet, sourceOptions);
                entry.pipeline = device.createGraphicsPipelineEx(
                    makeGraphicsPipelineDescriptor(program, dispatch, *layout, {fullscreenVertex, pixel}));
                break;
            }
            case fx::FxOpKind::compute: {
                const auto* compute = std::get_if<fx::FxComputeDispatch>(&dispatch.executable);
                if (compute == nullptr || compute->computeShader.empty())
                    throw std::invalid_argument("compute FX pass requires a compute shader: " + dispatch.name);
                const auto shader =
                    compileShader(device, program, dispatch, resolved, compute->computeShader,
                                  fx::FxShaderStage::compute, compiler, entry, resourceSet, sourceOptions);
                entry.pipeline = device.createComputePipelineEx({.layout = *layout, .shaders = {shader}});
                break;
            }
            case fx::FxOpKind::raytracing: {
                const auto* ray = std::get_if<fx::FxRayTracingDispatch>(&dispatch.executable);
                if (ray == nullptr || ray->rayGenerationShader.empty())
                    throw std::invalid_argument("ray-tracing FX pass requires a raygen shader: " + dispatch.name);
                RayTracingPipelineDescEx descriptor;
                descriptor.layout = *layout;
                descriptor.maxPayloadSize = ray->maxPayloadSize;
                descriptor.maxAttributeSize = ray->maxAttributeSize;
                descriptor.maxRecursionDepth = ray->maxRecursionDepth;
                descriptor.rayGeneration.push_back(
                    compileShader(device, program, dispatch, resolved, ray->rayGenerationShader,
                                  fx::FxShaderStage::rayGeneration, compiler, entry, resourceSet, sourceOptions));
                for (const auto& shader : ray->missShaders)
                    descriptor.miss.push_back(compileShader(device, program, dispatch, resolved, shader,
                                                            fx::FxShaderStage::miss, compiler, entry, resourceSet,
                                                            sourceOptions));
                for (const auto& group : ray->hitGroups) {
                    RayTracingHitGroupDesc hit;
                    hit.type = group.type == core::fx::FxRayTracingHitGroupType::procedural
                                   ? RayTracingHitGroupType::procedural
                                   : RayTracingHitGroupType::triangles;
                    if (!group.closestHit.empty())
                        hit.closestHit =
                            compileShader(device, program, dispatch, resolved, group.closestHit,
                                          fx::FxShaderStage::closestHit, compiler, entry, resourceSet, sourceOptions);
                    if (!group.anyHit.empty())
                        hit.anyHit =
                            compileShader(device, program, dispatch, resolved, group.anyHit, fx::FxShaderStage::anyHit,
                                          compiler, entry, resourceSet, sourceOptions);
                    if (!group.intersection.empty())
                        hit.intersection =
                            compileShader(device, program, dispatch, resolved, group.intersection,
                                          fx::FxShaderStage::intersection, compiler, entry, resourceSet, sourceOptions);
                    descriptor.hitGroups.push_back(hit);
                }
                for (const auto& shader : ray->callableShaders)
                    descriptor.callable.push_back(compileShader(device, program, dispatch, resolved, shader,
                                                                fx::FxShaderStage::callable, compiler, entry,
                                                                resourceSet, sourceOptions));
                entry.pipeline = device.createRayTracingPipelineEx(descriptor);
                entry.sbt = device.createShaderBindingTable({
                    .pipeline = entry.pipeline,
                    .raygenCount = static_cast<std::uint32_t>(descriptor.rayGeneration.size()),
                    .missCount = static_cast<std::uint32_t>(descriptor.miss.size()),
                    .hitCount = static_cast<std::uint32_t>(descriptor.hitGroups.size()),
                    .callableCount = static_cast<std::uint32_t>(descriptor.callable.size()),
                });
                break;
            }
            case fx::FxOpKind::copy:
            case fx::FxOpKind::clear:
            case fx::FxOpKind::mipmap:
            case fx::FxOpKind::oidn:
                break;
            }
            entries_.emplace(dispatch.name, std::move(entry));
        }
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        reset();
        return false;
    } catch (...) {
        setError(error, "FX pipeline build failed");
        reset();
        return false;
    }
    return true;
}

bool FxPipelineRuntime::build(Device& device, const fx::FxProgram& program, const fx::FxShaderCompiler& compiler,
                              const LayoutResolver& resolveLayout, std::string* error, std::uint32_t resourceSet,
                              const fx::FxNativeShaderSourceOptions& sourceOptions) {
    const auto context = fx::makeFxFrameContext(0.0F, 0, 1, 1, 0, 0, 1, 1, 1, program.meshCloneCount);
    const auto framePlan = fx::FxCompiler{}.plan(program, context);
    return build(device, program, framePlan, compiler, resolveLayout, error, resourceSet, sourceOptions);
}

void FxPipelineRuntime::destroyEntry(const Entry& entry) noexcept {
    if (device_ == nullptr)
        return;
    try {
        if (entry.sbt.valid())
            device_->destroyShaderBindingTable(entry.sbt);
        if (entry.pipeline.valid())
            device_->destroyPipelineEx(entry.pipeline);
        for (const auto shader : entry.shaders)
            if (shader.valid())
                device_->destroyShaderEx(shader);
    } catch (...) {
        // Runtime teardown must not hide the original pipeline error.
    }
}

void FxPipelineRuntime::reset() noexcept {
    if (device_ != nullptr) {
        try {
            device_->waitIdle();
        } catch (...) {
        }
        for (const auto& [name, entry] : entries_) {
            static_cast<void>(name);
            destroyEntry(entry);
        }
    }
    entries_.clear();
    device_ = nullptr;
}

std::optional<handles::PipelineHandle> FxPipelineRuntime::resolvePipeline(const fx::FxDispatch& dispatch) const {
    const auto it = entries_.find(dispatch.name);
    if (it == entries_.end() || !it->second.pipeline.valid())
        return std::nullopt;
    return it->second.pipeline;
}

std::optional<handles::ShaderBindingTableHandle>
FxPipelineRuntime::resolveShaderBindingTable(const fx::FxDispatch& dispatch) const {
    const auto it = entries_.find(dispatch.name);
    if (it == entries_.end() || !it->second.sbt.valid())
        return std::nullopt;
    return it->second.sbt;
}

} // namespace dayo::graphics
