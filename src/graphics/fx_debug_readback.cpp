#include "graphics/fx_debug_readback.hpp"
#include "core/output.hpp"
#include "fx/fx_shader_compiler.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace dayo::graphics {
namespace {
constexpr std::size_t kDebugBudget = 256U * 1024U * 1024U;

void writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("debug dump already exists: " + path.string());
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot create debug dump: " + path.string());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
        throw std::runtime_error("cannot write debug dump: " + path.string());
}

core::ImageData floatSlice(const FxResourceStore::Resource& resource, std::span<const std::uint8_t> bytes,
                           std::uint32_t mip, std::uint32_t slice) {
    if (mip >= 32)
        throw std::out_of_range("debug mip is out of range");
    const auto width = std::max(1U, resource.extent.width >> mip);
    const auto height = std::max(1U, resource.extent.height >> mip);
    const auto depth = std::max(1U, resource.extent.depth >> mip);
    if (slice >= depth)
        throw std::out_of_range("debug depth slice is out of range");
    const auto pixelBytes = pixelFormatByteSize(resource.format);
    const auto planeBytes = checkedResourceMul(checkedResourceMul(width, height), pixelBytes);
    if (bytes.size() != checkedResourceMul(planeBytes, depth))
        throw std::runtime_error("debug readback size mismatch");
    const auto outputBytes = checkedResourceMul(checkedResourceMul(width, height), 16);
    if (outputBytes > kDebugBudget)
        throw std::length_error("debug float conversion exceeds 256 MiB");
    core::ImageData image{.width = width,
                          .height = height,
                          .channels = 4,
                          .type = core::PixelType::float32,
                          .space = core::ColorSpace::linear,
                          .bytes = std::vector<std::uint8_t>(outputBytes)};
    const auto format = toString(resource.format);
    const auto components = isDepthFormat(resource.format)               ? 1U
                            : format.find('A') != std::string_view::npos ? 4U
                            : format.find('G') != std::string_view::npos ? 2U
                                                                         : 1U;
    const auto componentBytes = pixelBytes / components;
    const bool signedInteger = format.ends_with("_SINT");
    const bool signedNormalized = format.ends_with("_SNORM");
    const bool normalized = signedNormalized || format.ends_with("_UNORM") || format.ends_with("_SRGB");
    for (std::size_t index = 0; index < static_cast<std::size_t>(width) * height; ++index) {
        std::array<float, 4> value{0, 0, 0, 1};
        for (std::size_t channel = 0; channel < components; ++channel) {
            const auto* input = bytes.data() + planeBytes * slice + index * pixelBytes + channel * componentBytes;
            std::uint32_t raw = 0;
            for (std::size_t byte = 0; byte < componentBytes; ++byte)
                raw |= std::uint32_t(input[byte]) << (byte * 8U);
            if (resource.format == PixelFormat::depth24Stencil8)
                value[channel] = float(raw & 0xFFFFFFU) / 16777215.0F;
            else if (format.ends_with("_FLOAT"))
                value[channel] = componentBytes == 2 ? core::halfToFloat(static_cast<std::uint16_t>(raw))
                                                     : std::bit_cast<float>(raw);
            else if (signedInteger || signedNormalized) {
                const auto bits = static_cast<unsigned>(componentBytes * 8U);
                const std::int64_t signedValue =
                    (raw & (1U << (bits - 1U))) != 0 ? std::int64_t(raw) - (std::int64_t{1} << bits) : raw;
                value[channel] = signedNormalized
                                     ? std::max(-1.0F, static_cast<float>(signedValue) /
                                                           static_cast<float>((std::uint64_t{1} << (bits - 1U)) - 1U))
                                     : static_cast<float>(signedValue);
            } else
                value[channel] = normalized ? static_cast<float>(raw) /
                                                  static_cast<float>((std::uint64_t{1} << (componentBytes * 8U)) - 1U)
                                            : static_cast<float>(raw);
            if (resource.format == PixelFormat::rgba8Srgb && channel < 3)
                value[channel] = value[channel] <= 0.04045F ? value[channel] / 12.92F
                                                            : std::pow((value[channel] + 0.055F) / 1.055F, 2.4F);
        }
        std::memcpy(image.bytes.data() + index * 16U, value.data(), 16);
    }
    return image;
}

// Temporary GPU objects survive both command-list flushes. Destruction always
// follows completion, including exceptions during readback or file output.
struct DebugGpuObjects {
    Device& device;
    handles::TextureHandle source{}, fallback{}, output{};
    handles::BufferHandle constants{};
    handles::DescriptorSetLayoutHandle setLayout{};
    handles::DescriptorSetHandle set{};
    handles::PipelineLayoutHandle layout{};
    handles::ShaderHandle shader{};
    handles::PipelineHandle pipeline{};
    ~DebugGpuObjects() {
        try {
            device.waitIdle();
        } catch (...) {
        }
        const auto release = [](auto operation) {
            try {
                operation();
            } catch (...) {
            }
        };
        if (set.valid())
            release([&] { device.destroyDescriptorSetEx(set); });
        if (pipeline.valid())
            release([&] { device.destroyPipelineEx(pipeline); });
        if (shader.valid())
            release([&] { device.destroyShaderEx(shader); });
        if (layout.valid())
            release([&] { device.destroyPipelineLayoutEx(layout); });
        if (setLayout.valid())
            release([&] { device.destroyDescriptorSetLayoutEx(setLayout); });
        if (constants.valid())
            release([&] { device.destroyBufferEx(constants); });
        for (const auto texture : {source, fallback, output})
            if (texture.valid())
                release([&] { device.destroyTextureEx(texture); });
    }
};

