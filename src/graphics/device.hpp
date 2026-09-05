#pragma once

#include "core/image.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dayo::platform {
class Window;
}

namespace dayo::graphics {

enum class RendererKind { preview, subayai, bdpt };

struct GraphicsConvention {
    bool depthZeroToOne{true};
    bool framebufferYFlip{true};
    bool rightHanded{};
};

struct DeviceCapabilities {
    std::string gpuName;
    std::string driverName;
    std::uint32_t vendorId{};
    std::uint32_t apiVersion{};
    bool discreteGpu{};
    bool swapchain{};
    bool bufferDeviceAddress{};
    bool descriptorIndexing{};
    bool accelerationStructure{};
    bool rayTracingPipeline{};
    bool rayQuery{};
    bool fragmentShaderBarycentric{};
    bool nativeSubayai{};
    bool nativeBdpt{};

    [[nodiscard]] bool supportsPreview() const noexcept {
        return swapchain;
    }
    [[nodiscard]] bool hardwareSupportsSubayai() const noexcept {
        return supportsPreview() && bufferDeviceAddress && descriptorIndexing && accelerationStructure && rayQuery &&
               fragmentShaderBarycentric;
    }
    [[nodiscard]] bool hardwareSupportsBdpt() const noexcept {
        return hardwareSupportsSubayai() && rayTracingPipeline;
    }
    [[nodiscard]] bool supportsSubayai() const noexcept {
        return nativeSubayai && hardwareSupportsSubayai();
    }
    [[nodiscard]] bool supportsBdpt() const noexcept {
        return nativeBdpt && hardwareSupportsBdpt();
    }
    [[nodiscard]] bool supports(RendererKind renderer) const noexcept;
    [[nodiscard]] std::string missingFeatures(RendererKind renderer) const;
    [[nodiscard]] std::string json() const;
};

struct BufferDesc {
    std::size_t size{};
    enum class Usage { vertex, index, uniform, storage, accelerationStructure } usage{};
    bool cpuVisible{};
};

struct TextureDesc {
    std::uint32_t width{};
    std::uint32_t height{};
    enum class Format { rgba8Unorm, rgba8Srgb, rgba16Float, rgba32Float, depth32Float } format{};
    bool storage{};
    bool renderTarget{};
};

struct PreviewVertex {
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
    std::int32_t bones[4]{-1, -1, -1, -1};
    float weights[4]{1.0F, 0.0F, 0.0F, 0.0F};
    float sdefC[3]{};
    float sdefHalfDelta[3]{};
    std::uint32_t skinningType{};
    std::uint32_t gpuSkinning{};
    float cloneOffset{};
    float edgeScale{1.0F};
};

enum class PreviewSkinningType : std::uint32_t {
    bdef1,
    bdef2,
    bdef4,
    sdef,
    qdef,
};

struct PreviewBoneTransform {
    float rotation[4]{0.0F, 0.0F, 0.0F, 1.0F};
    float translation[4]{};
};

struct PreviewMaterial {
    float diffuse[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float ambient[3]{0.2F, 0.2F, 0.2F};
    float shininess{};
    float specular[3]{};
    float textureMultiply[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float textureAdd[4]{};
    float sphereMultiply[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float sphereAdd[4]{};
    float toonMultiply[4]{1.0F, 1.0F, 1.0F, 1.0F};
    float toonAdd[4]{};
    float edgeColor[4]{};
    float edgeSize{};
    bool doubleSided{};
    bool edgeEnabled{};
    std::uint32_t textureSlot{};
    std::uint32_t toonTextureSlot{};
    std::uint32_t sphereTextureSlot{};
    std::uint32_t toonMode{};
    std::uint32_t sphereMode{};
};

struct PreviewDraw {
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint32_t materialIndex{};
};

// This is the renderer-facing storage-buffer layout. Keep it explicitly
// vector-aligned so it matches the StructuredBuffer declaration in preview.hlsl.
struct PreviewMaterialGpu {
    float diffuse[4]{};
    float ambientShininess[4]{};
    float specular[4]{};
    float textureMultiply[4]{};
    float textureAdd[4]{};
    float sphereMultiply[4]{};
    float sphereAdd[4]{};
    float toonMultiply[4]{};
    float toonAdd[4]{};
    float edgeColor[4]{};
    float edgeSize{};
    std::uint32_t flags{};
    std::uint32_t reserved[2]{};
};
static_assert(sizeof(PreviewMaterialGpu) == 176);

struct PreviewTexture {
    std::uint32_t width{};
    std::uint32_t height{};
    std::span<const std::uint8_t> rgba;
    bool hasTransparency{};
};

struct PreviewScene {
    float cameraRotation[3]{};
    float cameraDistance{3.0F};
    float target[3]{};
    float verticalFovRadians{0.785398163F};
    float lightDirection[3]{-0.5F, -1.0F, 0.5F};
    bool perspective{true};
    enum class ScreenSource {
        previousFrame,
        backgroundVideo,
        backgroundImage,
        white
    } screenSource{ScreenSource::previousFrame};
    enum class ScreenCrop { none, crop4x3 } screenCrop{ScreenCrop::none};
    bool backgroundEnabled{true};
};

struct PreviewPushConstants {
    std::array<float, 4> camera{};
    std::array<float, 4> target{};
    std::array<float, 4> light{};
};
static_assert(sizeof(PreviewPushConstants) == 48);

struct RenderTargetDesc {
    std::uint32_t width{};
    std::uint32_t height{};
};

using BufferHandle = std::uint64_t;
using TextureHandle = std::uint64_t;
using PipelineHandle = std::uint64_t;
using TextureViewHandle = std::uint64_t;
using SamplerHandle = std::uint64_t;
using ShaderHandle = std::uint64_t;
using AccelerationStructureHandle = std::uint64_t;

struct ShaderDesc {
    std::span<const std::uint32_t> spirv;
    std::string entryPoint{"main"};
};
struct PipelineDesc {
    std::vector<ShaderHandle> shaders;
    bool compute{};
};
struct RayTracingPipelineDesc {
    std::vector<ShaderHandle> rayGeneration;
    std::vector<ShaderHandle> miss;
    std::vector<ShaderHandle> closestHit;
};
struct DescriptorBinding {
    std::uint32_t slot{};
    BufferHandle buffer{};
    TextureViewHandle texture{};
    SamplerHandle sampler{};
};

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
    // Renders the current preview scene without ImGui and returns a CPU image.
    // Backends that do not provide an offscreen target report this explicitly.
    [[nodiscard]] virtual core::ImageRgba8 renderToImage(const RenderTargetDesc&) {
        throw std::logic_error("offscreen rendering is not implemented by this backend");
    }
    virtual void waitIdle() = 0;
    virtual void uploadPreviewMesh(std::span<const PreviewVertex> vertices, std::span<const std::uint32_t> indices) = 0;
    virtual void updatePreviewVertices(std::span<const PreviewVertex> vertices) = 0;
    virtual void updatePreviewBones(std::span<const PreviewBoneTransform> bones) = 0;
    virtual void updatePreviewMaterials(std::span<const PreviewMaterial> materials) = 0;
    virtual void updatePreviewDraws(std::span<const PreviewDraw> draws) = 0;
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

[[nodiscard]] std::unique_ptr<Device> createVulkanDevice(platform::Window& window, bool validation);
[[nodiscard]] std::string_view toString(RendererKind renderer) noexcept;

} // namespace dayo::graphics
