#pragma once

// Vulkan Hybrid Renderer for After Effects
//
// "Hybrid" means: CPU checkout from AE -> upload to GPU -> compute -> download back to CPU
// This bypasses AE's native GPU API entirely, giving you full Vulkan control.

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include "AE_Effect.h"

class VulkanRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer();

    // Lifecycle
    PF_Err Initialize();
    PF_Err Shutdown();
    bool IsAvailable() const { return m_initialized; }

    // Render a simple gain (brightness) effect on GPU
    // Takes CPU memory from AE's PF_EffectWorld, processes on GPU, writes back
    PF_Err RenderGain(
        PF_EffectWorld* input,      // CPU input pixels (from AE checkout)
        PF_EffectWorld* output,     // CPU output pixels (AE will read this)
        float gain);                // Gain amount (0-100)

private:
    // Vulkan core objects
    VkInstance          m_instance;
    VkPhysicalDevice    m_physicalDevice;
    VkDevice            m_device;
    VkQueue             m_queue;
    uint32_t            m_queueFamilyIndex;
    VkCommandPool       m_commandPool;

    // Compute pipeline
    VkPipeline          m_gainPipeline;
    VkPipelineLayout    m_pipelineLayout;
    VkDescriptorSetLayout m_descriptorSetLayout;
    VkDescriptorPool    m_descriptorPool;
    VkShaderModule      m_gainShader;

    // State
    std::atomic<bool>   m_initialized;

    // Thread safety for MFR (Multi-Frame Rendering)
    struct PerThreadResources {
        VkCommandPool commandPool;
    };
    std::unordered_map<std::thread::id, PerThreadResources> m_threadResources;
    std::mutex m_threadResourcesMutex;
    std::mutex m_queueMutex;
    std::mutex m_descriptorPoolMutex;

    // --- Setup helpers ---
    PF_Err CreateVulkanInstance();
    PF_Err SelectPhysicalDevice();
    PF_Err CreateLogicalDevice();
    PF_Err CreateCommandPool();
    PF_Err LoadShaders();
    PF_Err CreateDescriptorSetLayout();
    PF_Err CreateComputePipeline();
    PF_Err CreateDescriptorPool();

    // --- Memory helpers ---
    PF_Err AllocateHostVisibleBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkBuffer& buffer,
        VkDeviceMemory& memory,
        VkMemoryPropertyFlags memProps =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    // --- Image helpers ---
    PF_Err CreateImage(
        uint32_t width, uint32_t height,
        VkFormat format, VkImageUsageFlags usage,
        VkImage& image, VkDeviceMemory& memory);

    PF_Err CreateImageView(
        VkImage image, VkFormat format,
        VkImageView& imageView);

    PF_Err TransitionImageLayout(
        VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);

    PF_Err UploadToImage(
        const void* pixels, uint32_t width, uint32_t height,
        int rowbytes, VkImage image, VkFormat format);

    PF_Err DownloadFromImage(
        VkImage image, uint32_t width, uint32_t height,
        int rowbytes, VkFormat format, void* pixels);

    // --- Command buffer helpers ---
    VkCommandPool GetThreadCommandPool();
    PF_Err BeginSingleTimeCommands(VkCommandBuffer& cmd);
    PF_Err EndSingleTimeCommands(VkCommandBuffer cmd);

    // --- Descriptor helpers ---
    PF_Err AllocateDescriptorSet(VkDescriptorSet& set);
    PF_Err UpdateDescriptorSet(
        VkDescriptorSet set,
        VkImageView inputView,
        VkImageView outputView,
        VkBuffer uniformBuffer);
};

#endif // HAVE_VULKAN
