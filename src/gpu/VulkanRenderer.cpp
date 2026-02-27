// Vulkan Hybrid Renderer Implementation
//
// This file implements the full Vulkan pipeline for GPU-accelerated
// pixel processing in an After Effects plugin. It demonstrates:
//
//   - Vulkan instance/device/queue creation
//   - Compute shader pipeline setup (8-bit, 16-bit, 32-bit float)
//   - CPU <-> GPU memory transfers via staging buffers
//   - ARGB <-> RGBA channel swizzle (AE uses ARGB, Vulkan uses RGBA)
//   - Thread-safe command pool management for MFR
//   - Proper resource cleanup
//
// The actual pixel work is done by GLSL compute shaders compiled to
// SPIR-V and embedded as byte arrays (gain_shader_spv.h).

#ifdef HAVE_VULKAN

#include "VulkanRenderer.h"
#include <cstring>
#include <algorithm>
#include <vector>

// Embedded SPIR-V shaders (compiled from shaders/gain.comp with different format defines)
#include "gain_shader_spv.h"

// Uniform buffer layout - must match the shader's layout(std140)
struct GainUniforms {
    int32_t  width;
    int32_t  height;
    float    gain;       // 0-100 range
    float    _padding;   // std140 alignment
};

// ===========================================
// Constructor / Destructor
// ===========================================

VulkanRenderer::VulkanRenderer()
    : m_instance(VK_NULL_HANDLE)
    , m_physicalDevice(VK_NULL_HANDLE)
    , m_device(VK_NULL_HANDLE)
    , m_queue(VK_NULL_HANDLE)
    , m_queueFamilyIndex(0)
    , m_commandPool(VK_NULL_HANDLE)
    , m_gainPipeline8(VK_NULL_HANDLE)
    , m_gainPipeline16(VK_NULL_HANDLE)
    , m_gainPipelineFloat(VK_NULL_HANDLE)
    , m_pipelineLayout(VK_NULL_HANDLE)
    , m_descriptorSetLayout(VK_NULL_HANDLE)
    , m_descriptorPool(VK_NULL_HANDLE)
    , m_gainShader8(VK_NULL_HANDLE)
    , m_gainShader16(VK_NULL_HANDLE)
    , m_gainShaderFloat(VK_NULL_HANDLE)
    , m_extendedFormatsSupported(false)
    , m_initialized(false)
{
}

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

// ===========================================
// Format Helpers
// ===========================================

