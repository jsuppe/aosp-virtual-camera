/**
 * vulkan_renderer.cpp - Vulkan renderer implementation
 */
#include "vulkan_renderer.h"
#include "cube_geometry.h"
#include "shaders.h"
#include <android/log.h>
#include <chrono>
#include <cstring>

#define LOG_TAG "VulkanRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

VulkanRenderer::~VulkanRenderer() {
    shutdown();
}

bool VulkanRenderer::initialize() {
    if (!createInstance()) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createDevice()) return false;
    
    LOGI("Vulkan core initialized");
    return true;
}

void VulkanRenderer::shutdown() {
    stopRenderLoop();
    
    if (mDevice != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(mDevice);
        
        cleanupSwapchain();
        
        if (mDescriptorPool) vkDestroyDescriptorPool(mDevice, mDescriptorPool, nullptr);
        if (mDescriptorSetLayout) vkDestroyDescriptorSetLayout(mDevice, mDescriptorSetLayout, nullptr);
        if (mUniformBuffer) vkDestroyBuffer(mDevice, mUniformBuffer, nullptr);
        if (mUniformBufferMemory) vkFreeMemory(mDevice, mUniformBufferMemory, nullptr);
        if (mIndexBuffer) vkDestroyBuffer(mDevice, mIndexBuffer, nullptr);
        if (mIndexBufferMemory) vkFreeMemory(mDevice, mIndexBufferMemory, nullptr);
        if (mVertexBuffer) vkDestroyBuffer(mDevice, mVertexBuffer, nullptr);
        if (mVertexBufferMemory) vkFreeMemory(mDevice, mVertexBufferMemory, nullptr);
        
        for (auto& sem : mImageAvailableSemaphores) vkDestroySemaphore(mDevice, sem, nullptr);
        for (auto& sem : mRenderFinishedSemaphores) vkDestroySemaphore(mDevice, sem, nullptr);
        for (auto& fence : mInFlightFences) vkDestroyFence(mDevice, fence, nullptr);
        
        if (mCommandPool) vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
        
        vkDestroyDevice(mDevice, nullptr);
    }
    
    if (mSurface && mInstance) vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
    if (mInstance) vkDestroyInstance(mInstance, nullptr);
    
    if (mWindow) ANativeWindow_release(mWindow);
}

bool VulkanRenderer::createInstance() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VCamRenderer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;
    
    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;
    
    if (vkCreateInstance(&createInfo, nullptr, &mInstance) != VK_SUCCESS) {
        LOGE("Failed to create Vulkan instance");
        return false;
    }
    
    return true;
}

bool VulkanRenderer::pickPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(mInstance, &deviceCount, nullptr);
    
    if (deviceCount == 0) {
        LOGE("No Vulkan devices found");
        return false;
    }
    
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(mInstance, &deviceCount, devices.data());
    
    // Just pick the first device
    mPhysicalDevice = devices[0];
    
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
    LOGI("Using GPU: %s", props.deviceName);
    
    return true;
}

bool VulkanRenderer::createDevice() {
    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &queueFamilyCount, queueFamilies.data());
    
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            mGraphicsQueueFamily = i;
            break;
        }
    }
    
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = mGraphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    
    VkPhysicalDeviceFeatures deviceFeatures{};
    
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = extensions;
    createInfo.pEnabledFeatures = &deviceFeatures;
    
    if (vkCreateDevice(mPhysicalDevice, &createInfo, nullptr, &mDevice) != VK_SUCCESS) {
        LOGE("Failed to create logical device");
        return false;
    }
    
    vkGetDeviceQueue(mDevice, mGraphicsQueueFamily, 0, &mGraphicsQueue);
    return true;
}