core::ImageRgba8 debugPreview(Device& device, CommandList& commands, const core::ImageData& input,
                              const FxDebugRequest& request) {
    const auto path = request.hlslDirectory / "system" / "debug.hlsl";
    if (std::filesystem::file_size(path) > 1024U * 1024U)
        throw std::length_error("Dayo debug shader is too large");
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot read Dayo debug shader: " + path.string());
    std::string source{std::istreambuf_iterator<char>(stream), {}};
    const auto replace = [&](std::string_view from, std::string_view to) {
        const auto offset = source.find(from);
        if (offset == std::string::npos)
            throw std::runtime_error("unsupported upstream debug shader ABI: " + std::string(from));
        source.replace(offset, from.size(), to);
    };
    // The debug pass uses none of the scene constants from cb.hlsli. Its local
    // ViewCB and source views receive explicit Vulkan bindings; PS is untouched.
    replace("#include \"../cb.hlsli\"", "namespace Dayo {}");
    replace("cbuffer ViewCB : register(b0)", "[[vk::binding(0,0)]] cbuffer ViewCB");
    replace("Texture2D<float4> Source : register(t0);", "[[vk::binding(1,0)]] Texture2D<float4> Source;");
    replace("Texture3D<float4> Source3D : register(t1);", "[[vk::binding(2,0)]] Texture3D<float4> Source3D;");
    source += R"hlsl(
[[vk::binding(3,0)]] RWTexture2D<float4> DebugOutput;
[numthreads(8,8,1)] void NativeDebug(uint3 id : SV_DispatchThreadID) {
    uint width, height; DebugOutput.GetDimensions(width,height);
    if (id.x >= width || id.y >= height) return;
    VSO vso = (VSO)0; vso.pos = float4(float2(id.xy) + 0.5,0,1);
    DebugOutput[id.xy] = PS(vso);
}
)hlsl";
    const auto artifact =
        fx::FxShaderCompiler{}.compile({.hlsl = source,
                                        .sourcePath = path,
                                        .entryPoint = "NativeDebug",
                                        .stage = fx::FxShaderStage::compute,
                                        .includeDirectories = {path.parent_path(), request.hlslDirectory}});
    DebugGpuObjects gpu{.device = device};
    TextureResourceDesc desc{.dimension = TextureDimension::d2,
                             .extent = {input.width, input.height, 1},
                             .format = PixelFormat::rgba32Float,
                             .usage = ResourceUsage::sampledRead | ResourceUsage::transferDst};
    gpu.source = device.createTextureEx(desc);
    device.uploadTextureEx(gpu.source, input.bytes, 0, 0);
    desc.dimension = TextureDimension::d3;
    desc.extent = {1, 1, 1};
    gpu.fallback = device.createTextureEx(desc);
    const std::array<std::uint8_t, 16> zero{};
    device.uploadTextureEx(gpu.fallback, zero, 0, 0);
    desc.dimension = TextureDimension::d2;
    desc.extent = {input.width, input.height, 1};
    desc.usage = ResourceUsage::storageWrite | ResourceUsage::transferSrc;
    gpu.output = device.createTextureEx(desc);
    const std::array<std::uint32_t, 4> constants{static_cast<std::uint32_t>(request.mode), 0,
                                                 std::bit_cast<std::uint32_t>(request.scale), 0};
    gpu.constants =
        device.createBufferEx({.size = sizeof(constants), .usage = ResourceUsage::uniformRead, .cpuVisible = true});
    device.uploadBufferEx(gpu.constants, std::as_bytes(std::span(constants)), 0);
    gpu.setLayout = device.createDescriptorSetLayoutEx(
        {.bindings = {{0, DescriptorKind::uniformBuffer, 1, ShaderStageMask::compute},
                      {1, DescriptorKind::sampledImage, 1, ShaderStageMask::compute},
                      {2, DescriptorKind::sampledImage, 1, ShaderStageMask::compute},
                      {3, DescriptorKind::storageImage, 1, ShaderStageMask::compute}}});
    gpu.layout = device.createPipelineLayoutEx({.setLayouts = {gpu.setLayout}});
    gpu.shader = device.createShaderEx(
        {.spirv = artifact.spirv, .entryPoint = "NativeDebug", .stage = ShaderStageMask::compute});
    gpu.pipeline = device.createComputePipelineEx({.layout = gpu.layout, .shaders = {gpu.shader}});
    const std::array bindings{
        DescriptorBindingEx{.slot = 0, .buffer = gpu.constants}, DescriptorBindingEx{.slot = 1, .texture = gpu.source},
        DescriptorBindingEx{.slot = 2, .texture = gpu.fallback}, DescriptorBindingEx{.slot = 3, .texture = gpu.output}};
    gpu.set = device.allocateDescriptorSetEx(gpu.setLayout, bindings);
    commands.bindPipelineEx(gpu.pipeline);
    commands.bindDescriptorSetEx(gpu.set);
    commands.dispatch((input.width + 7U) / 8U, (input.height + 7U) / 8U, 1);
    commands.memoryBarrierEx();
    commands.flushAndWaitForHostReadbackEx();
    auto pixels = device.readbackTextureEx(gpu.output, 0, 0);
    core::ImageRgba8 preview{input.width, input.height,
                             std::vector<std::uint8_t>(static_cast<std::size_t>(input.width) * input.height * 4U)};
    for (std::size_t i = 0; i < preview.pixels.size(); ++i) {
        float value = 0;
        std::memcpy(&value, pixels.data() + i * 4U, 4);
        preview.pixels[i] =
            static_cast<std::uint8_t>(std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) * 255.0F : 0.0F);
    }
    return preview;
}
} // namespace

