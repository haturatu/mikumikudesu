#pragma once

#include "graphics/device.hpp"
#include "graphics/preview_gpu_scene.hpp"
#include "graphics/preview_render_plan.hpp"
#include "graphics/vulkan/vulkan_acceleration_structure.hpp"

#include <vulkan/vulkan.h>

#if DAYO_ENABLE_VMA
VK_DEFINE_HANDLE(VmaAllocator)
#endif

#include "graphics/vulkan/vulkan_resources.hpp"

#include <array>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dayo::graphics {

class VulkanUploadContext;
class VulkanCommandList;

class VulkanDevice final : public Device {
  public:
    VulkanDevice(platform::Window& window, bool validation);
    ~VulkanDevice() override;

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    [[nodiscard]] const GraphicsConvention& convention() const noexcept override {
        return convention_;
    }
    [[nodiscard]] RendererKind activeRenderer() const noexcept override {
        return activeRenderer_;
    }
    [[nodiscard]] IAccelerationBackend* nativeAccelerationBackend() noexcept override {
        return &accelerationBackend_;
    }
    void selectRenderer(RendererKind requested) override;
    void resize() override;
    void beginUiFrame() override;
    void renderFrame() override;
    void setNativeFrameRecorder(NativeFrameRecorder recorder) override;
    void setNativeRendererAvailability(bool subayai, bool bdpt) override;
    void setPreviewViewportExtent(const RenderTargetDesc& target) override;
    [[nodiscard]] PreviewViewport previewViewport() const noexcept override;
    [[nodiscard]] std::uint64_t previewGpuNanoseconds() const noexcept override {
        return previewGpuNanoseconds_;
    }
    [[nodiscard]] core::ImageRgba8 renderToImage(const RenderTargetDesc& target) override;
    void waitIdle() override;
    void uploadPreviewMesh(std::span<const PreviewVertex> vertices, std::span<const std::uint32_t> indices) override;
    void updatePreviewVertices(std::span<const PreviewVertex> vertices) override;
    void updatePreviewBones(std::span<const PreviewBoneTransform> bones) override;
    void uploadPreviewMorphDeltas(std::span<const PreviewMorphDelta> deltas) override;
    void updatePreviewMorphWeights(std::span<const float> weights) override;
    void updatePreviewMaterials(std::span<const PreviewMaterial> materials) override;
    void updatePreviewDraws(std::span<const PreviewDraw> draws) override;
    void uploadPreviewTextures(std::span<const PreviewTexture> textures) override;
    void uploadPreviewBackground(std::span<const PreviewTexture> textures) override;
    void clearPreviewResources() override;
    void updatePreviewScene(const PreviewScene& scene) override {
        previewGpuScene_.view = scene;
    }
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc) override;
    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc) override;
    [[nodiscard]] handles::TextureHandle createTextureEx(const TextureResourceDesc& desc) override;
    [[nodiscard]] handles::BufferHandle createBufferEx(const BufferResourceDesc& desc) override;
    void destroyTextureEx(handles::TextureHandle handle) override;
    void destroyBufferEx(handles::BufferHandle handle) override;
    void destroySamplerEx(handles::SamplerHandle handle) override;
    void retireTextureEx(handles::TextureHandle handle, std::uint64_t frameIndex) override;
    void retireBufferEx(handles::BufferHandle handle, std::uint64_t frameIndex) override;
    [[nodiscard]] handles::SamplerHandle createSamplerEx() override;
    [[nodiscard]] handles::SamplerHandle createSamplerEx(const SamplerResourceDesc& desc) override;
    [[nodiscard]] handles::ShaderHandle createShaderEx(const ShaderDesc& desc) override;
    void destroyShaderEx(handles::ShaderHandle handle) override;
    [[nodiscard]] handles::PipelineLayoutHandle createPipelineLayoutEx(const PipelineLayoutDesc& desc) override;
    void destroyPipelineLayoutEx(handles::PipelineLayoutHandle handle) override;
    [[nodiscard]] handles::PipelineHandle createGraphicsPipelineEx(const GraphicsPipelineDescEx& desc) override;
    [[nodiscard]] handles::PipelineHandle createComputePipelineEx(const ComputePipelineDescEx& desc) override;
    [[nodiscard]] handles::PipelineHandle createRayTracingPipelineEx(const RayTracingPipelineDescEx& desc) override;
    void destroyPipelineEx(handles::PipelineHandle handle) override;
    [[nodiscard]] handles::AccelerationStructureHandle createBlasEx(const BlasGeometryDesc& desc) override;
    [[nodiscard]] handles::AccelerationStructureHandle rebuildBlasEx(handles::AccelerationStructureHandle blas,
                                                                     const BlasGeometryDesc& desc) override;
    void refitBlasEx(handles::AccelerationStructureHandle blas, const BlasGeometryDesc& desc) override;
    [[nodiscard]] handles::AccelerationStructureHandle
    createTlasEx(std::span<const AccelerationInstanceDesc> instances) override;
    [[nodiscard]] handles::AccelerationStructureHandle
    rebuildTlasEx(handles::AccelerationStructureHandle tlas,
                  std::span<const AccelerationInstanceDesc> instances) override;
    void updateTlasEx(handles::AccelerationStructureHandle tlas,
                      std::span<const AccelerationInstanceDesc> instances) override;
    void destroyAccelerationStructureEx(handles::AccelerationStructureHandle handle) override;
    [[nodiscard]] handles::ShaderBindingTableHandle
    createShaderBindingTable(const ShaderBindingTableDesc& desc) override;
    void destroyShaderBindingTable(handles::ShaderBindingTableHandle handle) override;
    void copyBufferEx(handles::BufferHandle source, handles::BufferHandle destination) override;
    void copyBufferToTextureEx(handles::BufferHandle source, handles::TextureHandle destination) override;
    void copyTextureToBufferEx(handles::TextureHandle source, handles::BufferHandle destination) override;
    void copyTextureEx(handles::TextureHandle source, handles::TextureHandle destination) override;
    void clearTextureEx(handles::TextureHandle texture, const std::array<float, 4>& value) override;
    void clearBufferEx(handles::BufferHandle buffer, std::uint32_t value) override;
    void generateMipmapsEx(handles::TextureHandle texture) override;
    void uploadTextureEx(handles::TextureHandle texture, std::span<const std::uint8_t> bytes, std::uint32_t mipLevel,
                         std::uint32_t arrayLayer) override;
    [[nodiscard]] std::vector<std::uint8_t> readbackTextureEx(handles::TextureHandle texture, std::uint32_t mipLevel,
                                                              std::uint32_t arrayLayer) override;
    [[nodiscard]] handles::DescriptorSetLayoutHandle
    createDescriptorSetLayoutEx(const DescriptorSetLayoutDesc& desc) override;
    void destroyDescriptorSetLayoutEx(handles::DescriptorSetLayoutHandle handle) override;
    [[nodiscard]] handles::DescriptorSetHandle
    allocateDescriptorSetEx(handles::DescriptorSetLayoutHandle layout,
                            std::span<const DescriptorBindingEx> bindings) override;
    void updateDescriptorSetEx(handles::DescriptorSetHandle set,
                               std::span<const DescriptorBindingEx> bindings) override;
    void destroyDescriptorSetEx(handles::DescriptorSetHandle set) override;
    void uploadBufferEx(handles::BufferHandle handle, std::span<const std::byte> bytes,
                        std::size_t offset = 0) override;
    [[nodiscard]] std::vector<std::byte> readbackBufferEx(handles::BufferHandle handle, std::size_t offset,
                                                          std::size_t size) override;

  private:
    friend class VulkanCommandList;

    struct Frame {
        VkCommandPool commandPool{};
        VkCommandBuffer commandBuffer{};
        VkSemaphore imageAvailable{};
        VkSemaphore renderFinished{};
        VkFence inFlight{};
        VkQueryPool timestampQueryPool{};
        VkBuffer backgroundStagingBuffer{};
        VkDeviceMemory backgroundStagingMemory{};
        void* mappedBackgroundStaging{};
        bool backgroundUploadPending{};
        VkBuffer previewVertexBuffer{};
        VkDeviceMemory previewVertexMemory{};
        void* mappedPreviewVertices{};
        std::uint64_t previewVertexGeneration{};
        VkBuffer previewBoneBuffer{};
        VkDeviceMemory previewBoneMemory{};
        void* mappedPreviewBones{};
        VkDescriptorSet previewBoneDescriptor{};
        std::uint64_t previewBoneGeneration{};
        VkBuffer previewMorphDeltaBuffer{};
        VkDeviceMemory previewMorphDeltaMemory{};
        void* mappedPreviewMorphDeltas{};
        VkBuffer previewMorphWeightBuffer{};
        VkDeviceMemory previewMorphWeightMemory{};
        void* mappedPreviewMorphWeights{};
        VkDescriptorSet previewMorphDescriptor{};
        std::uint64_t previewMorphDeltaGeneration{};
        std::uint64_t previewMorphGeneration{};
        VkBuffer previewMaterialBuffer{};
        VkDeviceMemory previewMaterialMemory{};
        void* mappedPreviewMaterials{};
        VkDescriptorSet previewMaterialDescriptor{};
        std::uint64_t previewMaterialGeneration{};
        VkBuffer previewIndirectBuffer{};
        VkDeviceMemory previewIndirectMemory{};
        void* mappedPreviewIndirect{};
        std::uint64_t previewIndirectGeneration{};
    };

    struct DepthResource {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        bool initialized{};
    };

    struct PreviewTextureResource {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkDescriptorSet descriptor{};
    };

    struct OffscreenResource {
        VkImage colorImage{};
        VkDeviceMemory colorMemory{};
        VkImageView colorView{};
        DepthResource depth;
        VkBuffer stagingBuffer{};
        VkDeviceMemory stagingMemory{};
        VkDeviceSize stagingSize{};
        VkExtent2D extent{};
        bool colorInitialized{};
    };

    struct ViewportResource {
        VkImage colorImage{};
        VkDeviceMemory colorMemory{};
        VkImageView colorView{};
        DepthResource depth;
#if DAYO_HAS_IMGUI
        VkDescriptorSet imguiDescriptor{};
#endif
        VkExtent2D extent{};
        bool colorInitialized{};
    };

    void createInstance(bool validation);
    void createSurface();
    void selectPhysicalDevice();
    void createLogicalDevice();
    void createTypedDescriptorPool();
    void queryCapabilities();
    void createSwapchain();
    void destroySwapchain();
    void createPipelineCache();
    void savePipelineCache() const noexcept;
    void destroyPipelineCache();
    void createPipeline();
    void destroyPipeline();
    void createNativeOutputPipeline();
    void destroyNativeOutputPipeline() noexcept;
    void createPreviewDescriptors();
    void destroyPreviewDescriptors();
    void destroyPreviewTextures();
    void destroyPreviewBackground();
    void destroyPreviewTextureResource(PreviewTextureResource& texture);
    void destroyOffscreenResource();
    void createOffscreenResource(VkExtent2D extent);
    void destroyViewportResource(ViewportResource& resource);
    void destroyViewportResources();
    void createViewportResource(ViewportResource& resource, VkExtent2D extent);
    void createPreviewTexture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);
    [[nodiscard]] PreviewTextureResource createPreviewTextureResource(std::uint32_t width, std::uint32_t height,
                                                                      std::span<const std::uint8_t> rgba);
    [[nodiscard]] PreviewTextureResource createEmptyPreviewTextureResource(std::uint32_t width, std::uint32_t height);
    void createPreviewBackgroundStream(std::uint32_t width, std::uint32_t height);
    void recordPreviewBackgroundUpload(VkCommandBuffer command, Frame& frame);
    void createFrames();
    void destroyFrames();
    void resolveTimestampQuery(Frame& frame) noexcept;
    void uploadPreviewDeviceLocalBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                                        VkDeviceMemory& memory);
    void createUi();
    void destroyUi();
    void recreateSwapchain();
    void destroyPreviewMesh();
    void destroyTypedResources() noexcept;
    struct TypedTexture;
    void synchronizePreviewVertices(Frame& frame);
    void destroyPreviewBones();
    void synchronizePreviewBones(Frame& frame);
    void destroyPreviewMorphs();
    void synchronizePreviewMorphs(Frame& frame);
    void rebuildPreviewMorphBuffers();
    void destroyPreviewMaterialBuffers();
    void synchronizePreviewMaterials(Frame& frame);
    void destroyPreviewMaterialDescriptors();
    void refreshPreviewMaterialDescriptors(bool waitForGpu = true);
    void destroyPreviewIndirectBuffers();
    void synchronizePreviewIndirect(Frame& frame);
    void destroyPreviewBindlessDescriptor();
    void refreshPreviewBindlessDescriptor();
    [[nodiscard]] PreviewRenderPlan buildPreviewRenderPlan(bool includeUi) const noexcept;
    void recordPreviewModel(VkCommandBuffer command, const PreviewPushConstants& constants,
                            const PreviewRenderPlan& plan);
    void recordPreviewPass(VkCommandBuffer command, Frame& frame, VkImage colorImage, VkImageView colorView,
                           DepthResource& depth, VkExtent2D extent, bool colorInitialized,
                           VkImageLayout previousColorLayout, VkPipelineStageFlags2 previousColorStage,
                           VkAccessFlags2 previousColorAccess, bool preservePreviousFrame);
    void uploadPreviewBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                             VkDeviceMemory& memory, VkDeviceSize allocationSize = 0);
    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const;
    [[nodiscard]] VkDeviceAddress bufferDeviceAddress(VkBuffer buffer) const;
    void recordTraceRays(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline,
                         handles::ShaderBindingTableHandle sbt, std::uint32_t width, std::uint32_t height,
                         std::uint32_t depth);
    void recordNativeOutputToImage(VkCommandBuffer commandBuffer, const NativeFrameOutput& output, VkImage target,
                                   VkImageView targetView, VkImageLayout previousLayout,
                                   VkPipelineStageFlags2 previousStage,
                                   VkAccessFlags2 previousAccess, VkImageLayout finalLayout,
                                   VkPipelineStageFlags2 finalStage, VkAccessFlags2 finalAccess, bool initialized,
                                   VkExtent2D extent);
    void recordBindPipeline(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline);
    void recordTransitionTexture(VkCommandBuffer commandBuffer, handles::TextureHandle texture);
    void recordTextureTransition(VkCommandBuffer commandBuffer, handles::TextureHandle texture,
                                 VkImageLayout nextLayout);
    void recordCopyTexture(VkCommandBuffer commandBuffer, handles::TextureHandle source,
                           handles::TextureHandle destination);
    void recordClearTexture(VkCommandBuffer commandBuffer, handles::TextureHandle texture,
                            const std::array<float, 4>& value);
    void recordGenerateMipmaps(VkCommandBuffer commandBuffer, handles::TextureHandle texture);
    void recordBindDescriptorSet(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline,
                                 handles::DescriptorSetHandle set, std::uint32_t setIndex);
    void recordPushConstants(VkCommandBuffer commandBuffer, handles::PipelineHandle pipeline,
                             std::span<const std::byte> bytes);
    void recordMemoryBarrier(VkCommandBuffer commandBuffer);
    struct TypedAccelerationStructure {
        VkAccelerationStructureKHR structure{};
        VkBuffer storageBuffer{};
        VkDeviceMemory storageMemory{};
        VkDeviceSize storageSize{};
        VkBuffer instanceBuffer{};
        VkDeviceMemory instanceMemory{};
        void* mappedInstances{};
        VkDeviceSize instanceSize{};
        bool topLevel{};
        bool allowUpdate{};
    };
    [[nodiscard]] VkBuffer createAccelerationBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory& memory,
                                                    bool hostVisible, void** mapped);
    void destroyAccelerationBuffer(VkBuffer buffer, VkDeviceMemory memory, void* mapped) noexcept;
    struct BlasBuildInput {
        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        std::vector<std::uint32_t> primitiveCounts;
        bool allowUpdate{};
    };
    [[nodiscard]] BlasBuildInput makeBlasBuildInput(const BlasGeometryDesc& geometry) const;
    [[nodiscard]] std::vector<VkAccelerationStructureInstanceKHR>
    makeTlasInstances(std::span<const AccelerationInstanceDesc> instances) const;
    void recordAccelerationBuild(const TypedAccelerationStructure& destination, const BlasGeometryDesc& geometry,
                                 bool update);
    void recordTopLevelBuild(const TypedAccelerationStructure& destination,
                             std::span<const AccelerationInstanceDesc> instances, bool update);
    void submitImmediate(const std::function<void(VkCommandBuffer)>& record);
    [[nodiscard]] static VkImageLayout typedTextureFinalLayout(const TypedTexture& texture) noexcept;

    platform::Window& window_;
    DeviceCapabilities capabilities_;
    GraphicsConvention convention_;
    RendererKind activeRenderer_{RendererKind::preview};
    bool validation_{};
    bool swapchainDirty_{};
    VulkanAccelerationBackend accelerationBackend_;

    VkInstance instance_{};
    VkDebugUtilsMessengerEXT debugMessenger_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physicalDevice_{};
    VkPhysicalDeviceProperties physicalProperties_{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingPipelineProperties_{};
    VkDevice device_{};
#if DAYO_ENABLE_VMA
    VmaAllocator allocator_{};
#endif
    std::uint32_t queueFamily_{};
    std::uint32_t timestampValidBits_{};
    VkQueue queue_{};
    VkSemaphore timelineSemaphore_{};
    std::uint64_t nextTimelineValue_{};
    std::unique_ptr<VulkanUploadContext> uploadContext_;

    VkSwapchainKHR swapchain_{};
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<bool> swapchainInitialized_;
    std::vector<DepthResource> swapchainDepth_;

    VkPipelineLayout pipelineLayout_{};
    VkPipelineCache pipelineCache_{};
    VkPipeline pipeline_{};
    VkPipeline transparentPipeline_{};
    VkPipeline edgePipeline_{};
    VkPipeline backgroundPipeline_{};
    VkPipeline nativeOutputPipeline_{};
    VkPipelineLayout nativeOutputPipelineLayout_{};
    VkDescriptorSetLayout nativeOutputDescriptorSetLayout_{};
    VkDescriptorPool nativeOutputDescriptorPool_{};
    std::array<VkDescriptorSet, 2> nativeOutputDescriptors_{};
    VkDescriptorSetLayout previewDescriptorSetLayout_{};
    VkDescriptorSetLayout previewSkinningDescriptorSetLayout_{};
    VkDescriptorSetLayout previewMorphDescriptorSetLayout_{};
    VkDescriptorSetLayout previewMaterialDescriptorSetLayout_{};
    VkDescriptorSetLayout previewBindlessDescriptorSetLayout_{};
    VkDescriptorPool previewDescriptorPool_{};
    VkSampler previewSampler_{};
    VkSampler previewClampSampler_{};
    VkDescriptorSet previewBindlessDescriptor_{};
    std::uint32_t previewBindlessTextureCapacity_{};
    bool previewBindlessSupported_{};
#if DAYO_HAS_IMGUI
    VkDescriptorPool imguiDescriptorPool_{};
    bool uiInitialized_{};
#endif
    std::array<Frame, 2> frames_{};
    std::size_t frameIndex_{};
    std::uint64_t previewGpuNanoseconds_{};
    VkBuffer previewStaticVertexBuffer_{};
    VkDeviceMemory previewStaticVertexMemory_{};
    VkDeviceSize previewVertexSize_{};
    VkDeviceSize previewVertexCapacity_{};
    std::uint64_t previewVertexGeneration_{};
    VkDeviceSize previewBoneSize_{};
    VkDeviceSize previewBoneCapacity_{};
    std::uint64_t previewBoneGeneration_{};
    VkDeviceSize previewMorphDeltaSize_{};
    VkDeviceSize previewMorphDeltaCapacity_{};
    VkDeviceSize previewMorphWeightSize_{};
    VkDeviceSize previewMorphWeightCapacity_{};
    std::uint64_t previewMorphDeltaGeneration_{};
    std::uint64_t previewMorphGeneration_{};
    VkDeviceSize previewMaterialSize_{};
    VkDeviceSize previewMaterialCapacity_{};
    std::uint64_t previewMaterialGeneration_{};
    VkDeviceSize previewIndirectSize_{};
    VkDeviceSize previewIndirectCapacity_{};
    std::uint64_t previewIndirectGeneration_{};
    VkBuffer previewIndexBuffer_{};
    VkDeviceMemory previewIndexMemory_{};
    std::uint32_t previewIndexCount_{};
    std::uint64_t previewVertexUpdateCount_{};
    std::vector<PreviewTextureResource> previewTextures_;
    std::vector<VkDescriptorSet> previewMaterialDescriptors_;
    std::vector<std::array<std::uint32_t, 3>> previewMaterialDescriptorKeys_;
    std::vector<VkDrawIndexedIndirectCommand> previewIndirectCommands_;
    VkBuffer previewBackgroundVertexBuffer_{};
    VkDeviceMemory previewBackgroundVertexMemory_{};
    VkBuffer previewBackgroundIndexBuffer_{};
    VkDeviceMemory previewBackgroundIndexMemory_{};
    std::uint32_t previewBackgroundIndexCount_{};
    PreviewTextureResource previewBackgroundTexture_;
    VkExtent2D previewBackgroundExtent_{};
    VkDeviceSize previewBackgroundByteSize_{};
    bool previewBackgroundInitialized_{};
    PreviewGpuScene previewGpuScene_;
    OffscreenResource offscreen_;
    std::array<ViewportResource, 2> viewportResources_{};
    bool viewportRequested_{};
    VkExtent2D requestedViewportExtent_{};
    NativeFrameRecorder nativeFrameRecorder_;

    std::uint64_t nextResourceHandle_{1};
    std::unordered_map<BufferHandle, VulkanBuffer> buffers_;
    std::unordered_map<TextureHandle, VulkanImage> textures_;

    struct TypedBuffer {
        VulkanBuffer resource;
        BufferResourceDesc desc;
        void* mapped{};
    };
    struct TypedTexture {
        VulkanImage resource;
        TextureResourceDesc desc;
        VkImageView view{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };
    struct TypedSampler {
        VkSampler sampler{};
    };
    struct TypedDescriptorSetLayout {
        VkDescriptorSetLayout layout{};
        DescriptorSetLayoutDesc desc;
    };
    struct TypedDescriptorSet {
        VkDescriptorSet set{};
        handles::DescriptorSetLayoutHandle layout{};
    };
    struct TypedShader {
        VkShaderModule module{};
        ShaderStageMask stage{ShaderStageMask::compute};
        std::string entryPoint{"main"};
    };
    struct TypedPipelineLayout {
        VkPipelineLayout layout{};
        PipelineLayoutDesc desc;
    };
    struct TypedPipeline {
        VkPipeline pipeline{};
        handles::PipelineLayoutHandle layout{};
        std::uint32_t groupCount{};
        bool rayTracing{};
        VkPipelineBindPoint bindPoint{VK_PIPELINE_BIND_POINT_GRAPHICS};
    };
    struct TypedShaderBindingTable {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize size{};
        VkStridedDeviceAddressRegionKHR raygen{};
        VkStridedDeviceAddressRegionKHR miss{};
        VkStridedDeviceAddressRegionKHR hit{};
        VkStridedDeviceAddressRegionKHR callable{};
    };

    handles::BufferPool typedBufferHandles_;
    handles::TexturePool typedTextureHandles_;
    handles::SamplerPool typedSamplerHandles_;
    handles::ShaderPool typedShaderHandles_;
    handles::DescriptorSetLayoutPool typedDescriptorSetLayoutHandles_;
    handles::DescriptorSetPool typedDescriptorSetHandles_;
    handles::PipelineLayoutPool typedPipelineLayoutHandles_;
    handles::PipelinePool typedPipelineHandles_;
    handles::ShaderBindingTablePool typedShaderBindingTableHandles_;
    handles::AccelerationStructurePool typedAccelerationStructureHandles_;
    std::unordered_map<handles::BufferHandle, TypedBuffer> typedBuffers_;
    std::unordered_map<handles::TextureHandle, TypedTexture> typedTextures_;
    std::unordered_map<handles::SamplerHandle, TypedSampler> typedSamplers_;
    std::unordered_map<handles::ShaderHandle, TypedShader> typedShaders_;
    std::unordered_map<handles::DescriptorSetLayoutHandle, TypedDescriptorSetLayout> typedDescriptorSetLayouts_;
    std::unordered_map<handles::DescriptorSetHandle, TypedDescriptorSet> typedDescriptorSets_;
    std::unordered_map<handles::PipelineLayoutHandle, TypedPipelineLayout> typedPipelineLayouts_;
    std::unordered_map<handles::PipelineHandle, TypedPipeline> typedPipelines_;
    std::unordered_map<handles::ShaderBindingTableHandle, TypedShaderBindingTable> typedShaderBindingTables_;
    std::unordered_map<handles::AccelerationStructureHandle, TypedAccelerationStructure> typedAccelerationStructures_;
    VkDescriptorPool typedDescriptorPool_{};
};

} // namespace dayo::graphics
