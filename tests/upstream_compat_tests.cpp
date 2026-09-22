#include "core/animation.hpp"
#include "core/asset.hpp"
#include "core/effect.hpp"
#include "core/fx/fx_pass.hpp"
#include "core/image.hpp"
#include "core/motion.hpp"
#include "fx/fx_compiler.hpp"
#include "fx/fx_frame.hpp"
#include "fx/fx_runtime_requirements.hpp"
#include "fx/fx_shader_compiler.hpp"
#include "fx/fx_shader_source.hpp"
#include "graphics/fx_pipeline_runtime.hpp"
#include "graphics/native_scene_bindings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

std::vector<std::filesystem::path> upstreamFxFiles(const std::filesystem::path& sourceDirectory) {
    std::vector<std::filesystem::path> files;
    for (const auto relativeRoot : {"renderer", "postprocess", "particle", "sample"}) {
        const auto root = sourceDirectory / relativeRoot;
        if (!std::filesystem::is_directory(root))
            throw std::runtime_error("missing upstream FX directory: " + root.string());
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".fxdayo")
                files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

bool upstreamShaderProbesRequired() {
    const auto* value = std::getenv("DAYO_UPSTREAM_REQUIRE_DXC");
    return value != nullptr && std::string_view(value) == "1";
}

std::string passMacro(std::string_view name) {
    std::string result = "YRZ_PASS_";
    for (const auto character : name) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(std::isalnum(byte) || character == '_' ? character : '_');
    }
    if (result.size() == std::string_view{"YRZ_PASS_"}.size() || std::isdigit(static_cast<unsigned char>(result[9])))
        result.insert(result.begin() + 9, '_');
    return result;
}

std::vector<std::filesystem::path> shaderIncludeDirectories(const std::filesystem::path& sourceDirectory,
                                                            const std::filesystem::path& effectPath) {
    std::vector<std::filesystem::path> directories;
    const auto add = [&](const std::filesystem::path& path) {
        if (path.empty() || !std::filesystem::is_directory(path))
            return;
        if (std::ranges::find(directories, path) == directories.end())
            directories.push_back(path);
    };
    for (auto current = effectPath.parent_path(); !current.empty() && current != sourceDirectory.parent_path();
         current = current.parent_path()) {
        add(current);
        if (current == sourceDirectory)
            break;
    }
    add(sourceDirectory / "hlsl");
    add(sourceDirectory);
    return directories;
}

struct UpstreamScanResult {
    std::size_t graphCount{};
    std::size_t passCount{};
    std::size_t shaderCount{};
    std::map<dayo::fx::FxRuntimeFeature, std::size_t> featureCounts;
    std::vector<std::string> failures;
};

class PipelineOracleDevice final : public dayo::graphics::Device {
  public:
    [[nodiscard]] const dayo::graphics::DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    [[nodiscard]] const dayo::graphics::GraphicsConvention& convention() const noexcept override {
        return convention_;
    }
    [[nodiscard]] dayo::graphics::RendererKind activeRenderer() const noexcept override {
        return dayo::graphics::RendererKind::preview;
    }
    [[nodiscard]] dayo::graphics::handles::ShaderHandle nativeFullscreenVertexShader() const noexcept override {
        return {900, 1};
    }
    void selectRenderer(dayo::graphics::RendererKind) override {}
    void resize() override {}
    void beginUiFrame() override {}
    void renderFrame() override {}
    void waitIdle() override {}
    void uploadPreviewMesh(std::span<const dayo::graphics::PreviewVertex>, std::span<const std::uint32_t>) override {}
    void updatePreviewVertices(std::span<const dayo::graphics::PreviewVertex>) override {}
    void updatePreviewBones(std::span<const dayo::graphics::PreviewBoneTransform>) override {}
    void uploadPreviewMorphDeltas(std::span<const dayo::graphics::PreviewMorphDelta>) override {}
    void updatePreviewMorphWeights(std::span<const float>) override {}
    void updatePreviewMaterials(std::span<const dayo::graphics::PreviewMaterial>) override {}
    void updatePreviewDraws(std::span<const dayo::graphics::PreviewDraw>) override {}
    void uploadPreviewTextures(std::span<const dayo::graphics::PreviewTexture>) override {}
    void uploadPreviewBackground(std::span<const dayo::graphics::PreviewTexture>) override {}
    void clearPreviewResources() override {}
    void updatePreviewScene(const dayo::graphics::PreviewScene&) override {}
    [[nodiscard]] dayo::graphics::BufferHandle createBuffer(const dayo::graphics::BufferDesc&) override {
        return nextLegacyHandle_++;
    }
    [[nodiscard]] dayo::graphics::TextureHandle createTexture(const dayo::graphics::TextureDesc&) override {
        return nextLegacyHandle_++;
    }
    [[nodiscard]] dayo::graphics::handles::ShaderHandle createShaderEx(const dayo::graphics::ShaderDesc&) override {
        return {nextTypedHandle_++, 1};
    }
    void destroyShaderEx(dayo::graphics::handles::ShaderHandle) override {}
    [[nodiscard]] dayo::graphics::handles::PipelineHandle
    createGraphicsPipelineEx(const dayo::graphics::GraphicsPipelineDescEx& descriptor) override {
        graphicsPipelines_.push_back(descriptor);
        return {nextTypedHandle_++, 1};
    }
    [[nodiscard]] dayo::graphics::handles::PipelineHandle
    createComputePipelineEx(const dayo::graphics::ComputePipelineDescEx&) override {
        return {nextTypedHandle_++, 1};
    }
    [[nodiscard]] dayo::graphics::handles::PipelineHandle
    createRayTracingPipelineEx(const dayo::graphics::RayTracingPipelineDescEx&) override {
        return {nextTypedHandle_++, 1};
    }
    void destroyPipelineEx(dayo::graphics::handles::PipelineHandle) override {}
    [[nodiscard]] const std::vector<dayo::graphics::GraphicsPipelineDescEx>& graphicsPipelines() const noexcept {
        return graphicsPipelines_;
    }
    [[nodiscard]] dayo::graphics::handles::ShaderBindingTableHandle
    createShaderBindingTable(const dayo::graphics::ShaderBindingTableDesc&) override {
        return {nextTypedHandle_++, 1};
    }
    void destroyShaderBindingTable(dayo::graphics::handles::ShaderBindingTableHandle) override {}