VkFormat VulkanRenderer::GetVkFormat(AEPixelFormat fmt) const
{
    switch (fmt) {
        case AEPixelFormat::ARGB16:  return VK_FORMAT_R16G16B16A16_UNORM;
        case AEPixelFormat::ARGB32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default:                     return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

size_t VulkanRenderer::GetPixelSize(AEPixelFormat fmt) const
{
    switch (fmt) {
        case AEPixelFormat::ARGB16:  return 8;   // 4 channels x 2 bytes
        case AEPixelFormat::ARGB32F: return 16;  // 4 channels x 4 bytes
        default:                     return 4;   // 4 channels x 1 byte
    }
}

VkPipeline VulkanRenderer::GetPipeline(AEPixelFormat fmt) const
{
    switch (fmt) {
        case AEPixelFormat::ARGB16:
            return m_extendedFormatsSupported ? m_gainPipeline16 : m_gainPipeline8;
        case AEPixelFormat::ARGB32F:
            return m_extendedFormatsSupported ? m_gainPipelineFloat : m_gainPipeline8;
        default:
            return m_gainPipeline8;
    }
}

// ===========================================
// Initialization
// ===========================================

PF_Err VulkanRenderer::Initialize()
{
    PF_Err err = PF_Err_NONE;

    err = CreateVulkanInstance();
    if (err != PF_Err_NONE) return err;

    err = SelectPhysicalDevice();
    if (err != PF_Err_NONE) return err;

    err = CreateLogicalDevice();
    if (err != PF_Err_NONE) return err;

    err = CreateCommandPool();
    if (err != PF_Err_NONE) return err;

    err = LoadShaders();
    if (err != PF_Err_NONE) return err;

    err = CreateDescriptorSetLayout();
    if (err != PF_Err_NONE) return err;

    err = CreateComputePipelines();
    if (err != PF_Err_NONE) return err;

    err = CreateDescriptorPool();
    if (err != PF_Err_NONE) return err;

    m_initialized = true;
    return PF_Err_NONE;
}

PF_Err VulkanRenderer::Shutdown()
{
    if (!m_initialized) return PF_Err_NONE;

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }

    // Destroy pipelines
    if (m_gainPipeline8 != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_gainPipeline8, nullptr);
        m_gainPipeline8 = VK_NULL_HANDLE;
    }
    if (m_gainPipeline16 != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_gainPipeline16, nullptr);
        m_gainPipeline16 = VK_NULL_HANDLE;
    }
    if (m_gainPipelineFloat != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_gainPipelineFloat, nullptr);
        m_gainPipelineFloat = VK_NULL_HANDLE;
    }

    // Destroy pipeline layout
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    // Destroy descriptor pool
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }

    // Destroy descriptor set layout
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }

    // Destroy shader modules
    if (m_gainShader8 != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_gainShader8, nullptr);
        m_gainShader8 = VK_NULL_HANDLE;
    }
    if (m_gainShader16 != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_gainShader16, nullptr);
        m_gainShader16 = VK_NULL_HANDLE;
    }
    if (m_gainShaderFloat != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, m_gainShaderFloat, nullptr);
        m_gainShaderFloat = VK_NULL_HANDLE;
    }

    // Destroy per-thread command pools
    {
        std::lock_guard<std::mutex> lock(m_threadResourcesMutex);
        for (auto& pair : m_threadResources) {
            if (pair.second.commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(m_device, pair.second.commandPool, nullptr);
            }
        }
        m_threadResources.clear();
    }

    // Destroy main command pool
    if (m_commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }

    // Destroy device and instance
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    m_initialized = false;
    return PF_Err_NONE;
}

// ===========================================
// Vulkan Device Setup
// ===========================================

PF_Err VulkanRenderer::CreateVulkanInstance()
{
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "AE Skeleton Vulkan";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Vulkan Hybrid Renderer";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledLayerCount = 0;
    createInfo.enabledExtensionCount = 0;

    // TIP: Enable validation layers during development:
    // const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    // createInfo.enabledLayerCount = 1;
    // createInfo.ppEnabledLayerNames = layers;

    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    return PF_Err_NONE;
}

PF_Err VulkanRenderer::SelectPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    // Prefer discrete GPU
    for (const auto& device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_physicalDevice = device;
            return PF_Err_NONE;
        }
    }

    // Fallback to first device
    m_physicalDevice = devices[0];
    return PF_Err_NONE;
}

PF_Err VulkanRenderer::CreateLogicalDevice()
{
    // Find compute queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    m_queueFamilyIndex = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            m_queueFamilyIndex = i;
            break;
        }
    }

    if (m_queueFamilyIndex == UINT32_MAX) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = m_queueFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    // Query supported features for extended storage image formats (needed for rgba16, rgba32f)
    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supportedFeatures);

    VkPhysicalDeviceFeatures enabledFeatures = {};
    m_extendedFormatsSupported = false;
    if (supportedFeatures.shaderStorageImageExtendedFormats) {
        enabledFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
        m_extendedFormatsSupported = true;
    }

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.pEnabledFeatures = &enabledFeatures;
    createInfo.enabledExtensionCount = 0;
    createInfo.enabledLayerCount = 0;

    VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &m_queue);
    return PF_Err_NONE;
}

PF_Err VulkanRenderer::CreateCommandPool()
{
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult result = vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    return PF_Err_NONE;
}

VkCommandPool VulkanRenderer::GetThreadCommandPool()
{
    std::thread::id threadId = std::this_thread::get_id();

    // Fast path: existing pool
    {
        std::lock_guard<std::mutex> lock(m_threadResourcesMutex);
        auto it = m_threadResources.find(threadId);
        if (it != m_threadResources.end()) {
            return it->second.commandPool;
        }
    }

    // Slow path: create new pool for this thread
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_queueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool newPool = VK_NULL_HANDLE;
    vkCreateCommandPool(m_device, &poolInfo, nullptr, &newPool);

    {
        std::lock_guard<std::mutex> lock(m_threadResourcesMutex);
        m_threadResources[threadId].commandPool = newPool;
    }

    return newPool;
}

