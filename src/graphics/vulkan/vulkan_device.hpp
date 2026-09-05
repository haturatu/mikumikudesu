#pragma once

#include "graphics/device.hpp"

#include <vulkan/vulkan.h>

#if DAYO_ENABLE_VMA
#include <vk_mem_alloc.h>
#endif

#include <array>
#include <unordered_map>

namespace dayo::graphics {

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
    void selectRenderer(RendererKind requested) override;
    void resize() override;
    void beginUiFrame() override;
    void renderFrame() override;
    [[nodiscard]] std::uint64_t previewGpuNanoseconds() const noexcept override {
        return previewGpuNanoseconds_;
    }
    [[nodiscard]] core::ImageRgba8 renderToImage(const RenderTargetDesc& target) override;
    void waitIdle() override;
    void uploadPreviewMesh(std::span<const PreviewVertex> vertices, std::span<const std::uint32_t> indices) override;
    void updatePreviewVertices(std::span<const PreviewVertex> vertices) override;
    void updatePreviewBones(std::span<const PreviewBoneTransform> bones) override;
    void updatePreviewMaterials(std::span<const PreviewMaterial> materials) override;
    void updatePreviewDraws(std::span<const PreviewDraw> draws) override;
    void uploadPreviewTextures(std::span<const PreviewTexture> textures) override;
    void uploadPreviewBackground(std::span<const PreviewTexture> textures) override;
    void clearPreviewResources() override;
    void updatePreviewScene(const PreviewScene& scene) override {
        previewScene_ = scene;
    }
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc) override;
    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc) override;

  private:
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
        VkBuffer previewMaterialBuffer{};
        VkDeviceMemory previewMaterialMemory{};
        void* mappedPreviewMaterials{};
        VkDescriptorSet previewMaterialDescriptor{};
        std::uint64_t previewMaterialGeneration{};
    };

    struct BufferResource {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
#if DAYO_ENABLE_VMA
        VmaAllocation allocation{};
#endif
    };

    struct TextureResource {
        VkImage image{};
        VkDeviceMemory memory{};
#if DAYO_ENABLE_VMA
        VmaAllocation allocation{};
#endif
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

    void createInstance(bool validation);
    void createSurface();
    void selectPhysicalDevice();
    void createLogicalDevice();
    void queryCapabilities();
    void createSwapchain();
    void destroySwapchain();
    void createPipeline();
    void destroyPipeline();
    void createPreviewDescriptors();
    void destroyPreviewDescriptors();
    void destroyPreviewTextures();
    void destroyPreviewBackground();
    void destroyPreviewTextureResource(PreviewTextureResource& texture);
    void destroyOffscreenResource();
    void createOffscreenResource(VkExtent2D extent);
    void createPreviewTexture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);
    [[nodiscard]] PreviewTextureResource createPreviewTextureResource(std::uint32_t width, std::uint32_t height,
                                                                      std::span<const std::uint8_t> rgba);
    [[nodiscard]] PreviewTextureResource createEmptyPreviewTextureResource(std::uint32_t width, std::uint32_t height);
    void createPreviewBackgroundStream(std::uint32_t width, std::uint32_t height);
    void recordPreviewBackgroundUpload(VkCommandBuffer command, Frame& frame);
    void createFrames();
    void destroyFrames();
    void resolveTimestampQuery(Frame& frame) noexcept;
    void createUi();
    void destroyUi();
    void recreateSwapchain();
    void destroyPreviewMesh();
    void synchronizePreviewVertices(Frame& frame);
    void destroyPreviewBones();
    void synchronizePreviewBones(Frame& frame);
    void destroyPreviewMaterialBuffers();
    void synchronizePreviewMaterials(Frame& frame);
    void destroyPreviewMaterialDescriptors();
    void refreshPreviewMaterialDescriptors();
    void recordPreviewModel(VkCommandBuffer command, const PreviewPushConstants& constants);
    void uploadPreviewBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer,
                             VkDeviceMemory& memory);
    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    platform::Window& window_;
    DeviceCapabilities capabilities_;
    GraphicsConvention convention_;
    RendererKind activeRenderer_{RendererKind::preview};
    bool validation_{};
    bool swapchainDirty_{};

    VkInstance instance_{};
    VkDebugUtilsMessengerEXT debugMessenger_{};
    VkSurfaceKHR surface_{};
    VkPhysicalDevice physicalDevice_{};
    VkPhysicalDeviceProperties physicalProperties_{};
    VkDevice device_{};
#if DAYO_ENABLE_VMA
    VmaAllocator allocator_{};
#endif
    std::uint32_t queueFamily_{};
    std::uint32_t timestampValidBits_{};
    VkQueue queue_{};

    VkSwapchainKHR swapchain_{};
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<bool> swapchainInitialized_;
    std::vector<DepthResource> swapchainDepth_;

    VkPipelineLayout pipelineLayout_{};
    VkPipeline pipeline_{};
    VkPipeline transparentPipeline_{};
    VkPipeline edgePipeline_{};
    VkPipeline backgroundPipeline_{};
    VkDescriptorSetLayout previewDescriptorSetLayout_{};
    VkDescriptorSetLayout previewSkinningDescriptorSetLayout_{};
    VkDescriptorSetLayout previewMaterialDescriptorSetLayout_{};
    VkDescriptorPool previewDescriptorPool_{};
    VkSampler previewSampler_{};
    VkSampler previewClampSampler_{};
#if DAYO_HAS_IMGUI
    VkDescriptorPool imguiDescriptorPool_{};
    bool uiInitialized_{};
#endif
    std::array<Frame, 2> frames_{};
    std::size_t frameIndex_{};
    std::uint64_t previewGpuNanoseconds_{};
    VkDeviceSize previewVertexSize_{};
    std::uint64_t previewVertexGeneration_{};
    VkDeviceSize previewBoneSize_{};
    std::uint64_t previewBoneGeneration_{};
    VkDeviceSize previewMaterialSize_{};
    std::uint64_t previewMaterialGeneration_{};
    std::vector<PreviewMaterialGpu> previewMaterialData_;
    VkBuffer previewIndexBuffer_{};
    VkDeviceMemory previewIndexMemory_{};
    std::uint32_t previewIndexCount_{};
    std::uint64_t previewVertexUpdateCount_{};
    std::vector<PreviewMaterial> previewMaterials_;
    std::vector<PreviewDraw> previewDraws_;
    std::vector<PreviewTextureResource> previewTextures_;
    std::vector<VkDescriptorSet> previewMaterialDescriptors_;
    std::vector<std::array<std::uint32_t, 3>> previewMaterialDescriptorKeys_;
    VkBuffer previewBackgroundVertexBuffer_{};
    VkDeviceMemory previewBackgroundVertexMemory_{};
    VkBuffer previewBackgroundIndexBuffer_{};
    VkDeviceMemory previewBackgroundIndexMemory_{};
    std::uint32_t previewBackgroundIndexCount_{};
    PreviewTextureResource previewBackgroundTexture_;
    VkExtent2D previewBackgroundExtent_{};
    VkDeviceSize previewBackgroundByteSize_{};
    bool previewBackgroundInitialized_{};
    PreviewScene previewScene_;
    OffscreenResource offscreen_;

    std::uint64_t nextResourceHandle_{1};
    std::unordered_map<BufferHandle, BufferResource> buffers_;
    std::unordered_map<TextureHandle, TextureResource> textures_;
};

} // namespace dayo::graphics
