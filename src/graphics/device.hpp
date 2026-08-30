#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace dayo::platform { class Window; }

namespace dayo::graphics {

enum class RendererKind { preview, subayai, bdpt };

struct GraphicsConvention {
    bool depthZeroToOne { true };
    bool framebufferYFlip { true };
    bool rightHanded {};
};

struct DeviceCapabilities {
    std::string gpuName;
    std::string driverName;
    std::uint32_t vendorId {};
    std::uint32_t apiVersion {};
    bool discreteGpu {};
    bool swapchain {};
    bool bufferDeviceAddress {};
    bool descriptorIndexing {};
    bool accelerationStructure {};
    bool rayTracingPipeline {};
    bool rayQuery {};
    bool fragmentShaderBarycentric {};
    bool nativeSubayai {};
    bool nativeBdpt {};

    [[nodiscard]] bool supportsPreview() const noexcept { return swapchain; }
    [[nodiscard]] bool hardwareSupportsSubayai() const noexcept {
        return supportsPreview() && bufferDeviceAddress && descriptorIndexing
            && accelerationStructure && rayQuery && fragmentShaderBarycentric;
    }
    [[nodiscard]] bool hardwareSupportsBdpt() const noexcept {
        return hardwareSupportsSubayai() && rayTracingPipeline;
    }
    [[nodiscard]] bool supportsSubayai() const noexcept { return nativeSubayai && hardwareSupportsSubayai(); }
    [[nodiscard]] bool supportsBdpt() const noexcept { return nativeBdpt && hardwareSupportsBdpt(); }
    [[nodiscard]] bool supports(RendererKind renderer) const noexcept;
    [[nodiscard]] std::string missingFeatures(RendererKind renderer) const;
    [[nodiscard]] std::string json() const;
};

struct BufferDesc {
    std::size_t size {};
    enum class Usage { vertex, index, uniform, storage, accelerationStructure } usage {};
    bool cpuVisible {};
};

struct TextureDesc {
    std::uint32_t width {};
    std::uint32_t height {};
    enum class Format { rgba8Unorm, rgba8Srgb, rgba16Float, rgba32Float, depth32Float } format {};
    bool storage {};
    bool renderTarget {};
};

struct PreviewVertex {
    float position[3] {};
    float normal[3] {};
    float uv[2] {};
};

struct PreviewMaterial {
    std::uint32_t firstIndex {};
    std::uint32_t indexCount {};
    float diffuse[4] { 1.0F, 1.0F, 1.0F, 1.0F };
    float ambient[3] { 0.2F, 0.2F, 0.2F };
    float shininess {};
    float specular[3] {};
    float textureMultiply[4] { 1.0F, 1.0F, 1.0F, 1.0F };
    float textureAdd[4] {};
    bool doubleSided {};
    std::uint32_t textureSlot {};
};

struct PreviewTexture {
    std::uint32_t width {};
    std::uint32_t height {};
    std::span<const std::uint8_t> rgba;
};

struct PreviewScene {
    float cameraRotation[3] {};
    float cameraDistance { 3.0F };
    float target[3] {};
    float verticalFovRadians { 0.785398163F };
    float lightDirection[3] { -0.5F, -1.0F, 0.5F };
    bool perspective { true };
    enum class ScreenSource { previousFrame, backgroundVideo, backgroundImage, white } screenSource { ScreenSource::previousFrame };
    enum class ScreenCrop { none, crop4x3 } screenCrop { ScreenCrop::none };
    bool backgroundEnabled { true };
};

using BufferHandle = std::uint64_t;
using TextureHandle = std::uint64_t;
using PipelineHandle = std::uint64_t;
using TextureViewHandle = std::uint64_t;
using SamplerHandle = std::uint64_t;
using ShaderHandle = std::uint64_t;
using AccelerationStructureHandle = std::uint64_t;

struct ShaderDesc { std::span<const std::uint32_t> spirv; std::string entryPoint { "main" }; };
struct PipelineDesc { std::vector<ShaderHandle> shaders; bool compute {}; };
struct RayTracingPipelineDesc { std::vector<ShaderHandle> rayGeneration; std::vector<ShaderHandle> miss; std::vector<ShaderHandle> closestHit; };
struct DescriptorBinding { std::uint32_t slot {}; BufferHandle buffer {}; TextureViewHandle texture {}; SamplerHandle sampler {}; };

class CommandList {
public:
    virtual ~CommandList() = default;
    virtual void transition(TextureHandle texture) = 0;
    virtual void bindPipeline(PipelineHandle pipeline) = 0;
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1) = 0;
    virtual void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;
    virtual void traceRays(std::uint32_t width, std::uint32_t height) = 0;
    virtual void bindResources(std::span<const DescriptorBinding>) {}
    virtual void pushConstants(std::span<const std::byte>) {}
    virtual void copyTexture(TextureHandle, TextureHandle) {}
    virtual void clearTexture(TextureHandle) {}
    virtual void generateMipmaps(TextureHandle) {}
    virtual void buildAccelerationStructure(AccelerationStructureHandle) {}
};

