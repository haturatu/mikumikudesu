#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
    bool doubleSided {};
    std::uint32_t textureSlot {};
};

struct PreviewTexture {
    std::uint32_t width {};
    std::uint32_t height {};
    std::span<const std::uint8_t> rgba;
};

using BufferHandle = std::uint64_t;
using TextureHandle = std::uint64_t;
using PipelineHandle = std::uint64_t;

class CommandList {
public:
    virtual ~CommandList() = default;
    virtual void transition(TextureHandle texture) = 0;
    virtual void bindPipeline(PipelineHandle pipeline) = 0;
    virtual void draw(std::uint32_t vertexCount, std::uint32_t instanceCount = 1) = 0;
    virtual void dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0;
    virtual void traceRays(std::uint32_t width, std::uint32_t height) = 0;
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

    [[nodiscard]] virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual TextureHandle createTexture(const TextureDesc& desc) = 0;

protected:
    Device() = default;
};

[[nodiscard]] std::unique_ptr<Device> createVulkanDevice(platform::Window& window,
                                                         bool validation);
[[nodiscard]] std::string_view toString(RendererKind renderer) noexcept;

} // namespace dayo::graphics
