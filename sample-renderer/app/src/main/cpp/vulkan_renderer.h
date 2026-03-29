/**
 * vulkan_renderer.h - Vulkan renderer for golden cube
 */
#pragma once

#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <atomic>
#include <thread>
#include <vector>
#include <array>

// GLM for matrix math
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Vertex is defined in cube_geometry.h

struct UniformBufferObject {
    alignas(16) glm::mat4 mvp;  // Combined model-view-projection matrix
};

class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();
    
    bool initialize();
    void shutdown();
    
    bool setSurface(ANativeWindow* window, int width, int height);
    void startRenderLoop();
    void stopRenderLoop();
    
    void setRotation(float angleX, float angleY);
    
private:
    // Vulkan objects
    VkInstance mInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mGraphicsQueue = VK_NULL_HANDLE;
    uint32_t mGraphicsQueueFamily = 0;
    
    // Surface and swapchain
    VkSurfaceKHR mSurface = VK_NULL_HANDLE;
    VkSwapchainKHR mSwapchain = VK_NULL_HANDLE;
    std::vector<VkImage> mSwapchainImages;
    std::vector<VkImageView> mSwapchainImageViews;
    VkFormat mSwapchainFormat;
    VkExtent2D mSwapchainExtent;
    
    // Render pass and framebuffers
    VkRenderPass mRenderPass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> mFramebuffers;
    
    // Depth buffer
    VkImage mDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory mDepthMemory = VK_NULL_HANDLE;
    VkImageView mDepthImageView = VK_NULL_HANDLE;
    
    // Pipeline
    VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
    VkPipeline mGraphicsPipeline = VK_NULL_HANDLE;
    
    // Buffers
    VkBuffer mVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory mVertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer mIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory mIndexBufferMemory = VK_NULL_HANDLE;
    VkBuffer mUniformBuffer = VK_NULL_HANDLE;
    VkDeviceMemory mUniformBufferMemory = VK_NULL_HANDLE;
    void* mUniformBufferMapped = nullptr;
    
    // Descriptors
    VkDescriptorSetLayout mDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;
    
    // Command buffers
    VkCommandPool mCommandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> mCommandBuffers;
    
    // Synchronization
    std::vector<VkSemaphore> mImageAvailableSemaphores;
    std::vector<VkSemaphore> mRenderFinishedSemaphores;
    std::vector<VkFence> mInFlightFences;
    uint32_t mCurrentFrame = 0;
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    
    // Native window
    ANativeWindow* mWindow = nullptr;
    int mWidth = 0;
    int mHeight = 0;
    
    // Render thread
    std::thread mRenderThread;
    std::atomic<bool> mRendering{false};
    
    // Rotation
    std::atomic<float> mRotationX{0.0f};
    std::atomic<float> mRotationY{0.0f};
    float mAutoRotation = 0.0f;
    
    // Cube data
    uint32_t mIndexCount = 0;
    
    // Helper methods
    bool createInstance();
    bool pickPhysicalDevice();
    bool createDevice();
    bool createSurface();
    bool createSwapchain();
    bool createRenderPass();
    bool createDepthResources();
    bool createFramebuffers();
    bool createDescriptorSetLayout();
    bool createGraphicsPipeline();
    bool createVertexBuffer();
    bool createIndexBuffer();
    bool createUniformBuffer();
    bool createDescriptorPool();
    bool createDescriptorSets();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    
    void cleanupSwapchain();
    void recreateSwapchain();
    
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void updateUniformBuffer();
    void renderFrame();
    void renderLoop();
    
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& memory);
    bool createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& memory);
    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
    VkShaderModule createShaderModule(const uint32_t* code, size_t size);
};