std::uint64_t fxDebugGeneration(const FxResourceStore::Resource& resource) noexcept {
    const auto index = resource.kind == FxResourceStore::Kind::texture ? resource.texture.index : resource.buffer.index;
    const auto generation =
        resource.kind == FxResourceStore::Kind::texture ? resource.texture.generation : resource.buffer.generation;
    return (static_cast<std::uint64_t>(index) << 32U) | generation;
}

FxDebugResult readFxDebugResource(Device& device, CommandList& commands, const FxResourceStore::Resource& resource,
                                  const FxDebugRequest& request) {
    FxDebugResult result;
    result.label = request.owner + " / " + request.name;
    if (fxDebugGeneration(resource) != request.generation)
        throw std::runtime_error("resource changed before debug readback; select it again");
    if (resource.allocationBytes > kDebugBudget)
        throw std::length_error("debug readback exceeds 256 MiB");
    if (request.mode < 0 || request.mode > 5 || !std::isfinite(request.scale))
        throw std::invalid_argument("invalid debug display options");
    commands.memoryBarrierEx();
    commands.flushAndWaitForHostReadbackEx();
    if (resource.kind == FxResourceStore::Kind::buffer) {
        const auto bytes =
            device.readbackBufferEx(resource.buffer, 0, static_cast<std::size_t>(resource.allocationBytes));
        result.buffer.resize(bytes.size());
        std::memcpy(result.buffer.data(), bytes.data(), bytes.size());
        if (!request.dumpPath.empty()) {
            if (request.dumpPath.extension() != ".bin")
                throw std::invalid_argument("buffer debug dump requires .bin");
            writeBytes(request.dumpPath, result.buffer);
        }
    } else if (resource.kind == FxResourceStore::Kind::texture) {
        const auto bytes = device.readbackTextureEx(resource.texture, request.mip, 0);
        const auto slice = floatSlice(resource, bytes, request.mip, request.slice);
        if (!request.dumpPath.empty()) {
            if (std::filesystem::exists(request.dumpPath))
                throw std::runtime_error("debug dump already exists");
            const auto extension = request.dumpPath.extension().string();
            if (extension == ".bin") {
                const auto metadataPath = std::filesystem::path(request.dumpPath.string() + ".json");
                if (std::filesystem::exists(metadataPath))
                    throw std::runtime_error("debug dump metadata already exists");
                std::ostringstream metadataStream;
                metadataStream << "{\"format\":\"" << toString(resource.format) << "\",\"width\":" << slice.width
                               << ",\"height\":" << slice.height
                               << ",\"depth\":" << std::max(1U, resource.extent.depth >> request.mip)
                               << ",\"mip\":" << request.mip << ",\"arrayLayer\":0}";
                const auto metadata = metadataStream.str();
                writeBytes(request.dumpPath, bytes);
                writeBytes(metadataPath, {reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size()});
            } else if (extension == ".exr")
                core::writeFrame(std::filesystem::absolute(request.dumpPath), slice);
            else if (extension == ".png")
                core::writeFrame(std::filesystem::absolute(request.dumpPath), core::halfToRgba8(slice),
                                 core::OutputFormat::png);
            else
                throw std::invalid_argument("texture debug dump requires .bin, .png or .exr");
        }
        try {
            result.preview = debugPreview(device, commands, slice, request);
        } catch (const std::exception& error) {
            result.message = (request.dumpPath.empty() ? std::string{} : "Saved " + request.dumpPath.string() + "; ") +
                             "preview unavailable: " + error.what();
            return result;
        }
    } else
        throw std::invalid_argument("samplers have no readable pixel data");
    result.message = request.dumpPath.empty() ? "Readback complete" : "Saved " + request.dumpPath.string();
    return result;
}
} // namespace dayo::graphics