// ===========================================
// Pipeline Setup
// ===========================================

PF_Err VulkanRenderer::LoadShaders()
{
    // Load 8-bit shader (always available)
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = gain_shader_8_spv_len;
    createInfo.pCode = reinterpret_cast<const uint32_t*>(gain_shader_8_spv);

    VkResult result = vkCreateShaderModule(m_device, &createInfo, nullptr, &m_gainShader8);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    // Load 16-bit and 32-bit float shaders (need extended formats)
    if (m_extendedFormatsSupported) {
        createInfo.codeSize = gain_shader_16_spv_len;
        createInfo.pCode = reinterpret_cast<const uint32_t*>(gain_shader_16_spv);
        result = vkCreateShaderModule(m_device, &createInfo, nullptr, &m_gainShader16);
        if (result != VK_SUCCESS) {
            m_gainShader16 = VK_NULL_HANDLE; // Non-fatal, fall back to 8-bit
        }

        createInfo.codeSize = gain_shader_float_spv_len;
        createInfo.pCode = reinterpret_cast<const uint32_t*>(gain_shader_float_spv);
        result = vkCreateShaderModule(m_device, &createInfo, nullptr, &m_gainShaderFloat);
        if (result != VK_SUCCESS) {
            m_gainShaderFloat = VK_NULL_HANDLE; // Non-fatal, fall back to 8-bit
        }
    }

    return PF_Err_NONE;
}

PF_Err VulkanRenderer::CreateDescriptorSetLayout()
{
    // 3 bindings: input image, output image, uniform buffer
    VkDescriptorSetLayoutBinding bindings[3] = {};

    // Binding 0: Input image (storage image, read-only in shader)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: Output image (storage image, write-only in shader)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 2: Uniform buffer (gain parameters)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    VkResult result = vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    return PF_Err_NONE;
}

PF_Err VulkanRenderer::CreatePipelineForShader(VkShaderModule shader, VkPipeline& pipeline)
{
    VkComputePipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    return PF_Err_NONE;
}

PF_Err VulkanRenderer::CreateComputePipelines()
{
    // Create shared pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;

    VkResult result = vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    // Create 8-bit pipeline (always)
    PF_Err err = CreatePipelineForShader(m_gainShader8, m_gainPipeline8);
    if (err != PF_Err_NONE) return err;

    // Create 16-bit pipeline (if shader available)
    if (m_gainShader16 != VK_NULL_HANDLE) {
        CreatePipelineForShader(m_gainShader16, m_gainPipeline16);
        // Non-fatal if this fails
    }

    // Create 32-bit float pipeline (if shader available)
    if (m_gainShaderFloat != VK_NULL_HANDLE) {
        CreatePipelineForShader(m_gainShaderFloat, m_gainPipelineFloat);
        // Non-fatal if this fails
    }

    return PF_Err_NONE;
}

PF_Err VulkanRenderer::CreateDescriptorPool()
{
    VkDescriptorPoolSize poolSizes[2] = {};

    // Storage images (input + output)
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 10;

    // Uniform buffers
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 10;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = 10;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

    VkResult result = vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
    if (result != VK_SUCCESS) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    return PF_Err_NONE;
}

// ===========================================
// Memory Management
// ===========================================

PF_Err VulkanRenderer::AllocateHostVisibleBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    VkMemoryPropertyFlags memProps)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(m_device, &bufferInfo, nullptr, &buffer);
    if (result != VK_SUCCESS) return PF_Err_OUT_OF_MEMORY;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, memProps);

    result = vkAllocateMemory(m_device, &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        vkDestroyBuffer(m_device, buffer, nullptr);
        return PF_Err_OUT_OF_MEMORY;
    }

    vkBindBufferMemory(m_device, buffer, memory, 0);
    return PF_Err_NONE;
}

uint32_t VulkanRenderer::FindMemoryType(
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return 0;
}

// ===========================================
// Image Management
// ===========================================