class Device {
public:
    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual const GraphicsConvention& convention() const noexcept = 0;
    [[nodiscard]] virtual RendererKind activeRenderer() const noexcept = 0;
    virtual void selectRenderer(RendererKind requested) = 0;
    virtual void resize() = 0;
    virtual void beginUiFrame() = 0;
    virtual void renderFrame() = 0;
    virtual void waitIdle() = 0;
    virtual void uploadPreviewMesh(std::span<const PreviewVertex> vertices,
                                   std::span<const std::uint32_t> indices) = 0;
    virtual void updatePreviewVertices(std::span<const PreviewVertex> vertices) = 0;
    virtual void updatePreviewMaterials(std::span<const PreviewMaterial> materials) = 0;
    virtual void uploadPreviewTextures(std::span<const PreviewTexture> textures) = 0;
    virtual void uploadPreviewBackground(std::span<const PreviewTexture> textures) = 0;
    // Returns the preview renderer to its empty/fallback state. Project
    // lifecycle resets use this to release GPU resources from the old scene.
    virtual void clearPreviewResources() = 0;
    virtual void updatePreviewScene(const PreviewScene& scene) = 0;

    [[nodiscard]] virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
    // Optional portions of the backend contract. Preview backends keep these
    // methods unavailable; RT/FX backends override them when Vulkan features
    // are available. Unsupported calls fail explicitly instead of returning
    // a zero handle that could be mistaken for a valid resource.
    [[nodiscard]] virtual TextureViewHandle createTextureView(TextureHandle) {
        throw std::logic_error("Texture views are not implemented by this backend");
    }
    [[nodiscard]] virtual SamplerHandle createSampler() {
        throw std::logic_error("Samplers are not implemented by this backend");
    }
    [[nodiscard]] virtual ShaderHandle createShader(const ShaderDesc&) {
        throw std::logic_error("Shaders are not implemented by this backend");
    }
    [[nodiscard]] virtual PipelineHandle createGraphicsPipeline(const PipelineDesc&) {
        throw std::logic_error("Graphics pipelines are not implemented by this backend");
    }
    [[nodiscard]] virtual PipelineHandle createComputePipeline(const PipelineDesc&) {
        throw std::logic_error("Compute pipelines are not implemented by this backend");
    }
    [[nodiscard]] virtual PipelineHandle createRayTracingPipeline(const RayTracingPipelineDesc&) {
        throw std::logic_error("Ray-tracing pipelines are not implemented by this backend");
    }
    [[nodiscard]] virtual AccelerationStructureHandle createBLAS(BufferHandle) {
        throw std::logic_error("BLAS is not implemented by this backend");
    }
    [[nodiscard]] virtual AccelerationStructureHandle createTLAS(std::span<const AccelerationStructureHandle>) {
        throw std::logic_error("TLAS is not implemented by this backend");
    }

protected:
    Device() = default;
};

[[nodiscard]] std::unique_ptr<Device> createVulkanDevice(platform::Window& window,
                                                         bool validation);
[[nodiscard]] std::string_view toString(RendererKind renderer) noexcept;

} // namespace dayo::graphics
