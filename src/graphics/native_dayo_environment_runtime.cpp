#include "graphics/native_dayo_environment_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace dayo::graphics {
namespace {

constexpr std::array<std::string_view, 6> kPdfEntries{"SkyLuminance", "SkyLuminanceRow", "SkyWalkin",
                                                      "SkyWalkinRow", "SHComboX",        "SHComboY"};
constexpr std::size_t kPdfPassCount = 4;
constexpr std::uint32_t kSkyboxBinding = 16;             // DXC's Vulkan t0 shift.
constexpr std::uint32_t kPrefilterSamplerBinding = 32;   // DXC's Vulkan s0 shift.
constexpr std::uint32_t kPrefilterConstantsBinding = 48; // DXC's Vulkan b0 shift.
constexpr std::uint32_t kPrefilterIterations = 4;
constexpr std::uint32_t kPrefilterSamplesPerIteration = 64;

// The pixel-shader algorithm is copied from the pinned MikuMikuDayo 1.30
// YRZ.ixx PrefilterShader. VS uses SV_VertexID to produce the same fullscreen
// coverage without depending on the upstream post-process vertex buffer.
constexpr std::string_view kSkyboxPrefilterHlsl = R"hlsl(
cbuffer CB : register(b0, space1) { uint roughInt; uint iIter; uint AlphaInt; uint Samples; }
Texture2D<float4> SrcTex : register(t0);
sampler samp : register(s0);
static float PI = acos(-1);

struct VSO { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VSO VS(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSO o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PSCopy(VSO vso) : SV_TARGET { return SrcTex.SampleLevel(samp, vso.uv, 0); }

float2 LongRat(float3 r) {
    return float2((atan2(r.y, r.x) / PI + 1) / 2, acos(r.z) / PI);
}

// PCG4d follows Jarzynski and Olano, "Hash Functions for GPU Rendering":
// https://jcgt.org/published/0009/03/02/paper.pdf
uint4 PCG4d(uint4 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    v ^= v >> 16u;
    v.x += v.y * v.w; v.y += v.z * v.x; v.z += v.x * v.y; v.w += v.y * v.z;
    return v;
}

float4 Hash4(uint4 x) {
    uint4 p = PCG4d(x) & 0x007FFFFF;
    return p / float(0x00800000);
}

float2 CosSin(float t) {
    float2 a;
    sincos(t, a.y, a.x);
    return a;
}

float3 SampleVndf_Hemisphere(float2 u, float3 wi) {
    // Visible-normal sampling follows Dupuy and Benyoub, "Sampling Visible
    // GGX Normals with Spherical Caps": https://arxiv.org/abs/2306.05044
    float z = mad(1 - u.y, 1 + wi.z, -wi.z);
    float st = sqrt(saturate(1 - z * z));
    float3 c = float3(st * CosSin(2 * PI * u.x), z);
    return c + wi;
}

float3 VNDF(float3 wi, float3x3 TBN, float2 Xi, float2 a) {
    if (any(a == 0))
        return TBN[2];
    float3 wiTan = mul(TBN, wi);
    float3 wiStd = normalize(float3(wiTan.xy * a, wiTan.z));
    float3 wmStd = SampleVndf_Hemisphere(Xi, wiStd);
    float3 wm = float3(wmStd.xy * a, wmStd.z);
    return mul(normalize(wm), TBN);
}

float SmithG1(float a2, float NoV) {
    return 2 / (1 + sqrt(a2 / (NoV * NoV) + (1 - a2)));
}

bool SameHemisphere(float3 wi, float3 wo, float3 N) {
    return (dot(wi, N) > 0) && (dot(wo, N) > 0);
}

float MicrofacetBRDFdivPDF(float a, float3 wi, float3 wo, float3 N) {
    if (!SameHemisphere(wi, wo, N))
        return 0;
    float NoL = max(dot(N, wi), 1e-10);
    return SmithG1(a * a, NoL);
}

float4 PS(VSO vso) : SV_TARGET {
    float roughness = asfloat(roughInt);
    float a = roughness * roughness;
    float2 uv = vso.uv;
    float alpha = asfloat(AlphaInt);
    float theta = uv.y * PI;
    float phi = (uv.x * 2 - 1) * PI;
    float3 N = float3(cos(phi) * sin(theta), sin(phi) * sin(theta), cos(theta));
    float3 T = float3(cos(phi) * cos(theta), sin(phi) * cos(theta), -sin(theta));
    float3 B = float3(-sin(phi), cos(phi), 0);
    float3x3 TBN = {T, B, N};
    float3 tc = 0;
    float tw = 0;
    for (int i = 0; i < Samples; i++) {
        float2 xi = Hash4(uint4(vso.pos.xy, i, iIter)).xy;
        float3 H = VNDF(N, TBN, xi, a);
        float w = MicrofacetBRDFdivPDF(a, N, reflect(-N, H), H);
        tw += w;
        float3 c = SrcTex.SampleLevel(samp, LongRat(H), 0).rgb;
        tc += c * w;
    }
    return float4(tc / tw, alpha);
}
)hlsl";

struct DayoPrefilterConstants {
    std::uint32_t roughness{};
    std::uint32_t iteration{};
    std::uint32_t alpha{};
    std::uint32_t samples{};
};
static_assert(sizeof(DayoPrefilterConstants) == 16);

[[nodiscard]] std::uint32_t ceilDiv(std::uint32_t value, std::uint32_t divisor) {
    return value / divisor + static_cast<std::uint32_t>(value % divisor != 0U);
}

[[nodiscard]] std::size_t bufferByteSize(std::uint64_t count, std::size_t stride, std::string_view name) {
    if (count == 0 || count > std::numeric_limits<std::size_t>::max() / stride)
        throw std::overflow_error(std::string(name) + " buffer size exceeds host address space");
    return static_cast<std::size_t>(count) * stride;
}

[[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open pinned Dayo shader: " + path.string());
    std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (result.empty())
        throw std::runtime_error("pinned Dayo shader is empty: " + path.string());
    return result;
}

[[nodiscard]] bool isDxc(const std::filesystem::path& executable) {
    auto name = executable.filename().string();
    std::ranges::transform(name, name.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return name == "dxc" || name == "dxc.exe";
}

[[nodiscard]] handles::BufferHandle createBuffer(Device& device, std::size_t size, bool hostVisible = false) {
    if (size == 0)
        throw std::invalid_argument("Dayo environment cannot allocate an empty buffer");
    const auto usage =
        hostVisible ? ResourceUsage::storageReadWrite | ResourceUsage::hostRead : ResourceUsage::storageReadWrite;
    const auto buffer = device.createBufferEx(
        {.size = size, .usage = usage, .cpuVisible = hostVisible, .lifetime = ResourceLifetime::persistent});
    if (!buffer.valid())
        throw std::runtime_error("Dayo environment buffer allocation returned an invalid handle");
    return buffer;
}

[[nodiscard]] handles::BufferHandle createDummyWalkerBuffer(Device& device) {
    const auto buffer = createBuffer(device, sizeof(DayoWalkerAlias), true);
    const std::array<DayoWalkerAlias, 1> dummy{};
    try {
        device.uploadBufferEx(buffer, std::as_bytes(std::span<const DayoWalkerAlias>(dummy)), 0);
    } catch (...) {
        device.destroyBufferEx(buffer);
        throw;
    }
    return buffer;
}

[[nodiscard]] DescriptorSetLayoutDesc environmentLayout() {
    DescriptorSetLayoutDesc result;
    result.bindings.reserve(7);
    for (std::uint32_t binding = 0; binding < 6; ++binding)
        result.bindings.push_back({binding, DescriptorKind::storageBuffer, 1, ShaderStageMask::compute});
    result.bindings.push_back({kSkyboxBinding, DescriptorKind::sampledImage, 1, ShaderStageMask::compute});
    return result;
}

[[nodiscard]] std::array<DescriptorBindingEx, 7>
pdfBindings(handles::BufferHandle skywalker, handles::BufferHandle skywalkerRow, handles::BufferHandle worker,
            handles::BufferHandle workerRow, handles::BufferHandle skyLum, handles::BufferHandle skyLumRow,
            handles::TextureHandle skybox) {
    return {DescriptorBindingEx{.slot = 0, .arrayElement = 0, .buffer = skywalker},
            DescriptorBindingEx{.slot = 1, .arrayElement = 0, .buffer = skywalkerRow},
            DescriptorBindingEx{.slot = 2, .arrayElement = 0, .buffer = worker},
            DescriptorBindingEx{.slot = 3, .arrayElement = 0, .buffer = workerRow},
            DescriptorBindingEx{.slot = 4, .arrayElement = 0, .buffer = skyLum},
            DescriptorBindingEx{.slot = 5, .arrayElement = 0, .buffer = skyLumRow},
            DescriptorBindingEx{.slot = kSkyboxBinding, .arrayElement = 0, .texture = skybox}};
}

[[nodiscard]] std::array<DescriptorBindingEx, 7>
shBindings(handles::BufferHandle skyboxShX, handles::BufferHandle skyboxSh, handles::TextureHandle skybox) {
    return {DescriptorBindingEx{.slot = 0, .arrayElement = 0, .buffer = skyboxShX},
            DescriptorBindingEx{.slot = 1, .arrayElement = 0, .buffer = skyboxSh},
            DescriptorBindingEx{.slot = 2, .arrayElement = 0, .buffer = skyboxShX},
            DescriptorBindingEx{.slot = 3, .arrayElement = 0, .buffer = skyboxShX},
            DescriptorBindingEx{.slot = 4, .arrayElement = 0, .buffer = skyboxShX},
            DescriptorBindingEx{.slot = 5, .arrayElement = 0, .buffer = skyboxShX},
            DescriptorBindingEx{.slot = kSkyboxBinding, .arrayElement = 0, .texture = skybox}};
}

} // namespace

std::vector<DayoEnvironmentDispatch> buildDayoEnvironmentDispatchPlan(Extent3D extent, bool buildSkyboxSampler) {
    if (extent.width == 0 || extent.height < 2 || extent.depth != 1 ||
        static_cast<std::uint64_t>(extent.height) * 2U != extent.width)
        throw std::invalid_argument("Dayo environment requires a non-empty 2:1 2D texture");
    if (static_cast<std::uint64_t>(extent.width) * extent.height > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Dayo environment dimensions exceed 32-bit shader indexing");

    std::vector<DayoEnvironmentDispatch> result;
    result.reserve(buildSkyboxSampler ? 6U : 2U);
    if (buildSkyboxSampler) {
        result.push_back(
            {DayoEnvironmentPass::skyLuminance, {ceilDiv(extent.width, 16), ceilDiv(extent.height, 16), 1}});
        result.push_back({DayoEnvironmentPass::skyLuminanceRow, {ceilDiv(extent.height, 128), 1, 1}});
        result.push_back({DayoEnvironmentPass::skyWalkin, {ceilDiv(extent.height, 128), 1, 1}});
        result.push_back({DayoEnvironmentPass::skyWalkinRow, {1, 1, 1}});
    }
    result.push_back({DayoEnvironmentPass::shComboX, {ceilDiv(extent.height, 32), 1, 1}});
    result.push_back({DayoEnvironmentPass::shComboY, {1, 1, 1}});
    return result;
}

std::vector<DayoSkyboxPrefilterDraw> buildDayoSkyboxPrefilterPlan(Extent3D extent) {
    if (extent.width == 0 || extent.height < 2 || extent.depth != 1 ||
        static_cast<std::uint64_t>(extent.height) * 2U != extent.width)
        throw std::invalid_argument("Dayo SkyboxPrefilter requires a non-empty 2:1 2D texture");

    std::uint32_t largestDimension = std::max(extent.width, extent.height);
    std::uint32_t mipLevels = 1;
    while (largestDimension > 1) {
        largestDimension = std::max(largestDimension / 2U, 1U);
        ++mipLevels;
    }
    if (mipLevels < 2)
        throw std::invalid_argument("Dayo SkyboxPrefilter requires at least two mip levels");

    std::vector<DayoSkyboxPrefilterDraw> result;
    result.reserve(static_cast<std::size_t>(kPrefilterIterations) * (mipLevels - 1U));
    for (std::uint32_t iteration = 0; iteration < kPrefilterIterations; ++iteration) {
        for (std::uint32_t mipLevel = 1; mipLevel < mipLevels; ++mipLevel) {
            const auto width = std::max(extent.width >> mipLevel, 1U);
            const auto height = std::max(extent.height >> mipLevel, 1U);
            const auto sampleShift = mipLevel - 1U;
            const auto sampleCount =
                sampleShift >= 10U ? 65536U : std::min(65536U, kPrefilterSamplesPerIteration << sampleShift);
            result.push_back({.mipLevel = mipLevel,
                              .width = width,
                              .height = height,
                              .iteration = iteration,
                              .roughness = static_cast<float>(mipLevel) / static_cast<float>(mipLevels - 1U),
                              .alpha = 1.0F / static_cast<float>(mipLevel),
                              .samples = sampleCount});
        }
    }
    return result;
}

NativeDayoEnvironmentRuntime::~NativeDayoEnvironmentRuntime() {
    reset();
}

bool NativeDayoEnvironmentRuntime::sync(Device& device, CommandList& commands, handles::TextureHandle skybox,
                                        Extent3D extent, const std::filesystem::path& source,
                                        std::uint64_t sourceVersion, const std::filesystem::path& hlslDirectory,
                                        bool buildSkyboxSampler, std::string* error, bool buildSkyboxPrefilter) {
    if (error != nullptr)
        error->clear();
    if (!skybox.valid() || source.empty() || hlslDirectory.empty()) {
        setError(error, "Dayo environment requires a Skybox texture, source path, and pinned HLSL directory");
        return false;
    }
    try {
        static_cast<void>(buildDayoEnvironmentDispatchPlan(extent, buildSkyboxSampler));
    } catch (const std::exception& exception) {
        setError(error, exception.what());
        return false;
    }
    if (device_ != nullptr && device_ != &device) {
        setError(error, "Dayo environment resources belong to a different device");
        return false;
    }
    const auto normalizedHlslDirectory = std::filesystem::absolute(hlslDirectory).lexically_normal();
    const bool sameExtent =
        extent_.width == extent.width && extent_.height == extent.height && extent_.depth == extent.depth;
    if (ready() && sourceSkybox_ == skybox && source_ == source && sourceVersion_ == sourceVersion && sameExtent &&
        normalizedHlslDirectory == hlslDirectory_ && buildSkyboxSampler_ == buildSkyboxSampler &&
        buildSkyboxPrefilter_ == buildSkyboxPrefilter)
        return true;
    if (!ensurePipelines(device, normalizedHlslDirectory, error))
        return false;
    if (buildSkyboxPrefilter && !ensurePrefilterPipelines(device, error))
        return false;

    Resources next;
    if (!createResources(device, skybox, extent, buildSkyboxSampler, buildSkyboxPrefilter, next, error))
        return false;

    const bool hasCurrentResources = resources_.skywalker.valid() || resources_.skywalkerRow.valid() ||
                                     resources_.skyboxSh.valid() || resources_.skyboxShX.valid() ||
                                     resources_.prefilteredSkybox.valid();
    if (device_ != nullptr && hasCurrentResources) {
        try {
            device.waitIdle();
        } catch (const std::exception& exception) {
            destroyResources(&device, next);
            setError(error, std::string("Dayo environment could not retire prior GPU resources: ") + exception.what());
            return false;
        } catch (...) {
            destroyResources(&device, next);
            setError(error, "Dayo environment could not retire prior GPU resources");
            return false;
        }
    }
    destroyResources(device_, resources_);
    resources_ = next;
    device_ = &device;
    sourceSkybox_ = skybox;
    skybox_ = resources_.prefilteredSkybox.valid() ? resources_.prefilteredSkybox : skybox;

    if (resources_.prefilteredSkybox.valid()) {
        commands.transitionEx(sourceSkybox_);
        commands.clearTextureEx(resources_.prefilteredSkybox, {0.0F, 0.0F, 0.0F, 0.0F});
        commands.bindPipelineEx(prefilterPipelines_[0]);
        commands.bindDescriptorSetEx(resources_.prefilterCopySet);
        const RenderingInfoEx copyInfo{
            .colors = {{.texture = resources_.prefilteredSkybox}},
            .extent = extent,
        };
        commands.beginRenderingEx(copyInfo);
        commands.draw(3);
        commands.endRenderingEx();

        const auto prefilterPlan = buildDayoSkyboxPrefilterPlan(extent);
        if (prefilterPlan.size() != resources_.prefilterConstantSets.size())
            throw std::logic_error("Dayo SkyboxPrefilter constant descriptors do not match its draw plan");
        for (std::size_t index = 0; index < prefilterPlan.size(); ++index) {
            const auto& draw = prefilterPlan[index];
            commands.bindPipelineEx(prefilterPipelines_[1]);
            commands.bindDescriptorSetEx(resources_.prefilterMipSet);
            commands.bindDescriptorSetEx(resources_.prefilterConstantSets[index], 1);
            const RenderingInfoEx mipInfo{
                .colors = {{.texture = resources_.prefilteredSkybox, .mipLevel = draw.mipLevel}},
                .extent = {draw.width, draw.height, 1},
            };
            commands.beginRenderingEx(mipInfo);
            commands.draw(3);
            commands.endRenderingEx();
        }
    }

    commands.transitionEx(skybox_);
    const auto plan = buildDayoEnvironmentDispatchPlan(extent, buildSkyboxSampler);
    for (const auto& dispatch : plan) {
        const auto index = static_cast<std::size_t>(dispatch.pass);
        const auto descriptorSet = index < kPdfPassCount ? resources_.pdfSet : resources_.shSet;
        commands.bindPipelineEx(pipelines_[index]);
        commands.bindDescriptorSetEx(descriptorSet);
        commands.dispatch(dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]);
        commands.memoryBarrierEx();
    }

    source_ = source;
    sourceVersion_ = sourceVersion;
    hlslDirectory_ = normalizedHlslDirectory;
    extent_ = extent;
    buildSkyboxSampler_ = buildSkyboxSampler;
    buildSkyboxPrefilter_ = buildSkyboxPrefilter;
    ++generation_;
    return true;
}

bool NativeDayoEnvironmentRuntime::ensurePipelines(Device& device, const std::filesystem::path& hlslDirectory,
                                                   std::string* error) {
    if (std::ranges::all_of(pipelines_, [](const auto pipeline) { return pipeline.valid(); })) {
        if (hlslDirectory == hlslDirectory_)
            return true;
        try {
            device.waitIdle();
        } catch (const std::exception& exception) {
            setError(error, std::string("Dayo environment could not retire shaders from the prior HLSL root: ") +
                                exception.what());
            return false;
        } catch (...) {
            setError(error, "Dayo environment could not retire shaders from the prior HLSL root");
            return false;
        }
        destroyResources(&device, resources_);
        destroyPipelines(&device);
        device_ = nullptr;
        skybox_ = {};
        sourceSkybox_ = {};
        source_.clear();
        hlslDirectory_.clear();
        extent_ = {};
        sourceVersion_ = 0;
        buildSkyboxSampler_ = false;
        buildSkyboxPrefilter_ = false;
    }

    const fx::FxShaderCompiler compiler;
    if (!compiler.available() || !isDxc(compiler.executable())) {
        setError(error, "MikuMikuDayo 1.30 system environment shaders require DXC");
        return false;
    }

    handles::DescriptorSetLayoutHandle descriptorLayout{};
    handles::PipelineLayoutHandle pipelineLayout{};
    std::array<handles::ShaderHandle, 6> shaders{};
    std::array<handles::PipelineHandle, 6> pipelines{};
    const auto destroyPartial = [&]() {
        for (const auto pipeline : pipelines)
            if (pipeline.valid()) {
                try {
                    device.destroyPipelineEx(pipeline);
                } catch (...) {
                }
            }
        for (const auto shader : shaders)
            if (shader.valid()) {
                try {
                    device.destroyShaderEx(shader);
                } catch (...) {
                }
            }
        if (pipelineLayout.valid()) {
            try {
                device.destroyPipelineLayoutEx(pipelineLayout);
            } catch (...) {
            }
        }
        if (descriptorLayout.valid()) {
            try {
                device.destroyDescriptorSetLayoutEx(descriptorLayout);
            } catch (...) {
            }
        }
    };

    try {
        const auto pdfPath = hlslDirectory / "system" / "skyboxPDF.hlsl";
        const auto shPath = hlslDirectory / "system" / "skyboxSH.hlsl";
        const auto pdfSource = readTextFile(pdfPath);
        const auto shSource = readTextFile(shPath);
        descriptorLayout = device.createDescriptorSetLayoutEx(environmentLayout());
        if (!descriptorLayout.valid())
            throw std::runtime_error("Dayo environment descriptor layout allocation returned an invalid handle");
        pipelineLayout = device.createPipelineLayoutEx({.setLayouts = {descriptorLayout}});
        if (!pipelineLayout.valid())
            throw std::runtime_error("Dayo environment pipeline layout allocation returned an invalid handle");

        for (std::size_t index = 0; index < kPdfEntries.size(); ++index) {
            const auto& path = index < kPdfPassCount ? pdfPath : shPath;
            const auto& source = index < kPdfPassCount ? pdfSource : shSource;
            fx::FxShaderCompileRequest request{.hlsl = source,
                                               .sourcePath = path,
                                               .entryPoint = std::string(kPdfEntries[index]),
                                               .stage = fx::FxShaderStage::compute,
                                               .includeDirectories = {hlslDirectory / "system", hlslDirectory}};
            const auto artifact = compiler.compile(request);
            shaders[index] = device.createShaderEx({.spirv = artifact.spirv,
                                                    .entryPoint = std::string(kPdfEntries[index]),
                                                    .stage = ShaderStageMask::compute});
            if (!shaders[index].valid())
                throw std::runtime_error("Dayo environment shader allocation returned an invalid handle");
            pipelines[index] = device.createComputePipelineEx({.layout = pipelineLayout, .shaders = {shaders[index]}});
            if (!pipelines[index].valid())
                throw std::runtime_error("Dayo environment pipeline allocation returned an invalid handle");
        }
    } catch (const std::exception& exception) {
        destroyPartial();
        setError(error, std::string("Dayo environment HLSL initialization failed: ") + exception.what());
        return false;
    } catch (...) {
        destroyPartial();
        setError(error, "Dayo environment HLSL initialization failed");
        return false;
    }

    descriptorLayout_ = descriptorLayout;
    pipelineLayout_ = pipelineLayout;
    shaders_ = shaders;
    pipelines_ = pipelines;
    hlslDirectory_ = hlslDirectory;
    device_ = &device;
    return true;
}

bool NativeDayoEnvironmentRuntime::ensurePrefilterPipelines(Device& device, std::string* error) {
    if (std::ranges::all_of(prefilterPipelines_, [](const auto pipeline) { return pipeline.valid(); }))
        return true;

    handles::DescriptorSetLayoutHandle descriptorLayout{};
    handles::DescriptorSetLayoutHandle constantLayout{};
    handles::PipelineLayoutHandle pipelineLayout{};
    std::array<handles::ShaderHandle, 3> shaders{};
    std::array<handles::PipelineHandle, 2> pipelines{};
    const auto destroyPartial = [&]() {
        for (const auto pipeline : pipelines)
            if (pipeline.valid()) {
                try {
                    device.destroyPipelineEx(pipeline);
                } catch (...) {
                }
            }
        for (const auto shader : shaders)
            if (shader.valid()) {
                try {
                    device.destroyShaderEx(shader);
                } catch (...) {
                }
            }
        if (pipelineLayout.valid()) {
            try {
                device.destroyPipelineLayoutEx(pipelineLayout);
            } catch (...) {
            }
        }
        for (const auto layout : {constantLayout, descriptorLayout})
            if (layout.valid()) {
                try {
                    device.destroyDescriptorSetLayoutEx(layout);
                } catch (...) {
                }
            }
    };

    try {
        const fx::FxShaderCompiler compiler;
        if (!compiler.available() || !isDxc(compiler.executable()))
            throw std::runtime_error("MikuMikuDayo 1.30 SkyboxPrefilter requires DXC");

        DescriptorSetLayoutDesc textureBindings;
        textureBindings.bindings = {
            {kSkyboxBinding, DescriptorKind::sampledImage, 1, ShaderStageMask::fragment},
            {kPrefilterSamplerBinding, DescriptorKind::sampler, 1, ShaderStageMask::fragment},
        };
        descriptorLayout = device.createDescriptorSetLayoutEx(textureBindings);
        DescriptorSetLayoutDesc constantBindings;
        constantBindings.bindings = {
            {kPrefilterConstantsBinding, DescriptorKind::uniformBuffer, 1, ShaderStageMask::fragment},
        };
        constantLayout = device.createDescriptorSetLayoutEx(constantBindings);
        if (!descriptorLayout.valid() || !constantLayout.valid())
            throw std::runtime_error("Dayo SkyboxPrefilter descriptor layout allocation failed");
        pipelineLayout = device.createPipelineLayoutEx({.setLayouts = {descriptorLayout, constantLayout}});
        if (!pipelineLayout.valid())
            throw std::runtime_error("Dayo SkyboxPrefilter pipeline layout allocation failed");

        const auto sourcePath = hlslDirectory_ / "system" / "skyboxPrefilter.hlsl";
        const auto compileShader = [&](std::size_t index, std::string entry, fx::FxShaderStage stage) {
            const auto artifact = compiler.compile({.hlsl = std::string(kSkyboxPrefilterHlsl),
                                                    .sourcePath = sourcePath,
                                                    .entryPoint = entry,
                                                    .stage = stage});
            shaders[index] = device.createShaderEx(
                {.spirv = artifact.spirv,
                 .entryPoint = std::move(entry),
                 .stage = stage == fx::FxShaderStage::vertex ? ShaderStageMask::vertex : ShaderStageMask::fragment});
            if (!shaders[index].valid())
                throw std::runtime_error("Dayo SkyboxPrefilter shader allocation failed");
        };
        compileShader(0, "VS", fx::FxShaderStage::vertex);
        compileShader(1, "PSCopy", fx::FxShaderStage::fragment);
        compileShader(2, "PS", fx::FxShaderStage::fragment);

        const auto createPipeline = [&](handles::ShaderHandle pixelShader, bool blend) {
            GraphicsPipelineDescEx descriptor;
            descriptor.layout = pipelineLayout;
            descriptor.shaders = {shaders[0], pixelShader};
            descriptor.colorFormats = {PixelFormat::rgba16Float};
            descriptor.rasterizer.cullMode = CullModeEx::none;
            if (blend) {
                BlendAttachmentStateEx state;
                state.enabled = true;
                state.srcColor = BlendFactorEx::srcAlpha;
                state.dstColor = BlendFactorEx::oneMinusSrcAlpha;
                state.srcAlpha = BlendFactorEx::one;
                state.dstAlpha = BlendFactorEx::zero;
                descriptor.blendAttachments = {state};
            }
            return device.createGraphicsPipelineEx(descriptor);
        };
        pipelines[0] = createPipeline(shaders[1], false);
        pipelines[1] = createPipeline(shaders[2], true);
        if (!pipelines[0].valid() || !pipelines[1].valid())
            throw std::runtime_error("Dayo SkyboxPrefilter graphics pipeline allocation failed");
    } catch (const std::exception& exception) {
        destroyPartial();
        setError(error, std::string("Dayo SkyboxPrefilter initialization failed: ") + exception.what());
        return false;
    } catch (...) {
        destroyPartial();
        setError(error, "Dayo SkyboxPrefilter initialization failed");
        return false;
    }

    prefilterDescriptorLayout_ = descriptorLayout;
    prefilterConstantLayout_ = constantLayout;
    prefilterPipelineLayout_ = pipelineLayout;
    prefilterShaders_ = shaders;
    prefilterPipelines_ = pipelines;
    return true;
}

bool NativeDayoEnvironmentRuntime::createResources(Device& device, handles::TextureHandle skybox, Extent3D extent,
                                                   bool buildSkyboxSampler, bool buildSkyboxPrefilter,
                                                   Resources& result, std::string* error) {
    try {
        const auto pixelCount = static_cast<std::uint64_t>(extent.width) * extent.height;
        if (buildSkyboxSampler) {
            result.skywalker = createBuffer(device, bufferByteSize(pixelCount, sizeof(DayoWalkerAlias), "Skywalker"));
            result.skywalkerRow =
                createBuffer(device, bufferByteSize(extent.height, sizeof(DayoWalkerAlias), "SkywalkerRow"));
            result.worker = createBuffer(device, bufferByteSize(pixelCount, 8, "SkyWorker"));
            result.workerRow = createBuffer(device, bufferByteSize(extent.height, 8, "SkyWorkerRow"));
            result.skyLum = createBuffer(device, bufferByteSize(pixelCount, sizeof(float), "SkyLum"));
            result.skyLumRow = createBuffer(device, bufferByteSize(extent.height, sizeof(float), "SkyLumRow"));
        } else {
            result.skywalker = createDummyWalkerBuffer(device);
            result.skywalkerRow = createDummyWalkerBuffer(device);
        }
        result.skyboxShX =
            createBuffer(device, bufferByteSize(extent.height, sizeof(DayoSphericalHarmonics), "SkyboxSHX"));
        result.skyboxSh = createBuffer(device, sizeof(DayoSphericalHarmonics));

        auto resourceSkybox = skybox;
        if (buildSkyboxPrefilter) {
            const auto prefilterPlan = buildDayoSkyboxPrefilterPlan(extent);
            std::uint32_t mipLevels = 1;
            for (auto dimension = std::max(extent.width, extent.height); dimension > 1;
                 dimension = std::max(dimension / 2U, 1U))
                ++mipLevels;
            result.prefilteredSkybox = device.createTextureEx({
                .dimension = TextureDimension::d2,
                .extent = extent,
                .format = PixelFormat::rgba16Float,
                .mipLevels = mipLevels,
                .arrayLayers = 1,
                .usage = ResourceUsage::sampledRead | ResourceUsage::colorAttachment | ResourceUsage::transferSrc |
                         ResourceUsage::transferDst,
                .lifetime = ResourceLifetime::persistent,
            });
            if (!result.prefilteredSkybox.valid())
                throw std::runtime_error("Dayo SkyboxPrefilter texture allocation returned an invalid handle");

            result.prefilterSampler = device.createSamplerEx({.filter = SamplerFilter::linear,
                                                              .addressU = SamplerAddressMode::repeat,
                                                              .addressV = SamplerAddressMode::clampToEdge,
                                                              .addressW = SamplerAddressMode::clampToEdge,
                                                              .minLod = 0.0F,
                                                              .maxLod = 0.0F});
            if (!result.prefilterSampler.valid())
                throw std::runtime_error("Dayo SkyboxPrefilter sampler allocation returned an invalid handle");

            const auto makeTextureSet = [&](handles::TextureHandle sampled) {
                const std::array bindings{
                    DescriptorBindingEx{.slot = kSkyboxBinding, .arrayElement = 0, .texture = sampled},
                    DescriptorBindingEx{
                        .slot = kPrefilterSamplerBinding, .arrayElement = 0, .sampler = result.prefilterSampler},
                };
                return device.allocateDescriptorSetEx(prefilterDescriptorLayout_, bindings);
            };
            result.prefilterCopySet = makeTextureSet(skybox);
            const std::array mipBindings{
                DescriptorBindingEx{
                    .slot = kSkyboxBinding, .arrayElement = 0, .texture = result.prefilteredSkybox, .mipLevel = 0},
                DescriptorBindingEx{
                    .slot = kPrefilterSamplerBinding, .arrayElement = 0, .sampler = result.prefilterSampler},
            };
            result.prefilterMipSet = device.allocateDescriptorSetEx(prefilterDescriptorLayout_, mipBindings);
            if (!result.prefilterCopySet.valid() || !result.prefilterMipSet.valid())
                throw std::runtime_error("Dayo SkyboxPrefilter texture descriptor allocation failed");

            result.prefilterConstants.reserve(prefilterPlan.size());
            result.prefilterConstantSets.reserve(prefilterPlan.size());
            for (const auto& draw : prefilterPlan) {
                const DayoPrefilterConstants constants{
                    .roughness = std::bit_cast<std::uint32_t>(draw.roughness),
                    .iteration = draw.iteration,
                    .alpha = std::bit_cast<std::uint32_t>(draw.alpha),
                    .samples = draw.samples,
                };
                const auto buffer =
                    device.createBufferEx({.size = sizeof(constants),
                                           .usage = ResourceUsage::uniformRead | ResourceUsage::hostRead,
                                           .cpuVisible = true,
                                           .lifetime = ResourceLifetime::persistent});
                if (!buffer.valid())
                    throw std::runtime_error("Dayo SkyboxPrefilter constants allocation returned an invalid handle");
                result.prefilterConstants.push_back(buffer);
                device.uploadBufferEx(buffer, std::as_bytes(std::span<const DayoPrefilterConstants>(&constants, 1)), 0);
                const std::array binding{
                    DescriptorBindingEx{.slot = kPrefilterConstantsBinding, .arrayElement = 0, .buffer = buffer}};
                const auto set = device.allocateDescriptorSetEx(prefilterConstantLayout_, binding);
                if (!set.valid())
                    throw std::runtime_error("Dayo SkyboxPrefilter constants descriptor allocation failed");
                result.prefilterConstantSets.push_back(set);
            }
            resourceSkybox = result.prefilteredSkybox;
        }

        if (buildSkyboxSampler) {
            const auto bindings = pdfBindings(result.skywalker, result.skywalkerRow, result.worker, result.workerRow,
                                              result.skyLum, result.skyLumRow, resourceSkybox);
            result.pdfSet = device.allocateDescriptorSetEx(descriptorLayout_, bindings);
            if (!result.pdfSet.valid())
                throw std::runtime_error("Dayo Skywalker descriptor allocation returned an invalid handle");
        }
        const auto bindings = shBindings(result.skyboxShX, result.skyboxSh, resourceSkybox);
        result.shSet = device.allocateDescriptorSetEx(descriptorLayout_, bindings);
        if (!result.shSet.valid())
            throw std::runtime_error("Dayo SH descriptor allocation returned an invalid handle");
    } catch (const std::exception& exception) {
        destroyResources(&device, result);
        setError(error, std::string("Dayo environment buffer allocation failed: ") + exception.what());
        return false;
    } catch (...) {
        destroyResources(&device, result);
        setError(error, "Dayo environment buffer allocation failed");
        return false;
    }
    return true;
}

void NativeDayoEnvironmentRuntime::destroyResources(Device* device, Resources& resources) noexcept {
    if (device != nullptr) {
        for (const auto descriptorSet :
             {resources.pdfSet, resources.shSet, resources.prefilterCopySet, resources.prefilterMipSet})
            if (descriptorSet.valid()) {
                try {
                    device->destroyDescriptorSetEx(descriptorSet);
                } catch (...) {
                }
            }
        for (const auto descriptorSet : resources.prefilterConstantSets)
            if (descriptorSet.valid()) {
                try {
                    device->destroyDescriptorSetEx(descriptorSet);
                } catch (...) {
                }
            }
        for (const auto buffer : {resources.skyboxShX, resources.skyboxSh, resources.skyLumRow, resources.skyLum,
                                  resources.workerRow, resources.worker, resources.skywalkerRow, resources.skywalker})
            if (buffer.valid()) {
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
        for (const auto buffer : resources.prefilterConstants)
            if (buffer.valid()) {
                try {
                    device->destroyBufferEx(buffer);
                } catch (...) {
                }
            }
        if (resources.prefilteredSkybox.valid()) {
            try {
                device->destroyTextureEx(resources.prefilteredSkybox);
            } catch (...) {
            }
        }
        if (resources.prefilterSampler.valid()) {
            try {
                device->destroySamplerEx(resources.prefilterSampler);
            } catch (...) {
            }
        }
    }
    resources = {};
}

void NativeDayoEnvironmentRuntime::destroyPipelines(Device* device) noexcept {
    if (device != nullptr) {
        for (const auto pipeline : pipelines_)
            if (pipeline.valid()) {
                try {
                    device->destroyPipelineEx(pipeline);
                } catch (...) {
                }
            }
        for (const auto pipeline : prefilterPipelines_)
            if (pipeline.valid()) {
                try {
                    device->destroyPipelineEx(pipeline);
                } catch (...) {
                }
            }
        for (const auto shader : shaders_)
            if (shader.valid()) {
                try {
                    device->destroyShaderEx(shader);
                } catch (...) {
                }
            }
        for (const auto shader : prefilterShaders_)
            if (shader.valid()) {
                try {
                    device->destroyShaderEx(shader);
                } catch (...) {
                }
            }
        if (pipelineLayout_.valid()) {
            try {
                device->destroyPipelineLayoutEx(pipelineLayout_);
            } catch (...) {
            }
        }
        if (prefilterPipelineLayout_.valid()) {
            try {
                device->destroyPipelineLayoutEx(prefilterPipelineLayout_);
            } catch (...) {
            }
        }
        if (descriptorLayout_.valid()) {
            try {
                device->destroyDescriptorSetLayoutEx(descriptorLayout_);
            } catch (...) {
            }
        }
        for (const auto layout : {prefilterConstantLayout_, prefilterDescriptorLayout_})
            if (layout.valid()) {
                try {
                    device->destroyDescriptorSetLayoutEx(layout);
                } catch (...) {
                }
            }
    }
    pipelines_.fill({});
    shaders_.fill({});
    prefilterPipelines_.fill({});
    prefilterShaders_.fill({});
    pipelineLayout_ = {};
    descriptorLayout_ = {};
    prefilterPipelineLayout_ = {};
    prefilterDescriptorLayout_ = {};
    prefilterConstantLayout_ = {};
}

void NativeDayoEnvironmentRuntime::setError(std::string* error, std::string value) const {
    if (error != nullptr)
        *error = std::move(value);
}

void NativeDayoEnvironmentRuntime::apply(NativeSceneResourceBindings& bindings) const noexcept {
    // Empty handles intentionally clear prior bindings; the scene resource
    // store substitutes its placeholders and drops the host-resource bits.
    bindings.skybox = skybox_;
    bindings.skywalker = resources_.skywalker;
    bindings.skywalkerRow = resources_.skywalkerRow;
    bindings.skyboxSh = resources_.skyboxSh;
}

void NativeDayoEnvironmentRuntime::reset() noexcept {
    Device* device = device_;
    const bool hasResources = resources_.skywalker.valid() || resources_.skywalkerRow.valid() ||
                              resources_.skyboxSh.valid() || resources_.skyboxShX.valid() ||
                              resources_.prefilteredSkybox.valid();
    if (device != nullptr && (hasResources || descriptorLayout_.valid() || prefilterDescriptorLayout_.valid())) {
        try {
            device->waitIdle();
        } catch (...) {
        }
    }
    destroyResources(device, resources_);
    destroyPipelines(device);
    device_ = nullptr;
    skybox_ = {};
    sourceSkybox_ = {};
    source_.clear();
    hlslDirectory_.clear();
    extent_ = {};
    sourceVersion_ = 0;
    buildSkyboxSampler_ = false;
    buildSkyboxPrefilter_ = false;
}

} // namespace dayo::graphics