PF_Err VulkanRenderer::CreateImage(
    uint32_t width, uint32_t height,
    VkFormat format, VkImageUsageFlags usage,
    VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;     // Best for GPU compute
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VkResult result = vkCreateImage(m_device, &imageInfo, nullptr, &image);
    if (result != VK_SUCCESS) return PF_Err_OUT_OF_MEMORY;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(
        memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);   // GPU-only memory (fastest)

    result = vkAllocateMemory(m_device, &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        vkDestroyImage(m_device, image, nullptr);
        return PF_Err_OUT_OF_MEMORY;
    }

    vkBindImageMemory(m_device, image, memory, 0);
    return PF_Err_NONE;
}

PF_Err VulkanRenderer::CreateImageView(
    VkImage image, VkFormat format,
    VkImageView& imageView)
{
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkResult result = vkCreateImageView(m_device, &viewInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    return PF_Err_NONE;
}

PF_Err VulkanRenderer::TransitionImageLayout(
    VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkCommandBuffer cmd;
    PF_Err err = BeginSingleTimeCommands(cmd);
    if (err != PF_Err_NONE) return err;

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_HOST_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    return EndSingleTimeCommands(cmd);
}

PF_Err VulkanRenderer::UploadToImage(
    const void* pixels, uint32_t width, uint32_t height,
    int rowbytes, VkImage image, VkFormat format,
    AEPixelFormat aeFormat)
{
    size_t pixelSize = GetPixelSize(aeFormat);
    VkDeviceSize imageSize = (VkDeviceSize)width * height * pixelSize;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    PF_Err err = AllocateHostVisibleBuffer(
        imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        stagingBuffer, stagingMemory);
    if (err != PF_Err_NONE) return err;

    // Copy pixel data to staging buffer with ARGB -> RGBA swizzle
    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);

    size_t dstRowBytes = width * pixelSize;
    const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(pixels);
    uint8_t* dstRow = reinterpret_cast<uint8_t*>(mapped);

    for (uint32_t y = 0; y < height; y++) {
        if (aeFormat == AEPixelFormat::ARGB8) {
            // 8-bit: AE ARGB [A,R,G,B] -> Vulkan RGBA [R,G,B,A]
            const uint8_t* src = srcRow;
            uint8_t* dst = dstRow;
            for (uint32_t x = 0; x < width; x++) {
                dst[0] = src[1];  // R <- AE red   (byte 1)
                dst[1] = src[2];  // G <- AE green (byte 2)
                dst[2] = src[3];  // B <- AE blue  (byte 3)
                dst[3] = src[0];  // A <- AE alpha (byte 0)
                src += 4;
                dst += 4;
            }
        } else if (aeFormat == AEPixelFormat::ARGB16) {
            // 16-bit: AE ARGB [A,R,G,B] -> Vulkan RGBA [R,G,B,A]
            // Also scale AE range (0-32768) to UNORM range (0-65535)
            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(srcRow);
            uint16_t* dst16 = reinterpret_cast<uint16_t*>(dstRow);
            for (uint32_t x = 0; x < width; x++) {
                dst16[0] = (uint16_t)((uint32_t)src16[1] * 65535 / 32768);  // R
                dst16[1] = (uint16_t)((uint32_t)src16[2] * 65535 / 32768);  // G
                dst16[2] = (uint16_t)((uint32_t)src16[3] * 65535 / 32768);  // B
                dst16[3] = (uint16_t)((uint32_t)src16[0] * 65535 / 32768);  // A
                src16 += 4;
                dst16 += 4;
            }
        } else {
            // 32-bit float: AE ARGB [A,R,G,B] -> Vulkan RGBA [R,G,B,A]
            const float* srcF = reinterpret_cast<const float*>(srcRow);
            float* dstF = reinterpret_cast<float*>(dstRow);
            for (uint32_t x = 0; x < width; x++) {
                dstF[0] = srcF[1];  // R <- AE red   (float 1)
                dstF[1] = srcF[2];  // G <- AE green (float 2)
                dstF[2] = srcF[3];  // B <- AE blue  (float 3)
                dstF[3] = srcF[0];  // A <- AE alpha (float 0)
                srcF += 4;
                dstF += 4;
            }
        }
        srcRow += rowbytes;
        dstRow += dstRowBytes;
    }

    vkUnmapMemory(m_device, stagingMemory);

    // Transition image to transfer destination
    err = TransitionImageLayout(image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (err != PF_Err_NONE) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return err;
    }

    // Copy staging buffer to image
    VkCommandBuffer cmd;
    err = BeginSingleTimeCommands(cmd);
    if (err != PF_Err_NONE) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return err;
    }

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    err = EndSingleTimeCommands(cmd);

    // Transition to GENERAL for compute shader access
    if (err == PF_Err_NONE) {
        err = TransitionImageLayout(image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    }

    // Cleanup staging
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    return err;
}