bool VulkanRenderer::setSurface(ANativeWindow* window, int width, int height) {
    mWindow = window;
    mWidth = width;
    mHeight = height;
    
    ANativeWindow_setBuffersGeometry(window, width, height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    
    if (!createSurface()) return false;
    if (!createSwapchain()) return false;
    if (!createRenderPass()) return false;
    if (!createDepthResources()) return false;
    if (!createFramebuffers()) return false;
    if (!createDescriptorSetLayout()) return false;
    if (!createGraphicsPipeline()) return false;
    if (!createVertexBuffer()) return false;
    if (!createIndexBuffer()) return false;
    if (!createUniformBuffer()) return false;
    if (!createDescriptorPool()) return false;
    if (!createDescriptorSets()) return false;
    if (!createCommandPool()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    
    LOGI("Swapchain created: %dx%d", width, height);
    return true;
}

bool VulkanRenderer::createSurface() {
    VkAndroidSurfaceCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.window = mWindow;
    
    if (vkCreateAndroidSurfaceKHR(mInstance, &createInfo, nullptr, &mSurface) != VK_SUCCESS) {
        LOGE("Failed to create Android surface");
        return false;
    }
    return true;
}

bool VulkanRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, mSurface, &capabilities);
    
    mSwapchainFormat = VK_FORMAT_R8G8B8A8_UNORM;
    mSwapchainExtent = {static_cast<uint32_t>(mWidth), static_cast<uint32_t>(mHeight)};
    
    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }
    
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = mSurface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = mSwapchainFormat;
    createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    createInfo.imageExtent = mSwapchainExtent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // Vsync
    createInfo.clipped = VK_TRUE;
    
    if (vkCreateSwapchainKHR(mDevice, &createInfo, nullptr, &mSwapchain) != VK_SUCCESS) {
        LOGE("Failed to create swapchain");
        return false;
    }
    
    vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, nullptr);
    mSwapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(mDevice, mSwapchain, &imageCount, mSwapchainImages.data());
    
    mSwapchainImageViews.resize(imageCount);
    for (size_t i = 0; i < imageCount; i++) {
        mSwapchainImageViews[i] = createImageView(mSwapchainImages[i], mSwapchainFormat, 
                                                   VK_IMAGE_ASPECT_COLOR_BIT);
    }
    
    return true;
}

bool VulkanRenderer::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = mSwapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;
    
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | 
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | 
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | 
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    
    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
    
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    
    if (vkCreateRenderPass(mDevice, &renderPassInfo, nullptr, &mRenderPass) != VK_SUCCESS) {
        LOGE("Failed to create render pass");
        return false;
    }
    return true;
}

bool VulkanRenderer::createDepthResources() {
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    
    if (!createImage(mSwapchainExtent.width, mSwapchainExtent.height, depthFormat,
                     VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mDepthImage, mDepthMemory)) {
        return false;
    }
    
    mDepthImageView = createImageView(mDepthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    return mDepthImageView != VK_NULL_HANDLE;
}

bool VulkanRenderer::createFramebuffers() {
    mFramebuffers.resize(mSwapchainImageViews.size());
    
    for (size_t i = 0; i < mSwapchainImageViews.size(); i++) {
        std::array<VkImageView, 2> attachments = {
            mSwapchainImageViews[i],
            mDepthImageView
        };
        
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = mRenderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = mSwapchainExtent.width;
        framebufferInfo.height = mSwapchainExtent.height;
        framebufferInfo.layers = 1;
        
        if (vkCreateFramebuffer(mDevice, &framebufferInfo, nullptr, &mFramebuffers[i]) != VK_SUCCESS) {
            LOGE("Failed to create framebuffer");
            return false;
        }
    }
    return true;
}

bool VulkanRenderer::createVertexBuffer() {
    auto vertices = CubeGeometry::getVertices();
    VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();
    
    if (!createBuffer(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      mVertexBuffer, mVertexBufferMemory)) {
        return false;
    }
    
    void* data;
    vkMapMemory(mDevice, mVertexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices.data(), bufferSize);
    vkUnmapMemory(mDevice, mVertexBufferMemory);
    
    return true;
}

bool VulkanRenderer::createIndexBuffer() {
    auto indices = CubeGeometry::getIndices();
    mIndexCount = static_cast<uint32_t>(indices.size());
    VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();
    
    if (!createBuffer(bufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      mIndexBuffer, mIndexBufferMemory)) {
        return false;
    }
    
    void* data;
    vkMapMemory(mDevice, mIndexBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, indices.data(), bufferSize);
    vkUnmapMemory(mDevice, mIndexBufferMemory);
    
    return true;
}

bool VulkanRenderer::createUniformBuffer() {
    VkDeviceSize bufferSize = sizeof(UniformBufferObject);
    
    if (!createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      mUniformBuffer, mUniformBufferMemory)) {
        return false;
    }
    
    vkMapMemory(mDevice, mUniformBufferMemory, 0, bufferSize, 0, &mUniformBufferMapped);
    return true;
}