  private:
    dayo::graphics::DeviceCapabilities capabilities_;
    dayo::graphics::GraphicsConvention convention_;
    std::vector<dayo::graphics::GraphicsPipelineDescEx> graphicsPipelines_;
    std::uint64_t nextLegacyHandle_{1};
    std::uint32_t nextTypedHandle_{1};
};

std::string compactPipelineValue(std::string_view value) {
    std::string result;
    for (const auto character : value) {
        if (character != '_' && character != '-')
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

std::optional<dayo::graphics::CullModeEx> oracleCullMode(dayo::core::EffectCullMode mode) {
    switch (mode) {
    case dayo::core::EffectCullMode::none:
        return dayo::graphics::CullModeEx::none;
    case dayo::core::EffectCullMode::front:
        return dayo::graphics::CullModeEx::front;
    case dayo::core::EffectCullMode::back:
        return dayo::graphics::CullModeEx::back;
    }
    return std::nullopt;
}

std::optional<dayo::graphics::CompareOpEx> oracleCompareOp(dayo::core::EffectDepthFunc function) {
    switch (function) {
    case dayo::core::EffectDepthFunc::never:
        return dayo::graphics::CompareOpEx::never;
    case dayo::core::EffectDepthFunc::less:
        return dayo::graphics::CompareOpEx::less;
    case dayo::core::EffectDepthFunc::equal:
        return dayo::graphics::CompareOpEx::equal;
    case dayo::core::EffectDepthFunc::lessEqual:
        return dayo::graphics::CompareOpEx::lessOrEqual;
    case dayo::core::EffectDepthFunc::greater:
        return dayo::graphics::CompareOpEx::greater;
    case dayo::core::EffectDepthFunc::notEqual:
        return dayo::graphics::CompareOpEx::notEqual;
    case dayo::core::EffectDepthFunc::greaterEqual:
        return dayo::graphics::CompareOpEx::greaterOrEqual;
    case dayo::core::EffectDepthFunc::always:
        return dayo::graphics::CompareOpEx::always;
    }
    return std::nullopt;
}

std::optional<dayo::graphics::BlendFactorEx> oracleBlendFactor(std::string_view value) {
    const auto key = compactPipelineValue(value);
    if (key.empty() || key == "one")
        return dayo::graphics::BlendFactorEx::one;
    if (key == "zero")
        return dayo::graphics::BlendFactorEx::zero;
    if (key == "srccolor")
        return dayo::graphics::BlendFactorEx::srcColor;
    if (key == "invsrccolor")
        return dayo::graphics::BlendFactorEx::oneMinusSrcColor;
    if (key == "destcolor")
        return dayo::graphics::BlendFactorEx::dstColor;
    if (key == "invdestcolor")
        return dayo::graphics::BlendFactorEx::oneMinusDstColor;
    if (key == "srcalpha")
        return dayo::graphics::BlendFactorEx::srcAlpha;
    if (key == "invsrcalpha")
        return dayo::graphics::BlendFactorEx::oneMinusSrcAlpha;
    if (key == "destalpha")
        return dayo::graphics::BlendFactorEx::dstAlpha;
    if (key == "invdestalpha")
        return dayo::graphics::BlendFactorEx::oneMinusDstAlpha;
    if (key == "srcalphasaturate")
        return dayo::graphics::BlendFactorEx::srcAlphaSaturate;
    return std::nullopt;
}

std::optional<dayo::graphics::BlendOpEx> oracleBlendOp(std::string_view value) {
    const auto key = compactPipelineValue(value);
    if (key.empty() || key == "add")
        return dayo::graphics::BlendOpEx::add;
    if (key == "subtract")
        return dayo::graphics::BlendOpEx::subtract;
    if (key == "revsubtract")
        return dayo::graphics::BlendOpEx::reverseSubtract;
    if (key == "min")
        return dayo::graphics::BlendOpEx::min;
    if (key == "max")
        return dayo::graphics::BlendOpEx::max;
    return std::nullopt;
}

bool validatePipelineOracle(const dayo::fx::FxProgram& program,
                            const std::vector<dayo::graphics::GraphicsPipelineDescEx>& descriptors,
                            std::string* error) {
    std::size_t descriptorIndex = 0;
    const auto fail = [error](std::string message) {
        if (error != nullptr)
            *error = std::move(message);
        return false;
    };
    for (const auto& dispatch : program.passes) {
        if (dispatch.kind != dayo::fx::FxOpKind::raster && dispatch.kind != dayo::fx::FxOpKind::postprocess)
            continue;
        if (descriptorIndex >= descriptors.size())
            return fail("pipeline oracle did not record graphics pass: " + dispatch.name);
        const auto& descriptor = descriptors[descriptorIndex++];
        std::size_t expectedColors = 0;
        if (const auto* raster = std::get_if<dayo::fx::FxRasterDispatch>(&dispatch.executable); raster != nullptr)
            expectedColors = raster->colorAttachments.size();
        else if (const auto* postprocess = std::get_if<dayo::fx::FxPostProcessDispatch>(&dispatch.executable);
                 postprocess != nullptr)
            expectedColors = postprocess->colorAttachments.size();
        if (expectedColors == 0) {
            expectedColors = static_cast<std::size_t>(
                std::count_if(dispatch.resources.begin(), dispatch.resources.end(), [](const auto& resource) {
                    return resource.write && resource.role == dayo::fx::FxResourceRole::colorAttachment;
                }));
        }
        const auto hasDepth = std::ranges::any_of(dispatch.resources, [](const auto& resource) {
            return resource.write && resource.role == dayo::fx::FxResourceRole::depthAttachment;
        });
        if (expectedColors == 0 && !hasDepth)
            expectedColors = 1;
        if (descriptor.colorFormats.size() != expectedColors)
            return fail("pipeline oracle color attachment count mismatch: " + dispatch.name);
        if (descriptor.depthFormat.has_value() != hasDepth)
            return fail("pipeline oracle depth attachment mismatch: " + dispatch.name);
        if (descriptor.depthOnly != (expectedColors == 0 && hasDepth))
            return fail("pipeline oracle depth-only flag mismatch: " + dispatch.name);

        const auto* raster = std::get_if<dayo::fx::FxRasterDispatch>(&dispatch.executable);
        if (raster == nullptr)
            continue;
        if (descriptor.rasterizer.cullMode != *oracleCullMode(raster->graphics.rasterizer.cullMode))
            return fail("pipeline oracle cull mode mismatch: " + dispatch.name);
        const auto expectedDepthTest = hasDepth && raster->graphics.depthStencil.depthEnable;
        const auto expectedDepthWrite = expectedDepthTest && raster->graphics.depthStencil.depthWrite;
        if (descriptor.depthStencil.depthTest != expectedDepthTest ||
            descriptor.depthStencil.depthWrite != expectedDepthWrite ||
            descriptor.depthStencil.depthCompare != *oracleCompareOp(raster->graphics.depthStencil.depthFunc))
            return fail("pipeline oracle depth state mismatch: " + dispatch.name);
        if (descriptor.blendAttachments.size() != descriptor.colorFormats.size())
            return fail("pipeline oracle blend attachment count mismatch: " + dispatch.name);
        for (std::size_t index = 0; index < raster->graphics.blend.size(); ++index) {
            if (index >= descriptor.blendAttachments.size())
                return fail("pipeline oracle blend state is missing: " + dispatch.name);
            const auto& source = raster->graphics.blend[index];
            const auto& actual = descriptor.blendAttachments[index];
            if (actual.enabled != source.enabled || actual.srcColor != *oracleBlendFactor(source.srcColor) ||
                actual.dstColor != *oracleBlendFactor(source.dstColor) ||
                actual.colorOp != *oracleBlendOp(source.colorOp) ||
                actual.srcAlpha != *oracleBlendFactor(source.srcAlpha) ||
                actual.dstAlpha != *oracleBlendFactor(source.dstAlpha) ||
                actual.alphaOp != *oracleBlendOp(source.alphaOp))
                return fail("pipeline oracle blend state mismatch: " + dispatch.name);
        }
    }
    if (descriptorIndex != descriptors.size())
        return fail("pipeline oracle recorded an unexpected graphics pass");
    return true;
}

bool buildPipelineOracle(const dayo::fx::FxProgram& program, const dayo::fx::FxShaderCompiler& shaderCompiler,
                         std::string* error) {
    PipelineOracleDevice device;
    dayo::graphics::FxPipelineRuntime runtime;
    const auto layout = dayo::graphics::handles::PipelineLayoutHandle{1, 1};
    if (!runtime.build(
            device, program, shaderCompiler,
            [layout](const dayo::fx::FxDispatch&) -> std::optional<dayo::graphics::handles::PipelineLayoutHandle> {
                return layout;
            },
            error, dayo::graphics::kNativeFxResourceSet))
        return false;
    return validatePipelineOracle(program, device.graphicsPipelines(), error);
}

void appendShaderRequest(const std::filesystem::path& sourceDirectory, const std::filesystem::path& effectPath,
                         const dayo::fx::FxProgram& program, const dayo::fx::FxDispatch& dispatch,
                         const dayo::core::EffectPass& pass, std::string entryPoint, dayo::fx::FxShaderStage stage,
                         dayo::fx::FxShaderCompiler& shaderCompiler, UpstreamScanResult& result) {
    if (entryPoint.empty())
        return;
    dayo::fx::FxShaderCompileRequest request;
    const auto effectDirectory = effectPath.parent_path();
    request.hlsl = dayo::fx::normalizeFxShaderIncludes(
        dayo::fx::makeNativeFxShaderSource(program, dispatch, dayo::graphics::kNativeFxResourceSet),
        effectDirectory.empty() ? std::filesystem::path{"."} : effectDirectory);
    request.sourcePath = effectPath;
    request.entryPoint = std::move(entryPoint);
    request.stage = stage;
    request.macros = dispatch.macros;
    request.macros.push_back(passMacro(pass.name));
    request.includeDirectories = shaderIncludeDirectories(sourceDirectory, effectPath);
    try {
        static_cast<void>(shaderCompiler.compile(request));
        ++result.shaderCount;
    } catch (const std::exception& exception) {
        result.failures.push_back(effectPath.string() + "#" + pass.name + ": " + exception.what());
    }
}

UpstreamScanResult scanUpstreamGraphs(const std::filesystem::path& sourceDirectory) {
    UpstreamScanResult result;
    const auto files = upstreamFxFiles(sourceDirectory);
    dayo::fx::FxCompiler compiler;
    const auto context = dayo::fx::makeFxFrameContext(0.0F, 0, 64, 64, 0, 0, 4096, 16, 1, 1);
    dayo::fx::FxShaderCompiler shaderCompiler;
    const bool dxc =
        shaderCompiler.executable().filename() == "dxc" || shaderCompiler.executable().filename() == "dxc.exe";
    const bool compileShaders = shaderCompiler.available() && dxc;
    if (!compileShaders) {
        if (upstreamShaderProbesRequired())
            result.failures.emplace_back("DXC is required for upstream shader probes but is unavailable");
        else
            std::cerr << "WARN: DXC unavailable; upstream graph shader probes skipped\n";
    }

    for (const auto& path : files) {
        try {
            const auto graph = dayo::core::loadEffectGraph(path);
            const auto linked = compiler.link(graph);
            const auto program = compiler.compile(linked);
            std::string pipelineError;
            if (compileShaders && !buildPipelineOracle(program, shaderCompiler, &pipelineError))
                throw std::runtime_error(pipelineError.empty() ? "FX pipeline oracle failed" : pipelineError);
            const auto plan = compiler.plan(program, context);
            if (plan.ordered.size() != program.passes.size())
                throw std::runtime_error("FX plan lost a dispatch");
            for (const auto feature : dayo::fx::analyzeRuntimeRequirements(program).features)
                ++result.featureCounts[feature];
            ++result.graphCount;
            result.passCount += program.passes.size();
            for (const auto& dispatch : program.passes) {
                for (const auto& resource : dispatch.resources)
                    if (resource.name.empty())
                        throw std::runtime_error("FX dispatch contains an empty resource name: " + dispatch.name);
            }
            if (!compileShaders)
                continue;
            for (std::size_t passIndex = 0; passIndex < graph.passes.size(); ++passIndex) {
                const auto& pass = graph.passes[passIndex];
                const auto& dispatch = program.passes[passIndex];
                switch (pass.type) {
                case dayo::core::EffectPassType::rasterizer:
                case dayo::core::EffectPassType::postprocess:
                    appendShaderRequest(sourceDirectory, path, program, dispatch, pass, pass.vertexShader,
                                        dayo::fx::FxShaderStage::vertex, shaderCompiler, result);
                    appendShaderRequest(sourceDirectory, path, program, dispatch, pass, pass.pixelShader,
                                        dayo::fx::FxShaderStage::fragment, shaderCompiler, result);
                    break;
                case dayo::core::EffectPassType::compute:
                    appendShaderRequest(sourceDirectory, path, program, dispatch, pass, pass.computeShader,
                                        dayo::fx::FxShaderStage::compute, shaderCompiler, result);
                    break;
                case dayo::core::EffectPassType::raytracing:
                    appendShaderRequest(sourceDirectory, path, program, dispatch, pass, pass.rayGenerationShader,
                                        dayo::fx::FxShaderStage::rayGeneration, shaderCompiler, result);
                    for (const auto& shader : pass.missShaders)
                        appendShaderRequest(sourceDirectory, path, program, dispatch, pass, shader,
                                            dayo::fx::FxShaderStage::miss, shaderCompiler, result);
                    for (const auto& group : pass.hitGroups) {
                        appendShaderRequest(sourceDirectory, path, program, dispatch, pass, group.closestHit,
                                            dayo::fx::FxShaderStage::closestHit, shaderCompiler, result);
                        appendShaderRequest(sourceDirectory, path, program, dispatch, pass, group.anyHit,
                                            dayo::fx::FxShaderStage::anyHit, shaderCompiler, result);
                        appendShaderRequest(sourceDirectory, path, program, dispatch, pass, group.intersection,
                                            dayo::fx::FxShaderStage::intersection, shaderCompiler, result);
                    }
                    for (const auto& shader : pass.callableShaders)
                        appendShaderRequest(sourceDirectory, path, program, dispatch, pass, shader,
                                            dayo::fx::FxShaderStage::callable, shaderCompiler, result);
                    break;
                case dayo::core::EffectPassType::copy:
                case dayo::core::EffectPassType::clear:
                case dayo::core::EffectPassType::mipmap:
                case dayo::core::EffectPassType::oidn:
                case dayo::core::EffectPassType::unknown:
                    break;
                }
            }
        } catch (const std::exception& exception) {
            result.failures.push_back(path.string() + ": " + exception.what());
        }
    }
    return result;
}

struct AbiBinding {
    char registerClass{};
    std::uint32_t registerIndex{};
    std::uint32_t descriptorSet{};
};

std::unordered_map<std::string, AbiBinding> readAbiBindings(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot read upstream ABI header: " + path.string());
    const std::regex declaration(
        R"(\b([A-Za-z_]\w*)\s*(?:\[\])?\s*:\s*register\(\s*([tubs])\s*(\d+)(?:\s*,\s*space\s*(\d+))?\s*\))");
    std::unordered_map<std::string, AbiBinding> result;
    for (std::string line; std::getline(input, line);) {
        std::smatch match;
        if (!std::regex_search(line, match, declaration))
            continue;
        result[match[1].str()] = {
            match[2].str().front(),
            static_cast<std::uint32_t>(std::stoul(match[3].str())),
            match[4].matched ? static_cast<std::uint32_t>(std::stoul(match[4].str())) : 0U,
        };
    }
    return result;
}

struct ExpectedAbiBinding {
    std::string_view name;
    char registerClass;
    std::uint32_t registerIndex;
    std::uint32_t descriptorSet;
    dayo::graphics::NativeSceneRegisterClass nativeClass;
};

bool checkUpstreamAbi(const std::filesystem::path& sourceDirectory) {
    const auto hlsl = sourceDirectory / "hlsl";
    bool ok = true;
    for (const auto relative : {"resources.hlsli", "resources_pp.hlsli", "cb.hlsli", "dayotypes.hlsli"})
        ok &= check(std::filesystem::is_regular_file(hlsl / relative), "pinned upstream ABI header exists");
    if (!ok)
        return false;

    const auto resources = readAbiBindings(hlsl / "resources.hlsli");
    const auto cb = readAbiBindings(hlsl / "cb.hlsli");
    const auto dayotypesText = [&] {
        std::ifstream input(hlsl / "dayotypes.hlsli");
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    const auto cbText = [&] {
        std::ifstream input(hlsl / "cb.hlsli");
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    const std::vector<ExpectedAbiBinding> expected = {
        {"RTOutput", 'u', 0, 0, dayo::graphics::NativeSceneRegisterClass::uav},
        {"OIDNBuf", 'u', 1, 0, dayo::graphics::NativeSceneRegisterClass::uav},
        {"NormalDepth", 'u', 2, 0, dayo::graphics::NativeSceneRegisterClass::uav},
        {"GBuffer1", 'u', 3, 0, dayo::graphics::NativeSceneRegisterClass::uav},
        {"GBuffer2", 'u', 4, 0, dayo::graphics::NativeSceneRegisterClass::uav},
        {"TLAS", 't', 0, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Model2Mat", 't', 1, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Mat2Model", 't', 2, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Peekaboo", 't', 3, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"MatSelected", 't', 4, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Skybox", 't', 5, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Skywalker", 't', 6, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"SkywalkerRow", 't', 7, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"SkyboxSH", 't', 8, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"ScreenBMP", 't', 9, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"CloneCount", 't', 10, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"ScreenTexture", 't', 11, 0, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"TextureTable", 't', 0, 1, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Textures", 't', 1, 1, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"MMDMaterials", 't', 0, 4, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Faces", 't', 0, 5, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"Mat2face", 't', 0, 6, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"FaceWalker", 't', 0, 7, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"PreVB", 't', 0, 8, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"RawVB", 't', 0, 9, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"VB", 't', 0, 2, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"IB", 't', 0, 3, dayo::graphics::NativeSceneRegisterClass::sampled},
        {"ViewCB", 'b', 0, 0, dayo::graphics::NativeSceneRegisterClass::uniform},
        {"CBuff1", 'b', 0, 1, dayo::graphics::NativeSceneRegisterClass::uniform},
    };
    for (const auto& item : expected) {
        const auto& table = item.name == "ViewCB" ? cb : resources;
        const auto found = table.find(std::string(item.name));
        ok &= check(found != table.end(), std::string("upstream ABI declares ") + std::string(item.name));
        if (found == table.end())
            continue;
        const auto& actual = found->second;
        const auto expectedBinding = dayo::graphics::nativeSceneBinding(item.nativeClass, item.registerIndex);
        ok &= check(actual.registerClass == item.registerClass && actual.registerIndex == item.registerIndex &&
                        actual.descriptorSet == item.descriptorSet,
                    std::string("upstream ABI register coordinates for ") + std::string(item.name));
        ok &= check(expectedBinding == dayo::graphics::nativeSceneBinding(item.nativeClass, actual.registerIndex),
                    std::string("native binding map covers ") + std::string(item.name));
    }
    for (const auto field : {"ViewMatrix", "ProjectionMatrix", "ModelCount", "TotalMaterialCount", "Resolution",
                             "SelfShadowMode", "ScreenBMPMode", "BackgroundMode", "BackgroundTransparent",
                             "DenoiserEnabled", "OnStart", "OnLoadSkybox", "OnResize", "OnLoad"})
        ok &= check(cbText.find(field) != std::string::npos, std::string("ViewCB field exists: ") + field);
    for (const auto field : {"struct OIDNInput", "float3 color", "float3 albedo", "float3 normal"})
        ok &=
            check(dayotypesText.find(field) != std::string::npos, std::string("dayotypes OIDN field exists: ") + field);
    return ok;
}

std::string spirvString(std::span<const std::uint32_t> words, std::size_t firstWord, std::size_t wordCount) {
    std::string result;
    for (std::size_t word = firstWord; word < wordCount; ++word) {
        const auto value = words[word];
        for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
            const auto character = static_cast<char>((value >> (byte * 8U)) & 0xFFU);
            if (character == '\0')
                return result;
            result.push_back(character);
        }
    }
    return result;
}

std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>>
reflectSpirvBindings(std::span<const std::uint32_t> words) {
    std::unordered_map<std::uint32_t, std::string> names;
    std::unordered_map<std::uint32_t, std::uint32_t> bindings;
    std::unordered_map<std::uint32_t, std::uint32_t> sets;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto instruction = words[offset];
        const auto wordCount = static_cast<std::size_t>(instruction >> 16U);
        if (wordCount == 0 || offset + wordCount > words.size())
            break;
        const auto opcode = instruction & 0xFFFFU;
        if (opcode == 5U && wordCount >= 3)
            names[words[offset + 1U]] = spirvString(words.subspan(offset, wordCount), 2, wordCount);
        if (opcode == 71U && wordCount >= 4) {
            if (words[offset + 2U] == 33U)
                bindings[words[offset + 1U]] = words[offset + 3U];
            if (words[offset + 2U] == 34U)
                sets[words[offset + 1U]] = words[offset + 3U];
        }
        offset += wordCount;
    }
    std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> result;
    for (const auto& [id, name] : names) {
        const auto binding = bindings.find(id);
        const auto set = sets.find(id);
        if (binding != bindings.end() && set != sets.end())
            result.emplace(name, std::pair{set->second, binding->second});
    }
    return result;
}

bool checkAbiSpirvProbe(const std::filesystem::path& sourceDirectory) {
    dayo::fx::FxShaderCompiler compiler;
    const bool dxc = compiler.executable().filename() == "dxc" || compiler.executable().filename() == "dxc.exe";
    if (!compiler.available() || !dxc) {
        if (upstreamShaderProbesRequired()) {
            std::cerr << "FAIL: DXC unavailable; upstream ABI SPIR-V probe is required\n";
            return false;
        }
        std::cerr << "WARN: DXC unavailable; upstream ABI SPIR-V probe skipped\n";
        return true;
    }
    dayo::fx::FxShaderCompileRequest request;
    request.sourcePath = sourceDirectory / "hlsl/dayo_abi_probe.hlsl";
    request.entryPoint = "main";
    request.stage = dayo::fx::FxShaderStage::compute;
    request.includeDirectories = {sourceDirectory / "hlsl"};
    request.hlsl = R"HLSL(#include "resources_pp.hlsli"
using namespace Dayo;
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 color = ScreenBMP.Load(int3(0, 0, 0)) + ScreenTexture.Load(int3(0, 0, 0));
    uint scene = CloneCount[0] + ModelIndex + RasterizeOrder + DeformIndex + DeformOrder;
    color += float4(float(ModelCount + TotalMaterialCount + scene + iSample), Time, 0, 0);
    RTOutput[id.xy] = color;
}
)HLSL";
    try {
        const auto artifact = compiler.compile(request);
        const auto reflected = reflectSpirvBindings(artifact.spirv);
        bool ok = true;
        struct ExpectedProbeBinding {
            const char* name;
            std::uint32_t set;
            std::uint32_t binding;
        };
        const std::array expectedBindings = {
            ExpectedProbeBinding{"RTOutput", 0,
                                 dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::uav, 0)},
            ExpectedProbeBinding{
                "ScreenBMP", 0,
                dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::sampled, 9)},
            ExpectedProbeBinding{
                "ScreenTexture", 0,
                dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::sampled, 11)},
            ExpectedProbeBinding{
                "CloneCount", 0,
                dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::sampled, 10)},
            ExpectedProbeBinding{
                "ViewCB", 0, dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::uniform, 0)},
            ExpectedProbeBinding{
                "CBuff1", 1, dayo::graphics::nativeSceneBinding(dayo::graphics::NativeSceneRegisterClass::uniform, 0)},
        };
        for (const auto& expected : expectedBindings) {
            const auto found = reflected.find(expected.name);
            ok &= check(found != reflected.end(), std::string("SPIR-V ABI probe reflects ") + expected.name);
            if (found == reflected.end())
                continue;
            ok &= check(found->second.first == expected.set && found->second.second == expected.binding,
                        std::string("SPIR-V ABI coordinates for ") + expected.name);
        }
        return ok;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: SPIR-V ABI probe: " << exception.what() << '\n';
        return false;
    }
}

} // namespace

