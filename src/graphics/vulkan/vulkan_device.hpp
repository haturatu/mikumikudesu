#pragma once

#include "graphics/device.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <unordered_map>

namespace dayo::graphics {

class VulkanDevice final : public Device {
public:
    VulkanDevice(platform::Window& window, bool validation);
    ~VulkanDevice() override;

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override { return capabilities_; }
    [[nodiscard]] const GraphicsConvention& convention() const noexcept override { return convention_; }
    [[nodiscard]] RendererKind activeRenderer() const noexcept override { return activeRenderer_; }
    void selectRenderer(RendererKind requested) override;
    void resize() override;
    void beginUiFrame() override;
    void renderFrame() override;
    void waitIdle() override;
    void uploadPreviewMesh(std::span<const PreviewVertex> vertices,
                           std::span<const std::uint32_t> indices) override;
    void updatePreviewVertices(std::span<const PreviewVertex> vertices) override;
    void updatePreviewMaterials(std::span<const PreviewMaterial> materials) override;
    void uploadPreviewTextures(std::span<const PreviewTexture> textures) override;
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc) override;
    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc) override;

private:
    struct Frame {
        VkCommandPool commandPool {};
        VkCommandBuffer commandBuffer {};
        VkSemaphore imageAvailable {};
        VkSemaphore renderFinished {};
        VkFence inFlight {};
    };

    struct BufferResource {
        VkBuffer buffer {};
        VkDeviceMemory memory {};
    };

    struct TextureResource {
        VkImage image {};
        VkDeviceMemory memory {};
    };

    struct DepthResource {
        VkImage image {};
        VkDeviceMemory memory {};
        VkImageView view {};
        bool initialized {};
    };

    struct PreviewTextureResource {
        VkImage image {};
        VkDeviceMemory memory {};
        VkImageView view {};
        VkDescriptorSet descriptor {};
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
    void createPreviewTexture(std::uint32_t width, std::uint32_t height,
                              std::span<const std::uint8_t> rgba);
    void createFrames();
    void destroyFrames();
    void createUi();
    void destroyUi();
    void recreateSwapchain();
    void destroyPreviewMesh();
    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags flags) const;

    platform::Window& window_;
    DeviceCapabilities capabilities_;
    GraphicsConvention convention_;
    RendererKind activeRenderer_ { RendererKind::preview };
    bool validation_ {};
    bool swapchainDirty_ {};

    VkInstance instance_ {};
    VkDebugUtilsMessengerEXT debugMessenger_ {};
    VkSurfaceKHR surface_ {};
    VkPhysicalDevice physicalDevice_ {};
    VkPhysicalDeviceProperties physicalProperties_ {};
    VkDevice device_ {};
    std::uint32_t queueFamily_ {};
    VkQueue queue_ {};

    VkSwapchainKHR swapchain_ {};
    VkFormat swapchainFormat_ { VK_FORMAT_UNDEFINED };
    VkExtent2D swapchainExtent_ {};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<bool> swapchainInitialized_;
    std::vector<DepthResource> swapchainDepth_;

    VkPipelineLayout pipelineLayout_ {};
    VkPipeline pipeline_ {};
    VkDescriptorSetLayout previewDescriptorSetLayout_ {};
    VkDescriptorPool previewDescriptorPool_ {};
    VkSampler previewSampler_ {};
    VkDescriptorPool imguiDescriptorPool_ {};
    bool uiInitialized_ {};
    std::array<Frame, 2> frames_ {};
    std::size_t frameIndex_ {};
    VkBuffer previewVertexBuffer_ {};
    VkDeviceMemory previewVertexMemory_ {};
    VkDeviceSize previewVertexSize_ {};
    VkBuffer previewIndexBuffer_ {};
    VkDeviceMemory previewIndexMemory_ {};
    std::uint32_t previewIndexCount_ {};
    std::vector<PreviewMaterial> previewMaterials_;
    std::vector<PreviewTextureResource> previewTextures_;

    std::uint64_t nextResourceHandle_ { 1 };
    std::unordered_map<BufferHandle, BufferResource> buffers_;
    std::unordered_map<TextureHandle, TextureResource> textures_;
};

} // namespace dayo::graphics