void VulkanRenderer::updateUniformBuffer() {
    mAutoRotation += 0.01f;
    
    UniformBufferObject ubo{};
    
    // Model matrix: rotate the cube
    ubo.model = glm::mat4(1.0f);
    ubo.model = glm::rotate(ubo.model, mAutoRotation + mRotationY.load(), glm::vec3(0.0f, 1.0f, 0.0f));
    ubo.model = glm::rotate(ubo.model, mRotationX.load(), glm::vec3(1.0f, 0.0f, 0.0f));
    
    // View matrix: camera position
    ubo.view = glm::lookAt(
        glm::vec3(2.5f, 2.5f, 2.5f),  // Camera position
        glm::vec3(0.0f, 0.0f, 0.0f),  // Look at origin
        glm::vec3(0.0f, 1.0f, 0.0f)   // Up vector
    );
    
    // Projection matrix
    float aspect = static_cast<float>(mSwapchainExtent.width) / static_cast<float>(mSwapchainExtent.height);
    ubo.proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
    ubo.proj[1][1] *= -1;  // Flip Y for Vulkan
    
    // Lighting
    ubo.lightPos = glm::vec3(3.0f, 3.0f, 3.0f);
    ubo.viewPos = glm::vec3(2.5f, 2.5f, 2.5f);
    
    memcpy(mUniformBufferMapped, &ubo, sizeof(ubo));
}

void VulkanRenderer::startRenderLoop() {
    if (mRendering.exchange(true)) return;
    
    mRenderThread = std::thread(&VulkanRenderer::renderLoop, this);
}

void VulkanRenderer::stopRenderLoop() {
    mRendering = false;
    if (mRenderThread.joinable()) {
        mRenderThread.join();
    }
}

void VulkanRenderer::setRotation(float angleX, float angleY) {
    mRotationX = angleX;
    mRotationY = angleY;
}

void VulkanRenderer::renderLoop() {
    while (mRendering) {
        renderFrame();
        // Target 60fps
        std::this_thread::sleep_for(std::chrono::microseconds(16667));
    }
}

void VulkanRenderer::renderFrame() {
    vkWaitForFences(mDevice, 1, &mInFlightFences[mCurrentFrame], VK_TRUE, UINT64_MAX);
    
    uint32_t imageIndex;
    VkResult result = vkAcquireNextImageKHR(mDevice, mSwapchain, UINT64_MAX,
                                             mImageAvailableSemaphores[mCurrentFrame],
                                             VK_NULL_HANDLE, &imageIndex);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    
    vkResetFences(mDevice, 1, &mInFlightFences[mCurrentFrame]);
    
    updateUniformBuffer();
    
    vkResetCommandBuffer(mCommandBuffers[mCurrentFrame], 0);
    recordCommandBuffer(mCommandBuffers[mCurrentFrame], imageIndex);
    
    VkSemaphore waitSemaphores[] = {mImageAvailableSemaphores[mCurrentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSemaphore signalSemaphores[] = {mRenderFinishedSemaphores[mCurrentFrame]};
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &mCommandBuffers[mCurrentFrame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;
    
    if (vkQueueSubmit(mGraphicsQueue, 1, &submitInfo, mInFlightFences[mCurrentFrame]) != VK_SUCCESS) {
        LOGE("Failed to submit draw command buffer");
    }
    
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &mSwapchain;
    presentInfo.pImageIndices = &imageIndex;
    
    vkQueuePresentKHR(mGraphicsQueue, &presentInfo);
    
    mCurrentFrame = (mCurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    
    vkBeginCommandBuffer(cmd, &beginInfo);
    
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.1f, 0.1f, 0.15f, 1.0f}};  // Dark blue background
    clearValues[1].depthStencil = {1.0f, 0};
    
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = mRenderPass;
    renderPassInfo.framebuffer = mFramebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = mSwapchainExtent;
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mGraphicsPipeline);
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(mSwapchainExtent.width);
    viewport.height = static_cast<float>(mSwapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = mSwapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    
    VkBuffer vertexBuffers[] = {mVertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, mIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 
                            0, 1, &mDescriptorSet, 0, nullptr);
    
    vkCmdDrawIndexed(cmd, mIndexCount, 1, 0, 0, 0);
    
    vkCmdEndRenderPass(cmd);
    
    vkEndCommandBuffer(cmd);
}

// Helper implementations...
uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

bool VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags properties,
                                   VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateBuffer(mDevice, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(mDevice, buffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);
    
    if (vkAllocateMemory(mDevice, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    
    vkBindBufferMemory(mDevice, buffer, memory, 0);
    return true;
}

bool VulkanRenderer::createImage(uint32_t width, uint32_t height, VkFormat format,
                                  VkImageTiling tiling, VkImageUsageFlags usage,
                                  VkMemoryPropertyFlags properties,
                                  VkImage& image, VkDeviceMemory& memory) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (vkCreateImage(mDevice, &imageInfo, nullptr, &image) != VK_SUCCESS) {
        return false;
    }
    
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(mDevice, image, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);
    
    if (vkAllocateMemory(mDevice, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }
    
    vkBindImageMemory(mDevice, image, memory, 0);
    return true;
}

VkImageView VulkanRenderer::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    
    VkImageView imageView;
    if (vkCreateImageView(mDevice, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return imageView;
}

// Continued in next files...