int main() {
    const auto sourceDirectory = std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo";
    bool ok = true;

    // Do not silently run the compatibility suite against a stale 1.20 install.
    {
        std::ifstream lock(std::filesystem::path(DAYO_SOURCE_DIR) / "deps/mikumikudayo.lock");
        std::ifstream marker(sourceDirectory / ".mikumikudayo-ready");
        if (!check(lock.good() && marker.good(), "run scripts/fetch-mikumikudayo.py before upstream tests"))
            return 1;
        std::string expected;
        for (std::string line; std::getline(lock, line);) {
            if (!line.empty() && line.front() != '#')
                expected += line + '\n';
        }
        const std::string actual((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
        if (!check(actual == expected, "upstream installation must match the pinned release lock"))
            return 1;
    }

    try {
        ok &= checkUpstreamAbi(sourceDirectory);
        ok &= checkAbiSpirvProbe(sourceDirectory);
        const auto expectedGraphCount = upstreamFxFiles(sourceDirectory).size();
        const auto scan = scanUpstreamGraphs(sourceDirectory);
        ok &= check(scan.graphCount == expectedGraphCount, "all pinned upstream FX graphs compile and link");
        ok &= check(scan.failures.empty(), "all pinned upstream FX metadata and shader probes pass");
        std::cout << "INFO: upstream oracle validated " << scan.graphCount << " graphs, " << scan.passCount
                  << " dispatches, and " << scan.shaderCount << " shader probes\n";
        for (const auto& [feature, count] : scan.featureCounts)
            std::cout << "INFO: upstream runtime requirement " << dayo::fx::toString(feature) << " appears in " << count
                      << " graphs\n";
        for (const auto& failure : scan.failures)
            std::cerr << "FAIL: upstream oracle: " << failure << '\n';
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: upstream oracle: " << exception.what() << '\n';
        ok = false;
    }

    try {
        const auto previewEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/Preview.fxdayo");
        ok &= check(previewEffect.passes.size() == 5 && !previewEffect.hlsl.empty(), "Preview fxdayo graph");
        const auto previewRaster =
            std::ranges::find_if(previewEffect.passes, [](const auto& pass) { return pass.name == "MMD"; });
        const auto previewGBuffer =
            std::ranges::find_if(previewEffect.passes, [](const auto& pass) { return pass.name == "GBuffer"; });
        ok &= check(previewRaster != previewEffect.passes.end() &&
                        previewRaster->graphics.rasterizer.cullMode == dayo::core::EffectCullMode::none &&
                        previewRaster->graphics.blend.size() == 1 && previewRaster->graphics.blend[0].enabled &&
                        previewRaster->graphics.blend[0].srcColor == "src_alpha" &&
                        previewRaster->graphics.blend[0].dstColor == "inv_src_alpha" &&
                        previewRaster->depth.clearValue.depth == 1.0F,
                    "Preview graphics state is lossless");
        ok &= check(previewGBuffer != previewEffect.passes.end() && previewGBuffer->renderTargets.size() == 3 &&
                        previewGBuffer->renderTargets[1].clearValue.color[0] == -1.0F,
                    "Preview MRT clear values");
        const auto subayaiEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/Subayai.fxdayo");
        ok &= check(subayaiEffect.passes.size() >= 20 &&
                        std::ranges::any_of(
                            subayaiEffect.passes,
                            [](const auto& pass) { return pass.type == dayo::core::EffectPassType::raytracing; }) &&
                        subayaiEffect.hlslPrefix.find("resources.hlsli") != std::string::npos &&
                        subayaiEffect.materialDescriptor.has_value() && !subayaiEffect.controllers.empty(),
                    "Subayai Jsonnet expansion");
        const auto subayaiRaster =
            std::ranges::find_if(subayaiEffect.passes, [](const auto& pass) { return pass.name == "MMD"; });
        ok &= check(subayaiRaster != subayaiEffect.passes.end() && subayaiRaster->renderTargets.size() == 4 &&
                        subayaiRaster->graphics.blend.size() == 4 && subayaiRaster->graphics.depthStencil.depthWrite &&
                        subayaiRaster->graphics.depthStencil.depthFunc == dayo::core::EffectDepthFunc::lessEqual,
                    "Subayai MRT/depth/blend state");
        const auto rayPass = std::ranges::find_if(
            subayaiEffect.passes, [](const auto& pass) { return pass.type == dayo::core::EffectPassType::raytracing; });
        ok &= check(rayPass != subayaiEffect.passes.end() && !rayPass->hitGroups.empty() &&
                        rayPass->maxPayloadSize != 0 && rayPass->maxRecursionDepth != 0,
                    "Subayai ray-tracing pipeline metadata");
        const auto cloneEffect = dayo::core::loadEffectGraph(sourceDirectory / "sample/clone_sample.fxdayo");
        const auto cloneBuffer =
            std::ranges::find_if(cloneEffect.buffers, [](const auto& buffer) { return buffer.name == "skinned"; });
        ok &= check(cloneEffect.meshCloneCount == 4 && cloneBuffer != cloneEffect.buffers.end() &&
                        cloneBuffer->size.base == "VERTEXCOUNT",
                    "1.30 mesh cloning and buffer size metadata");
        const auto fluidEffect = dayo::core::loadEffectGraph(sourceDirectory / "postprocess/Fog/fluid3D.fxdayo");
        const auto fluidTexture =
            std::ranges::find_if(fluidEffect.textures3D, [](const auto& texture) { return texture.name == "VMap"; });
        const auto fluidBaseTexture =
            std::ranges::find_if(fluidEffect.textures3D, [](const auto& texture) { return texture.name == "WMap"; });
        ok &= check(fluidTexture != fluidEffect.textures3D.end() && fluidBaseTexture != fluidEffect.textures3D.end() &&
                        fluidTexture->size.base == "WMap" && fluidBaseTexture->size.absolute &&
                        fluidBaseTexture->size.depth > 0 && !fluidEffect.buffers.empty(),
                    "1.30 3D texture size and buffer metadata");
        const auto bdptEffect = dayo::core::loadEffectGraph(sourceDirectory / "renderer/BDPT.fxdayo");
        ok &= check(!bdptEffect.passes.empty() && std::ranges::any_of(bdptEffect.passes,
                                                                      [](const auto& pass) {
                                                                          return pass.type ==
                                                                                 dayo::core::EffectPassType::raytracing;
                                                                      }),
                    "BDPT fxdayo graph");

        const std::string fixture = R"FX([YRZFX]
{
  "fx": {
    "category": "render",
    "passes": [{
      "name": "Fixture",
      "type": "rasterizer",
      "vertexShader": "VS",
      "pixelShader": "PS",
      "RTV": [{"name":"Color", "clear":true, "value":{"x":1,"y":0.5,"z":0,"w":1}}],
      "DSV": {"name":"Depth", "clear":true, "depth":0.25},
      "rasterizerDesc": {"cullMode":"front"},
      "depthStencilDesc": {"depthWriteMask":"zero", "depthFunc":"less_equal"},
      "blendDesc": {"renderTarget0": {"blendEnable":true, "srcBlend":"one", "destBlend":"inv_src_alpha", "blendOp":"add"}},
      "rasterModelTarget":"other"
    }]
  }
}
[HLSL]
float4 PS() : SV_TARGET { return 1; }
)FX";
        const auto fixtureEffect = dayo::core::loadEffectGraphFromText("lossless-fixture.fxdayo", fixture);
        const auto& fixturePass = fixtureEffect.passes.front();
        ok &= check(fixturePass.graphics.rasterizer.cullMode == dayo::core::EffectCullMode::front &&
                        !fixturePass.graphics.depthStencil.depthWrite &&
                        fixturePass.graphics.depthStencil.depthFunc == dayo::core::EffectDepthFunc::lessEqual &&
                        fixturePass.graphics.modelTarget == dayo::core::fx::RasterModelTarget::other &&
                        fixturePass.graphics.blend.front().srcColor == "one" &&
                        fixturePass.renderTargets.front().clearValue.color[1] == 0.5F &&
                        fixturePass.depth.clearValue.depth == 0.25F,
                    "YRZFX graphics state fixture");
        ok &= check(fixtureEffect.rawYrzfx.find("Fixture") != std::string::npos,
                    "YRZFX source section is retained verbatim in the graph");

        const std::string runtimeFixture = R"FX([YRZFX]
{
  fx: {
    category: "postprocess",
    memos: ["SkyboxSampler", "unknown-capability"],
    globalVarSize: 16,
    meshCloning: {count: 4},
    controllers: [{name:"gain", controllerName:"(self)", item:"gain", type:"float", description:"gain control", slider:{min:0.1, max:4, step:0.1, default:1, log:true}}],
    samplers: [{name:"Linear", filter:"ANISOTROPIC", addressU:"CLAMP", addressV:"MIRROR", addressW:"BORDER", mipLodBias:1, maxAnisotropy:8, comparisonFunc:"LESS", borderColor:"OPAQUE_WHITE", minLod:2, maxLod:10}],
    buffers: [
      {name:"Vertices", type:"Vertex", elemSize:16, view:"SRV", size:{absolute:true, width:4, dimension:1}},
      {name:"Indices", type:"uint", view:"UAV", size:{absolute:true, width:6, dimension:1}}
    ],
    passes: [
      {name:"Compute", type:"compute", computeShader:"CS", numthreads:{x:4}, outputSize:{base:"DEFAULT_RTSIZE", ratio:{x:0.5, y:0.25}}},
      {name:"BufferDraw", type:"rasterizer", vertexShader:"VS", pixelShader:"PS", rasterModelTarget:"buffer", rasterVB:"Vertices", rasterIB:"Indices", layout:[{semanticName:"POSITION", semanticIndex:0, format:"R32G32B32_FLOAT", inputSlot:0, alignedByteOffset:0}]},
      {name:"Clear", type:"clearRTV", target:"Output", value:{x:0.25, y:0.5, z:0.75, w:1}}
    ]
  }
}
[HLSL]
void CS() {}
)FX";
        const auto runtimeGraph = dayo::core::loadEffectGraphFromText("runtime-metadata.fxdayo", runtimeFixture);
        ok &= check(runtimeGraph.rawYrzfx.find("unknown-capability") != std::string::npos &&
                        runtimeGraph.memos.size() == 2 && runtimeGraph.globalVarSize == 16 &&
                        runtimeGraph.meshCloneCount == 4,
                    "effect graph preserves memos, global variable size, clone count, and raw source");
        ok &= check(runtimeGraph.controllers.size() == 1 && runtimeGraph.controllers[0].slider.has_value() &&
                        runtimeGraph.controllers[0].slider->logarithmic &&
                        runtimeGraph.controllers[0].description == "gain control" &&
                        runtimeGraph.samplers.size() == 1 && runtimeGraph.samplers[0].maxAnisotropy == 8 &&
                        runtimeGraph.samplers[0].addressModeW == dayo::core::FxAddressMode::border &&
                        runtimeGraph.samplers[0].comparisonFunc == dayo::core::FxCompareOp::less &&
                        runtimeGraph.samplers[0].borderColor == dayo::core::FxBorderColor::opaqueWhite,
                    "controller slider and complete sampler metadata survive parsing");
        ok &= check(runtimeGraph.passes.size() == 3 &&
                        runtimeGraph.passes[0].numThreads == std::array<std::uint32_t, 3>{4, 0, 0} &&
                        runtimeGraph.buffers.size() == 2 && runtimeGraph.buffers[1].elementSize == 4 &&
                        runtimeGraph.passes[1].rasterSource == dayo::core::EffectRasterSource::buffer &&
                        runtimeGraph.passes[1].rasterVertexBuffer == "Vertices" &&
                        runtimeGraph.passes[1].rasterIndexBuffer == "Indices" &&
                        runtimeGraph.passes[1].vertexLayout.attributes.size() == 1 &&
                        runtimeGraph.passes[1].vertexLayout.attributes[0].semanticName == "POSITION" &&
                        runtimeGraph.passes[2].functionalKind == dayo::core::EffectFunctionalPassKind::clearRtv &&
                        runtimeGraph.passes[2].functional.clearValue.color[1] == 0.5F,
                    "compute size, buffer raster layout, and functional clear metadata survive parsing");
        const auto runtimeProgram = dayo::fx::FxCompiler{}.compile(runtimeGraph);
        const auto required = dayo::fx::analyzeRuntimeRequirements(runtimeProgram);
        ok &= check(required.contains(dayo::fx::FxRuntimeFeature::globalVariables) &&
                        required.contains(dayo::fx::FxRuntimeFeature::meshCloning) &&
                        required.contains(dayo::fx::FxRuntimeFeature::fullSamplerState) &&
                        required.contains(dayo::fx::FxRuntimeFeature::bufferRaster) &&
                        required.contains(dayo::fx::FxRuntimeFeature::functionalClearRtv),
                    "runtime feature analyzer inventories parsed upstream requirements");
        bool rejectedUnknown = false;
        try {
            auto invalid = fixture;
            const auto marker = invalid.find("front");
            invalid.replace(marker, 5, "diagonal");
            static_cast<void>(dayo::core::loadEffectGraphFromText("invalid-lossless-fixture.fxdayo", invalid));
        } catch (const std::runtime_error&) {
            rejectedUnknown = true;
        }
        ok &= check(rejectedUnknown, "YRZFX rejects unknown cull values");
        bool rejectedUnsupportedBlend = false;
        try {
            auto invalid = fixture;
            const auto marker = invalid.find("\"srcBlend\":\"one\"");
            if (marker == std::string::npos)
                throw std::runtime_error("blend fixture marker missing");
            invalid.replace(marker, std::string("\"srcBlend\":\"one\"").size(), "\"srcBlend\":\"src1_color\"");
            static_cast<void>(dayo::core::loadEffectGraphFromText("unsupported-blend-fixture.fxdayo", invalid));
        } catch (const std::runtime_error&) {
            rejectedUnsupportedBlend = true;
        }
        ok &= check(rejectedUnsupportedBlend, "YRZFX rejects blend factors without a native pipeline contract");
        bool rejectedUnsupportedBlendFactor = false;
        try {
            auto invalid = fixture;
            const auto marker = invalid.find("\"srcBlend\":\"one\"");
            if (marker == std::string::npos)
                throw std::runtime_error("blend fixture marker is missing");
            invalid.replace(marker, std::string_view{"\"srcBlend\":\"one\""}.size(), "\"srcBlend\":\"blendfactor\"");
            static_cast<void>(dayo::core::loadEffectGraphFromText("invalid-blend-fixture.fxdayo", invalid));
        } catch (const std::runtime_error& exception) {
            rejectedUnsupportedBlendFactor =
                std::string_view(exception.what()).find("blend factor") != std::string_view::npos;
        }
        ok &= check(rejectedUnsupportedBlendFactor, "YRZFX rejects blend factors without a native Vulkan mapping");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: effect graph: " << exception.what() << '\n';
        ok = false;
    }

    try {
        const auto icon = dayo::core::loadImageRgba8(sourceDirectory / "res/dayoicon.png");
        ok &= check(icon.width > 0 && icon.height > 0 && icon.pixels.size() == icon.width * icon.height * 4U,
                    "RGBA image decode");
        const auto dds = dayo::core::loadImageRgba8(sourceDirectory / "particle/Smoke.dds");
        ok &= check(dds.width > 0 && dds.height > 0 && dds.pixels.size() == dds.width * dds.height * 4U,
                    "DDS image decode");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: image load: " << exception.what() << '\n';
        ok = false;
    }

    try {
        std::filesystem::path sampleVmd;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(sourceDirectory / "sample")) {
            if (entry.path().extension() == ".vmd") {
                sampleVmd = entry.path();
                break;
            }
        }
        if (sampleVmd.empty())
            throw std::runtime_error("no sample VMD was found");
        const auto motion = dayo::core::loadVmd(sampleVmd);
        ok &= check(!motion.modelName.empty(), "VMD CP932 model name");
        ok &= check(!motion.bones.empty(), "VMD bone keys");
        const auto exportedVmd = std::filesystem::temp_directory_path() / "mikumikudesu-vmd-export-test.vmd";
        dayo::core::saveVmd(exportedVmd, motion);
        const auto exported = dayo::core::loadVmd(exportedVmd);
        ok &= check(exported.modelName == motion.modelName && exported.bones.size() == motion.bones.size() &&
                        exported.morphs.size() == motion.morphs.size() &&
                        exported.cameras.size() == motion.cameras.size() &&
                        exported.lights.size() == motion.lights.size() &&
                        exported.shadows.size() == motion.shadows.size() && exported.ik.size() == motion.ik.size(),
                    "VMD export round trip");
        std::error_code exportError;
        std::filesystem::remove(exportedVmd, exportError);
        bool evaluatedFixture = false;
        for (const auto& entry : std::filesystem::directory_iterator(sourceDirectory / "sample")) {
            if (entry.path().extension() != ".pmx")
                continue;
            try {
                auto candidate = dayo::core::loadPmxModel(entry.path());
                if (candidate.metadata.modelName != motion.modelName || candidate.vertices.empty())
                    continue;
                dayo::core::MmdAnimator animator(candidate);
                animator.setMotion(&motion);
                const auto compatibility = animator.motionCompatibility();
                ok &= check(compatibility.matchedBoneTrackCount > 0, "sample VMD/PMX has compatible bone tracks");
                const auto first = animator.evaluate(0.0F);
                const auto animated = animator.evaluate(10.0F);
                bool changed = false;
                for (std::size_t i = 0; i < first.vertices.size(); ++i) {
                    if (first.vertices[i].position != animated.vertices[i].position) {
                        changed = true;
                        break;
                    }
                }
                ok &= check(changed, "VMD CPU skinning changes vertices");
                evaluatedFixture = true;
                break;
            } catch (const std::exception&) {
                // Some tiny effect descriptors use the PMX extension without model sections.
            }
        }
        ok &= check(evaluatedFixture, "sample VMD has a matching PMX fixture");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: VMD load: " << exception.what() << '\n';
        ok = false;
    }

    return ok ? 0 : 1;
}
