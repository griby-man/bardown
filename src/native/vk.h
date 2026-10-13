#pragma once

// the native renderer's Vulkan context, shared by its pieces. one device, the runtime's

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace bardown::native {

// loaded straight from the device, not the runtime's table, which only has what the emulation uses
#define BARDOWN_VK_DEVICE_FUNCTIONS(X) \
  X(vkAllocateCommandBuffers)          \
  X(vkAllocateDescriptorSets)          \
  X(vkAllocateMemory)                  \
  X(vkBeginCommandBuffer)              \
  X(vkBindBufferMemory)                \
  X(vkBindImageMemory)                 \
  X(vkCmdBeginRendering)               \
  X(vkCmdBindDescriptorSets)           \
  X(vkCmdBindIndexBuffer)              \
  X(vkCmdBindPipeline)                 \
  X(vkCmdBindVertexBuffers)            \
  X(vkCmdBlitImage)                    \
  X(vkCmdClearAttachments)             \
  X(vkCmdClearColorImage)              \
  X(vkCmdClearDepthStencilImage)       \
  X(vkCmdCopyBufferToImage)            \
  X(vkCmdCopyImageToBuffer)            \
  X(vkCmdCopyImage)                    \
  X(vkCmdDraw)                         \
  X(vkCmdDrawIndexed)                  \
  X(vkCmdEndRendering)                 \
  X(vkCmdPipelineBarrier)              \
  X(vkCmdPushConstants)                \
  X(vkCmdSetBlendConstants)            \
  X(vkCmdSetScissor)                   \
  X(vkCmdSetStencilCompareMask)        \
  X(vkCmdSetStencilReference)          \
  X(vkCmdSetStencilWriteMask)          \
  X(vkCmdSetViewport)                  \
  X(vkCreateBuffer)                    \
  X(vkCreateCommandPool)               \
  X(vkCreateDescriptorPool)            \
  X(vkCreateDescriptorSetLayout)       \
  X(vkCreateFence)                     \
  X(vkCreateGraphicsPipelines)         \
  X(vkCreateImage)                     \
  X(vkCreateImageView)                 \
  X(vkCreatePipelineLayout)            \
  X(vkCreateSampler)                   \
  X(vkCreateShaderModule)              \
  X(vkDestroyBuffer)                   \
  X(vkDestroyImage)                    \
  X(vkDestroyImageView)                \
  X(vkDestroyShaderModule)             \
  X(vkEndCommandBuffer)                \
  X(vkFreeMemory)                      \
  X(vkGetBufferDeviceAddress)          \
  X(vkGetBufferMemoryRequirements)     \
  X(vkGetImageMemoryRequirements)      \
  X(vkMapMemory)                       \
  X(vkQueueSubmit)                     \
  X(vkQueueWaitIdle)                   \
  X(vkResetCommandBuffer)              \
  X(vkResetFences)                     \
  X(vkUpdateDescriptorSets)            \
  X(vkWaitForFences)

struct Vk {
  const rex::ui::vulkan::VulkanDevice* rex = nullptr;
  rex::ui::Presenter* presenter = nullptr;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceMemoryProperties memory = {};
#define X(name) PFN_##name name = nullptr;
  BARDOWN_VK_DEVICE_FUNCTIONS(X)
#undef X
};
Vk& Api();

bool Check(VkResult result, const char* what);
// guest physical memory, as the GPU sees it (big-endian)
uint8_t* Physical(uint32_t address);
// guest memory of the renderer's own (physical heap), for what the game would otherwise get from the D3D ring
uint32_t GuestAlloc(uint32_t bytes);
uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want);

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0, levels = 1, layers = 1;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
enum class ImageKind { k2D, k3D, kCube };
bool MakeImage(Image& out, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
               uint32_t levels = 1, VkComponentMapping swizzle = {}, ImageKind kind = ImageKind::k2D);
// runs the frame recorded so far, waits for it and reads the image back (RGBA, 8 bits a channel); the frame
// carries on recording after it. for the few results the CPU reads back (load time only)
bool FlushAndRead(Image& image, std::vector<uint8_t>& out);
// another view of a colour image, through a different swizzle
VkImageView MakeView(const Image& image, VkComponentMapping swizzle);
void FreeImage(Image& image);
// frees it once no frame in flight can still be using it
void RetireImage(Image& image);
VkImageAspectFlags Aspect(VkFormat format);
// moves an image to a new layout; the stages and access come from the layouts
void Transition(VkCommandBuffer cb, Image& image, VkImageLayout to);

VkShaderModule MakeModule(const uint32_t* code, size_t bytes);

// per-frame upload space: host-visible, read by the GPU through buffer addresses or as vertex/index buffers
struct Upload {
  uint8_t* host = nullptr;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  VkDeviceAddress address = 0;
};
Upload Allocate(size_t bytes, size_t align = 16);

// this frame's command buffers: uploads run before the scene, in the same submit. the scene is the main
// thread's; uploads can come from any thread, holding FrameLock while they record
VkCommandBuffer UploadCommands();
VkCommandBuffer SceneCommands();
std::unique_lock<std::recursive_mutex> FrameLock();

// the bindless tables the translated shaders index into
enum class Heap { k2D, k3D, kCube, kSampler };
uint32_t HeapAdd(Heap heap, VkImageView view, VkSampler sampler = VK_NULL_HANDLE);
// gives the slot back once no frame in flight can still be reading it
void HeapFree(Heap heap, uint32_t index);
// counts up each frame; upload space handed out in one frame is only good for that frame's records
uint64_t FrameNumber();
// the depth-stencil format targets use: the 360's 24-bit one where the GPU has it
VkFormat DepthFormat();
// no emulated GPU underneath (the native graphics plugin)
bool NullGpu();
// the most upload space a frame has used since the last call, in bytes
uint64_t TakePeakUpload();
// time spent waiting for the GPU to free a frame slot, since the last call
void TakeGpuWait(double& total_ms, double& worst_ms);
VkPipelineLayout ShaderLayout();
void BindHeaps(VkCommandBuffer cb);

}  // namespace bardown::native