PF_Err VulkanRenderer::DownloadFromImage(
    VkImage image, uint32_t width, uint32_t height,
    int rowbytes, VkFormat format, AEPixelFormat aeFormat,
    void* pixels)
{
    size_t pixelSize = GetPixelSize(aeFormat);
    VkDeviceSize imageSize = (VkDeviceSize)width * height * pixelSize;

    // Create staging buffer with HOST_CACHED memory for fast CPU reads
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    PF_Err err = AllocateHostVisibleBuffer(
        imageSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        stagingBuffer, stagingMemory,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    if (err != PF_Err_NONE) {
        // Fallback to HOST_COHERENT if HOST_CACHED not available
        err = AllocateHostVisibleBuffer(
            imageSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            stagingBuffer, stagingMemory,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (err != PF_Err_NONE) return err;
    }

    // Transition output image to transfer source
    err = TransitionImageLayout(image,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (err != PF_Err_NONE) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return err;
    }

    // Copy image to staging buffer
    VkCommandBuffer cmd;
    err = BeginSingleTimeCommands(cmd);
    if (err != PF_Err_NONE) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return err;
    }

    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyImageToBuffer(cmd, image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuffer, 1, &region);

    err = EndSingleTimeCommands(cmd);
    if (err != PF_Err_NONE) {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return err;
    }

    // Invalidate cache before CPU read (required for HOST_CACHED memory)
    VkMappedMemoryRange memRange = {};
    memRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    memRange.memory = stagingMemory;
    memRange.offset = 0;
    memRange.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(m_device, 1, &memRange);

    // Map and copy to output with RGBA -> ARGB swizzle
    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, imageSize, 0, &mapped);

    size_t srcRowBytes = width * pixelSize;
    const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(mapped);
    uint8_t* dstRow = reinterpret_cast<uint8_t*>(pixels);

    for (uint32_t y = 0; y < height; y++) {
        if (aeFormat == AEPixelFormat::ARGB8) {
            // 8-bit: Vulkan RGBA [R,G,B,A] -> AE ARGB [A,R,G,B]
            const uint8_t* src = srcRow;
            uint8_t* dst = dstRow;
            for (uint32_t x = 0; x < width; x++) {
                dst[0] = src[3];  // A <- Vulkan alpha (byte 3)
                dst[1] = src[0];  // R <- Vulkan red   (byte 0)
                dst[2] = src[1];  // G <- Vulkan green (byte 1)
                dst[3] = src[2];  // B <- Vulkan blue  (byte 2)
                src += 4;
                dst += 4;
            }
        } else if (aeFormat == AEPixelFormat::ARGB16) {
            // 16-bit: Vulkan RGBA -> AE ARGB, scale UNORM (0-65535) to AE (0-32768)
            const uint16_t* src16 = reinterpret_cast<const uint16_t*>(srcRow);
            uint16_t* dst16 = reinterpret_cast<uint16_t*>(dstRow);
            for (uint32_t x = 0; x < width; x++) {
                dst16[0] = (uint16_t)((uint32_t)src16[3] * 32768 / 65535);  // A
                dst16[1] = (uint16_t)((uint32_t)src16[0] * 32768 / 65535);  // R
                dst16[2] = (uint16_t)((uint32_t)src16[1] * 32768 / 65535);  // G
                dst16[3] = (uint16_t)((uint32_t)src16[2] * 32768 / 65535);  // B
                src16 += 4;
                dst16 += 4;
            }
        } else {
            // 32-bit float: Vulkan RGBA [R,G,B,A] -> AE ARGB [A,R,G,B]
            const float* srcF = reinterpret_cast<const float*>(srcRow);
            float* dstF = reinterpret_cast<float*>(dstRow);
            for (uint32_t x = 0; x < width; x++) {
                dstF[0] = srcF[3];  // A <- Vulkan alpha (float 3)
                dstF[1] = srcF[0];  // R <- Vulkan red   (float 0)
                dstF[2] = srcF[1];  // G <- Vulkan green (float 1)
                dstF[3] = srcF[2];  // B <- Vulkan blue  (float 2)
                srcF += 4;
                dstF += 4;
            }
        }
        srcRow += srcRowBytes;
        dstRow += rowbytes;
    }

    vkUnmapMemory(m_device, stagingMemory);

    // Cleanup staging
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    return PF_Err_NONE;
}

