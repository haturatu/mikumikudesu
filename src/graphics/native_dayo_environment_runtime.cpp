#include "graphics/native_dayo_environment_runtime.hpp"

#include <algorithm>
#include <array>
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
constexpr std::uint32_t kSkyboxBinding = 16; // DXC's Vulkan t0 shift.

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

NativeDayoEnvironmentRuntime::~NativeDayoEnvironmentRuntime() {
    reset();
}

bool NativeDayoEnvironmentRuntime::sync(Device& device, CommandList& commands, handles::TextureHandle skybox,
                                        Extent3D extent, const std::filesystem::path& source,
                                        std::uint64_t sourceVersion, const std::filesystem::path& hlslDirectory,
                                        bool buildSkyboxSampler, std::string* error) {
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
    if (ready() && skybox_ == skybox && source_ == source && sourceVersion_ == sourceVersion && sameExtent &&
        normalizedHlslDirectory == hlslDirectory_ && buildSkyboxSampler_ == buildSkyboxSampler)
        return true;
    if (!ensurePipelines(device, normalizedHlslDirectory, error))
        return false;

    Resources next;
    if (!createResources(device, skybox, extent, buildSkyboxSampler, next, error))
        return false;

    const bool hasCurrentResources = resources_.skywalker.valid() || resources_.skywalkerRow.valid() ||
                                     resources_.skyboxSh.valid() || resources_.skyboxShX.valid();
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
    skybox_ = skybox;

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
        source_.clear();
        hlslDirectory_.clear();
        extent_ = {};
        sourceVersion_ = 0;
        buildSkyboxSampler_ = false;
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

bool NativeDayoEnvironmentRuntime::createResources(Device& device, handles::TextureHandle skybox, Extent3D extent,
                                                   bool buildSkyboxSampler, Resources& result, std::string* error) {
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

        if (buildSkyboxSampler) {
            const auto bindings = pdfBindings(result.skywalker, result.skywalkerRow, result.worker, result.workerRow,
                                              result.skyLum, result.skyLumRow, skybox);
            result.pdfSet = device.allocateDescriptorSetEx(descriptorLayout_, bindings);
            if (!result.pdfSet.valid())
                throw std::runtime_error("Dayo Skywalker descriptor allocation returned an invalid handle");
        }
        const auto bindings = shBindings(result.skyboxShX, result.skyboxSh, skybox);
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
        for (const auto descriptorSet : {resources.pdfSet, resources.shSet})
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
        for (const auto shader : shaders_)
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
        if (descriptorLayout_.valid()) {
            try {
                device->destroyDescriptorSetLayoutEx(descriptorLayout_);
            } catch (...) {
            }
        }
    }
    pipelines_.fill({});
    shaders_.fill({});
    pipelineLayout_ = {};
    descriptorLayout_ = {};
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
                              resources_.skyboxSh.valid() || resources_.skyboxShX.valid();
    if (device != nullptr && (hasResources || descriptorLayout_.valid())) {
        try {
            device->waitIdle();
        } catch (...) {
        }
    }
    destroyResources(device, resources_);
    destroyPipelines(device);
    device_ = nullptr;
    skybox_ = {};
    source_.clear();
    hlslDirectory_.clear();
    extent_ = {};
    sourceVersion_ = 0;
    buildSkyboxSampler_ = false;
}

} // namespace dayo::graphics