// ===========================================
// Command Buffer Helpers
// ===========================================

PF_Err VulkanRenderer::BeginSingleTimeCommands(VkCommandBuffer& cmd)
{
    VkCommandPool pool = GetThreadCommandPool();
    if (pool == VK_NULL_HANDLE) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkResult result = vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);
    if (result != VK_SUCCESS) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);
    return PF_Err_NONE;
}

PF_Err VulkanRenderer::EndSingleTimeCommands(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    // Lock queue for submission (Vulkan spec requires this)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(m_queue);
    }

    // Free command buffer back to its thread's pool
    VkCommandPool pool = GetThreadCommandPool();
    vkFreeCommandBuffers(m_device, pool, 1, &cmd);

    return PF_Err_NONE;
}

// ===========================================
// Descriptor Set Helpers
// ===========================================

PF_Err VulkanRenderer::AllocateDescriptorSet(VkDescriptorSet& set)
{
    VkDescriptorSetAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;

    std::lock_guard<std::mutex> lock(m_descriptorPoolMutex);
    VkResult result = vkAllocateDescriptorSets(m_device, &allocInfo, &set);
    if (result != VK_SUCCESS) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    return PF_Err_NONE;
}

PF_Err VulkanRenderer::UpdateDescriptorSet(
    VkDescriptorSet set,
    VkImageView inputView,
    VkImageView outputView,
    VkBuffer uniformBuffer)
{
    VkDescriptorImageInfo inputImageInfo = {};
    inputImageInfo.imageView = inputView;
    inputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo outputImageInfo = {};
    outputImageInfo.imageView = outputView;
    outputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo uniformInfo = {};
    uniformInfo.buffer = uniformBuffer;
    uniformInfo.offset = 0;
    uniformInfo.range = sizeof(GainUniforms);

    VkWriteDescriptorSet writes[3] = {};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &inputImageInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputImageInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &uniformInfo;

    vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);

    return PF_Err_NONE;
}

// ===========================================
// RenderGain - the main GPU render method
// ===========================================

PF_Err VulkanRenderer::RenderGain(
    PF_EffectWorld* input,
    PF_EffectWorld* output,
    float gain,
    AEPixelFormat pixelFormat)
{
    if (!m_initialized) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    // For non-8-bit formats, fall back to 8-bit pipeline if extended formats
    // aren't supported (the caller will need to handle format conversion)
    if (pixelFormat != AEPixelFormat::ARGB8 && !m_extendedFormatsSupported) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED; // Signal caller to use CPU fallback
    }

    // Use extent_hint for the area to process (matches CPU iterate behavior)
    uint32_t width  = output->extent_hint.right - output->extent_hint.left;
    uint32_t height = output->extent_hint.bottom - output->extent_hint.top;

    if (width == 0 || height == 0) return PF_Err_NONE;

    int in_rowbytes = input->rowbytes;
    int out_rowbytes = output->rowbytes;

    VkFormat format = GetVkFormat(pixelFormat);
    size_t pixelSize = GetPixelSize(pixelFormat);
    VkPipeline pipeline = GetPipeline(pixelFormat);

    if (pipeline == VK_NULL_HANDLE) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED; // No pipeline for this format
    }

    // Compute data pointers offset by extent_hint (in case the buffer is larger)
    const char* inputBase = (const char*)input->data
        + input->extent_hint.top * in_rowbytes
        + input->extent_hint.left * (int)pixelSize;
    char* outputBase = (char*)output->data
        + output->extent_hint.top * out_rowbytes
        + output->extent_hint.left * (int)pixelSize;

    // 1. Create GPU images
    VkImage inputImage, outputImage;
    VkDeviceMemory inputMemory, outputMemory;
    VkImageView inputView, outputView;

    PF_Err err = CreateImage(width, height, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        inputImage, inputMemory);
    if (err != PF_Err_NONE) return err;

    err = CreateImage(width, height, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        outputImage, outputMemory);
    if (err != PF_Err_NONE) {
        vkDestroyImage(m_device, inputImage, nullptr);
        vkFreeMemory(m_device, inputMemory, nullptr);
        return err;
    }

    // 2. Create image views
    err = CreateImageView(inputImage, format, inputView);
    if (err != PF_Err_NONE) goto cleanup_images;

    err = CreateImageView(outputImage, format, outputView);
    if (err != PF_Err_NONE) {
        vkDestroyImageView(m_device, inputView, nullptr);
        goto cleanup_images;
    }

    {
        // 3. Upload input pixels to GPU (with ARGB->RGBA swizzle)
        err = UploadToImage(inputBase, width, height, in_rowbytes, inputImage, format, pixelFormat);
        if (err != PF_Err_NONE) goto cleanup_views;

        // Transition output image to GENERAL for compute shader
        err = TransitionImageLayout(outputImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        if (err != PF_Err_NONE) goto cleanup_views;

        // 4. Create and upload uniform buffer
        GainUniforms uniforms;
        uniforms.width = (int32_t)width;
        uniforms.height = (int32_t)height;
        uniforms.gain = gain;
        uniforms._padding = 0.0f;

        VkBuffer uniformBuffer;
        VkDeviceMemory uniformMemory;
        err = AllocateHostVisibleBuffer(
            sizeof(GainUniforms),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            uniformBuffer, uniformMemory);
        if (err != PF_Err_NONE) goto cleanup_views;

        void* mapped = nullptr;
        vkMapMemory(m_device, uniformMemory, 0, sizeof(GainUniforms), 0, &mapped);
        memcpy(mapped, &uniforms, sizeof(GainUniforms));
        vkUnmapMemory(m_device, uniformMemory);

        // 5. Allocate and update descriptor set
        VkDescriptorSet descriptorSet;
        err = AllocateDescriptorSet(descriptorSet);
        if (err != PF_Err_NONE) {
            vkDestroyBuffer(m_device, uniformBuffer, nullptr);
            vkFreeMemory(m_device, uniformMemory, nullptr);
            goto cleanup_views;
        }

        err = UpdateDescriptorSet(descriptorSet, inputView, outputView, uniformBuffer);

        // 6. Record and submit compute commands
        VkCommandBuffer cmd;
        err = BeginSingleTimeCommands(cmd);
        if (err == PF_Err_NONE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

            // Dispatch: 16x16 workgroup size, round up
            uint32_t groupCountX = (width + 15) / 16;
            uint32_t groupCountY = (height + 15) / 16;
            vkCmdDispatch(cmd, groupCountX, groupCountY, 1);

            err = EndSingleTimeCommands(cmd);
        }

        // 7. Download result back to CPU (with RGBA->ARGB swizzle)
        if (err == PF_Err_NONE) {
            err = DownloadFromImage(outputImage, width, height,
                out_rowbytes, format, pixelFormat, outputBase);
        }

        // Cleanup per-frame resources
        {
            std::lock_guard<std::mutex> lock(m_descriptorPoolMutex);
            vkFreeDescriptorSets(m_device, m_descriptorPool, 1, &descriptorSet);
        }
        vkDestroyBuffer(m_device, uniformBuffer, nullptr);
        vkFreeMemory(m_device, uniformMemory, nullptr);
    }

cleanup_views:
    vkDestroyImageView(m_device, outputView, nullptr);
    vkDestroyImageView(m_device, inputView, nullptr);

cleanup_images:
    vkDestroyImage(m_device, outputImage, nullptr);
    vkFreeMemory(m_device, outputMemory, nullptr);
    vkDestroyImage(m_device, inputImage, nullptr);
    vkFreeMemory(m_device, inputMemory, nullptr);

    return err;
}

#endif // HAVE_VULKAN
