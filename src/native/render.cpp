#include "render.h"

#include <algorithm>
#include <array>
#include <string>
#include <filesystem>
#include <cstdio>
#include <cinttypes>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <SDL3/SDL_messagebox.h>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/external_output.h>
#include <rex/ui/renderdoc_api.h>
// the full RenderDoc header: on macOS the runtime's only declares the struct
#include <renderdoc_app.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#include "shaders/present_frag.h"
#include "shaders/present_vert.h"
#include "streams.h"
#include "textures.h"
#include "vk.h"

namespace bardown::native {

namespace {

using rex::ui::vulkan::VulkanPresenter;

// three, because a worker's segment can be recorded in one frame and spliced into the next: its data has to
// outlive both
constexpr uint32_t kFrames = 3;
constexpr VkDeviceSize kRingPerFrame = VkDeviceSize(128) << 20;

// how many descriptors each bindless table holds
constexpr uint32_t kHeapSizes[4] = {16384, 1024, 1024, 2048};

struct Frame {
  VkCommandBuffer upload = VK_NULL_HANDLE, scene = VK_NULL_HANDLE, present = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkDescriptorSet present_set = VK_NULL_HANDLE;
  VkDeviceSize ring_used = 0;
};

struct Renderer {
  Vk vk;
  std::recursive_mutex mutex;  // the frame, the upload space and the tables, for the game's worker threads
  uint64_t frames_begun = 0;
  VkDeviceSize peak_upload = 0;
  double gpu_wait_ms = 0, gpu_wait_worst = 0;
  rex::memory::Memory* memory = nullptr;
  std::vector<std::pair<uint64_t, Image>> retired;  // and the frame they were dropped in
  uint8_t* physical = nullptr;
  uint32_t scale = 1;
  VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
  VkCommandPool pool = VK_NULL_HANDLE;
  std::array<Frame, kFrames> frames;
  uint32_t frame = 0;
  bool recording = false;

  VkBuffer ring = VK_NULL_HANDLE;
  VkDeviceMemory ring_memory = VK_NULL_HANDLE;
  uint8_t* ring_host = nullptr;
  VkDeviceAddress ring_address = 0;

  VkDescriptorSetLayout heap_layouts[4] = {};
  VkDescriptorSet heaps[4] = {};
  uint32_t heap_used[4] = {};
  std::vector<uint32_t> heap_free[4];
  struct FreedSlot {
    uint64_t frame;
    int heap;
    uint32_t index;
  };
  std::vector<FreedSlot> heap_freed;  // waiting out the frames in flight
  VkPipelineLayout shader_layout = VK_NULL_HANDLE;
  Image dummy2d, dummy3d, dummy_cube;
  VkSampler dummy_sampler = VK_NULL_HANDLE;

  VkSampler linear = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout present_layout = VK_NULL_HANDLE;
  VkPipeline present = VK_NULL_HANDLE;
  bool ready = false;
  // BARDOWN_NATIVE_RDOC=<frame>: under RenderDoc, captures that frame from its first command to its submit
  uint64_t presents = 0, rdoc_frame = [] {
    const char* v = std::getenv("BARDOWN_NATIVE_RDOC");
    return v ? std::strtoull(v, nullptr, 10) : 0;
  }();
};
Renderer r;

const RENDERDOC_API_1_0_0* RenderDoc() {
  const auto* api = r.vk.rex->vulkan_instance()->renderdoc_api();
  return api ? api->api_1_0_0() : nullptr;
}

void BeginFrame() {
  std::lock_guard lock(r.mutex);
  if (r.recording) return;
  auto& vk = r.vk;
  Frame& f = r.frames[r.frame];
  // this slot's last frame, and the one after it, which may have spliced data from it
  const VkFence fences[2] = {f.fence, r.frames[(r.frame + 1) % kFrames].fence};
  const auto start = std::chrono::steady_clock::now();
  vk.vkWaitForFences(vk.device, 2, fences, VK_TRUE, UINT64_MAX);
  {
    const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard stats(r.mutex);
    r.gpu_wait_ms += waited;
    r.gpu_wait_worst = std::max(r.gpu_wait_worst, waited);
  }
  vk.vkResetFences(vk.device, 1, &f.fence);
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  for (VkCommandBuffer cb : {f.upload, f.scene}) {
    vk.vkResetCommandBuffer(cb, 0);
    vk.vkBeginCommandBuffer(cb, &begin);
  }
  f.ring_used = 0;
  r.recording = true;
  // this frame's fence is in, and so is every frame before it
  ++r.frames_begun;
  while (!r.retired.empty() && r.retired.front().first + kFrames < r.frames_begun) {
    FreeImage(r.retired.front().second);
    r.retired.erase(r.retired.begin());
  }
  while (!r.heap_freed.empty() && r.heap_freed.front().frame + kFrames < r.frames_begun) {
    r.heap_free[r.heap_freed.front().heap].push_back(r.heap_freed.front().index);
    r.heap_freed.erase(r.heap_freed.begin());
  }
}

bool MakeRing() {
  auto& vk = r.vk;
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = kRingPerFrame * kFrames;
  info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!Check(vk.vkCreateBuffer(vk.device, &info, nullptr, &r.ring), "ring buffer")) return false;
  VkMemoryRequirements req;
  vk.vkGetBufferMemoryRequirements(vk.device, r.ring, &req);
  VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &flags;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!Check(vk.vkAllocateMemory(vk.device, &alloc, nullptr, &r.ring_memory), "ring memory") ||
      !Check(vk.vkBindBufferMemory(vk.device, r.ring, r.ring_memory, 0), "ring bind") ||
      !Check(vk.vkMapMemory(vk.device, r.ring_memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&r.ring_host)),
             "ring map"))
    return false;
  VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = r.ring;
  r.ring_address = vk.vkGetBufferDeviceAddress(vk.device, &address);
  return r.ring_address != 0;
}

// set 0-3 = Texture2D[], Texture3D[], TextureCube[], SamplerState[], as the translated shaders declare them
bool MakeHeaps() {
  auto& vk = r.vk;
  const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                     VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kHeapSizes[0] + kHeapSizes[1] + kHeapSizes[2]},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER, kHeapSizes[3]}};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pool_info.maxSets = 4;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = sizes;
  VkDescriptorPool pool;
  if (!Check(vk.vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &pool), "heap pool")) return false;
  for (int i = 0; i < 4; ++i) {
    VkDescriptorSetLayoutBinding binding = {0, types[i], kHeapSizes[i],
                                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorBindingFlags binding_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                             VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                             VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 1;
    flags.pBindingFlags = &binding_flags;
    VkDescriptorSetLayoutCreateInfo info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.pNext = &flags;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (!Check(vk.vkCreateDescriptorSetLayout(vk.device, &info, nullptr, &r.heap_layouts[i]), "heap layout"))
      return false;
    VkDescriptorSetVariableDescriptorCountAllocateInfo count = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    count.descriptorSetCount = 1;
    count.pDescriptorCounts = &kHeapSizes[i];
    VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.pNext = &count;
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &r.heap_layouts[i];
    if (!Check(vk.vkAllocateDescriptorSets(vk.device, &alloc, &r.heaps[i]), "heap set")) return false;
  }
  VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
  VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 4;
  layout.pSetLayouts = r.heap_layouts;
  layout.pushConstantRangeCount = 1;
  layout.pPushConstantRanges = &push;
  if (!Check(vk.vkCreatePipelineLayout(vk.device, &layout, nullptr, &r.shader_layout), "shader layout")) return false;

  // index 0 of each table is a stand-in, so an unbound slot reads something harmless
  VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.maxLod = VK_LOD_CLAMP_NONE;
  if (!Check(vk.vkCreateSampler(vk.device, &sampler, nullptr, &r.dummy_sampler), "sampler")) return false;
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!MakeImage(r.dummy2d, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, usage) ||
      !MakeImage(r.dummy3d, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, usage, 1, {}, ImageKind::k3D) ||
      !MakeImage(r.dummy_cube, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, usage, 1, {}, ImageKind::kCube))
    return false;
  HeapAdd(Heap::k2D, r.dummy2d.view);
  HeapAdd(Heap::k3D, r.dummy3d.view);
  HeapAdd(Heap::kCube, r.dummy_cube.view);
  HeapAdd(Heap::kSampler, VK_NULL_HANDLE, r.dummy_sampler);
  BeginFrame();
  VkCommandBuffer cb = r.frames[r.frame].upload;
  const VkClearColorValue white = {{1, 1, 1, 1}};
  for (Image* image : {&r.dummy2d, &r.dummy3d, &r.dummy_cube}) {
    Transition(cb, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image->layers};
    vk.vkCmdClearColorImage(cb, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    Transition(cb, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  return true;
}

// samples the finished picture into the presenter's output image
bool MakePresent() {
  auto& vk = r.vk;
  VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (!Check(vk.vkCreateSampler(vk.device, &sampler, nullptr, &r.linear), "vkCreateSampler")) return false;

  VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, &r.linear};
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (!Check(vk.vkCreateDescriptorSetLayout(vk.device, &set_info, nullptr, &r.present_set_layout), "set layout"))
    return false;
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFrames};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kFrames;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &size;
  VkDescriptorPool pool;
  if (!Check(vk.vkCreateDescriptorPool(vk.device, &pool_info, nullptr, &pool), "descriptor pool")) return false;
  for (auto& f : r.frames) {
    VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &r.present_set_layout;
    if (!Check(vk.vkAllocateDescriptorSets(vk.device, &alloc, &f.present_set), "descriptor set")) return false;
  }

  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &r.present_set_layout;
  if (!Check(vk.vkCreatePipelineLayout(vk.device, &layout_info, nullptr, &r.present_layout), "pipeline layout"))
    return false;

  VkShaderModule vs = MakeModule(k_present_vert, sizeof(k_present_vert));
  VkShaderModule fs = MakeModule(k_present_frag, sizeof(k_present_frag));
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;
  const VkFormat output_format = VulkanPresenter::kGuestOutputFormat;
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &output_format;
  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = r.present_layout;
  const bool made = Check(vk.vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, nullptr, &r.present),
                          "present pipeline");
  vk.vkDestroyShaderModule(vk.device, vs, nullptr);
  vk.vkDestroyShaderModule(vk.device, fs, nullptr);
  return made;
}

// draws the picture into the presenter's image; the presenter wants it back readable by its fragment shaders
void RecordPresent(VkCommandBuffer cb, const VulkanPresenter::VulkanGuestOutputRefreshContext& out,
                   VkDescriptorSet set, uint32_t width, uint32_t height) {
  auto& vk = r.vk;
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = out.image();
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vk.vkCmdPipelineBarrier(cb,
                          out.image_ever_written_previously() ? VulkanPresenter::kGuestOutputInternalStageMask
                                                              : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = out.image_view();
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = {{0, 0}, {width, height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vk.vkCmdBeginRendering(cb, &rendering);
  VkViewport viewport = {0, 0, float(width), float(height), 0, 1};
  VkRect2D scissor = {{0, 0}, {width, height}};
  vk.vkCmdSetViewport(cb, 0, 1, &viewport);
  vk.vkCmdSetScissor(cb, 0, 1, &scissor);
  vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.present);
  vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.present_layout, 0, 1, &set, 0, nullptr);
  vk.vkCmdDraw(cb, 3, 1, 0, 0);
  vk.vkCmdEndRendering(cb);
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.newLayout = VulkanPresenter::kGuestOutputInternalLayout;
  vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

Vk& Api() { return r.vk; }

bool Check(VkResult result, const char* what) {
  if (result == VK_SUCCESS) return true;
  REXLOG_ERROR("[native] {} failed ({})", what, int(result));
  return false;
}

uint8_t* Physical(uint32_t address) { return r.physical + (address & 0x1FFFFFFF); }

uint32_t GuestAlloc(uint32_t bytes) {
  std::lock_guard lock(r.mutex);
  return r.memory ? r.memory->SystemHeapAlloc(bytes, 0x1000, rex::memory::kSystemHeapPhysical) : 0;
}

uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < r.vk.memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (r.vk.memory.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

VkImageAspectFlags Aspect(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D32_SFLOAT:
      return VK_IMAGE_ASPECT_DEPTH_BIT;
    default:
      return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

bool MakeImage(Image& out, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
               uint32_t levels, VkComponentMapping swizzle, ImageKind kind) {
  auto& vk = r.vk;
  const uint32_t layers = kind == ImageKind::kCube ? 6 : 1;
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = kind == ImageKind::kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.imageType = kind == ImageKind::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = levels;
  info.arrayLayers = layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!Check(vk.vkCreateImage(vk.device, &info, nullptr, &out.image), "vkCreateImage")) return false;
  VkMemoryRequirements req;
  vk.vkGetImageMemoryRequirements(vk.device, out.image, &req);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!Check(vk.vkAllocateMemory(vk.device, &alloc, nullptr, &out.memory), "vkAllocateMemory") ||
      !Check(vk.vkBindImageMemory(vk.device, out.image, out.memory, 0), "vkBindImageMemory"))
    return false;
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = kind == ImageKind::kCube ? VK_IMAGE_VIEW_TYPE_CUBE
                  : kind == ImageKind::k3D  ? VK_IMAGE_VIEW_TYPE_3D
                                            : VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.components = swizzle;
  // depth images are sampled through their depth aspect only
  const VkImageAspectFlags aspect = Aspect(format);
  view.subresourceRange = {(aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : aspect,
                           0, levels, 0, layers};
  out.format = format;
  out.layers = layers;
  out.width = width;
  out.height = height;
  out.levels = levels;
  out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return Check(vk.vkCreateImageView(vk.device, &view, nullptr, &out.view), "vkCreateImageView");
}

void FreeImage(Image& image) {
  auto& vk = r.vk;
  if (image.view) vk.vkDestroyImageView(vk.device, image.view, nullptr);
  if (image.image) vk.vkDestroyImage(vk.device, image.image, nullptr);
  if (image.memory) vk.vkFreeMemory(vk.device, image.memory, nullptr);
  image = {};
}

VkImageView MakeView(const Image& image, VkComponentMapping swizzle) {
  auto& vk = r.vk;
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = image.format;
  view.components = swizzle;
  view.subresourceRange = {Aspect(image.format), 0, image.levels, 0, 1};
  VkImageView out = VK_NULL_HANDLE;
  Check(vk.vkCreateImageView(vk.device, &view, nullptr, &out), "vkCreateImageView");
  return out;
}

void RetireImage(Image& image) {
  std::lock_guard lock(r.mutex);
  if (image.image || image.view) r.retired.emplace_back(r.frames_begun, image);
  image = {};
}

void Transition(VkCommandBuffer cb, Image& image, VkImageLayout to) {
  if (image.layout == to) return;
  const auto use = [](VkImageLayout layout, VkPipelineStageFlags& stage, VkAccessFlags& access) {
    switch (layout) {
      case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
      case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
      case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_SHADER_READ_BIT;
        break;
      case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
      case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
      default:
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        access = 0;
        break;
    }
  };
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  VkPipelineStageFlags src_stage, dst_stage;
  use(image.layout, src_stage, b.srcAccessMask);
  use(to, dst_stage, b.dstAccessMask);
  b.oldLayout = image.layout;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image.image;
  b.subresourceRange = {Aspect(image.format), 0, image.levels, 0, image.layers};
  r.vk.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  image.layout = to;
}

VkShaderModule MakeModule(const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = bytes;
  info.pCode = code;
  VkShaderModule module = VK_NULL_HANDLE;
  Check(r.vk.vkCreateShaderModule(r.vk.device, &info, nullptr, &module), "vkCreateShaderModule");
  return module;
}

Upload Allocate(size_t bytes, size_t align) {
  std::lock_guard lock(r.mutex);
  BeginFrame();
  Frame& f = r.frames[r.frame];
  VkDeviceSize at = (f.ring_used + align - 1) & ~VkDeviceSize(align - 1);
  if (at + bytes > kRingPerFrame) {
    static bool warned = false;
    if (!warned) REXLOG_WARN("[native] the frame's upload space ran out ({} MB)", kRingPerFrame >> 20);
    warned = true;
    at = 0;
  }
  f.ring_used = at + bytes;
  r.peak_upload = std::max(r.peak_upload, f.ring_used);
  const VkDeviceSize offset = r.frame * kRingPerFrame + at;
  return {r.ring_host + offset, r.ring, offset, r.ring_address + offset};
}

VkCommandBuffer UploadCommands() {
  std::lock_guard lock(r.mutex);
  BeginFrame();
  return r.frames[r.frame].upload;
}

std::unique_lock<std::recursive_mutex> FrameLock() { return std::unique_lock(r.mutex); }

VkCommandBuffer SceneCommands() {
  BeginFrame();
  return r.frames[r.frame].scene;
}

uint32_t HeapAdd(Heap heap, VkImageView view, VkSampler sampler) {
  std::lock_guard lock(r.mutex);
  const int i = int(heap);
  uint32_t index;
  if (!r.heap_free[i].empty()) {
    index = r.heap_free[i].back();
    r.heap_free[i].pop_back();
  } else if (r.heap_used[i] < kHeapSizes[i]) {
    index = r.heap_used[i]++;
  } else {
    static bool warned = false;
    if (!warned) REXLOG_WARN("[native] texture table {} is full", i);
    warned = true;
    return 0;
  }
  VkDescriptorImageInfo image = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = r.heaps[i];
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = heap == Heap::kSampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image;
  r.vk.vkUpdateDescriptorSets(r.vk.device, 1, &write, 0, nullptr);
  return index;
}

void HeapFree(Heap heap, uint32_t index) {
  std::lock_guard lock(r.mutex);
  if (index) r.heap_freed.push_back({r.frames_begun, int(heap), index});
}

void TakeGpuWait(double& total_ms, double& worst_ms) {
  std::lock_guard lock(r.mutex);
  total_ms = r.gpu_wait_ms;
  worst_ms = r.gpu_wait_worst;
  r.gpu_wait_ms = r.gpu_wait_worst = 0;
}

uint64_t TakePeakUpload() {
  std::lock_guard lock(r.mutex);
  const uint64_t peak = r.peak_upload;
  r.peak_upload = 0;
  return peak;
}

uint64_t FrameNumber() {
  std::lock_guard lock(r.mutex);
  BeginFrame();
  return r.frames_begun;
}

VkPipelineLayout ShaderLayout() { return r.shader_layout; }

void BindHeaps(VkCommandBuffer cb) {
  r.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.shader_layout, 0, 4, r.heaps, 0, nullptr);
}

uint32_t RenderScale() { return r.scale; }
VkFormat DepthFormat() { return r.depth_format; }

bool RenderInit(rex::Runtime* runtime, uint32_t scale) {
  auto* gfx = runtime ? runtime->graphics_system() : nullptr;
  if (!gfx || !gfx->provider() || !gfx->presenter()) {
    REXLOG_WARN("[native] no graphics device to draw on");
    return false;
  }
  // the native plugin's provider is Vulkan; the emulator's can be Direct3D 12 on Windows
  auto* provider = dynamic_cast<rex::ui::vulkan::VulkanProvider*>(gfx->provider());
  if (!provider) {
    REXLOG_ERROR("[native] the graphics backend isn't Vulkan; the native renderer can't draw on it");
    return false;
  }
  auto& vk = r.vk;
  r.scale = scale;
  r.physical = runtime->memory()->physical_membase();
  r.memory = runtime->memory();
  vk.rex = provider->vulkan_device();
  vk.presenter = gfx->presenter();
  vk.device = vk.rex->device();
  vk.family = vk.rex->queue_family_graphics_compute();
  const auto& ifn = vk.rex->vulkan_instance()->functions();
  ifn.vkGetPhysicalDeviceMemoryProperties(vk.rex->physical_device(), &vk.memory);
  // the 360's depth is 24-bit with stencil; not every GPU has that format (Apple's, some AMD), and 32-bit
  // float depth with stencil is the next nearest
  // BARDOWN_NATIVE_DEPTH32=1 takes the fallback anyway, to test it on a GPU that has both
  const bool force32 = std::getenv("BARDOWN_NATIVE_DEPTH32") != nullptr;
  for (const VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    if (force32 && f == VK_FORMAT_D24_UNORM_S8_UINT) continue;
    VkFormatProperties props;
    ifn.vkGetPhysicalDeviceFormatProperties(vk.rex->physical_device(), f, &props);
    constexpr VkFormatFeatureFlags kNeed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                           VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((props.optimalTilingFeatures & kNeed) == kNeed) {
      r.depth_format = f;
      break;
    }
  }
  // what the renderer can't draw without; a GPU missing any of it is told plainly, not left on a black screen
  {
    std::string missing;
    if (vk.rex->properties().apiVersion < VK_API_VERSION_1_2 || !ifn.vkGetPhysicalDeviceFeatures2) {
      missing = "Vulkan 1.2";
    } else {
      VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f.pNext = &f12;
      ifn.vkGetPhysicalDeviceFeatures2(vk.rex->physical_device(), &f);
      const std::pair<VkBool32, const char*> needs[] = {
          {f.features.shaderInt64, "64-bit shader integers"},
          {f12.bufferDeviceAddress, "buffer device addresses"},
          {f12.runtimeDescriptorArray, "runtime descriptor arrays"},
          {f12.descriptorBindingPartiallyBound, "partially bound descriptors"},
          {f12.descriptorBindingVariableDescriptorCount, "variable descriptor counts"},
          {f12.descriptorBindingSampledImageUpdateAfterBind, "update-after-bind images"},
          {f12.shaderSampledImageArrayNonUniformIndexing, "non-uniform image indexing"},
      };
      for (const auto& [has, what] : needs)
        if (!has) missing += std::string(missing.empty() ? "" : ", ") + what;
    }
    if (!missing.empty()) {
      const std::string text = fmt::format(
          "{} doesn't support what the native renderer needs ({}).\n\nSet native_renderer = false in bardown.toml "
          "to use the emulated GPU instead.",
          vk.rex->properties().deviceName, missing);
      REXLOG_ERROR("[native] {}", text);
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "bardown", text.c_str(), nullptr);
      return false;
    }
  }
  bool loaded = true;
#define X(name)                                                                         \
  vk.name = reinterpret_cast<PFN_##name>(ifn.vkGetDeviceProcAddr(vk.device, #name)); \
  if (!vk.name) {                                                                       \
    REXLOG_ERROR("[native] the device has no " #name);                                \
    loaded = false;                                                                     \
  }
  BARDOWN_VK_DEVICE_FUNCTIONS(X)
#undef X
  if (!loaded) return false;

  VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = vk.family;
  if (!Check(vk.vkCreateCommandPool(vk.device, &pool, nullptr, &r.pool), "vkCreateCommandPool")) return false;
  for (auto& f : r.frames) {
    VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = r.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 3;
    VkCommandBuffer cbs[3];
    if (!Check(vk.vkAllocateCommandBuffers(vk.device, &alloc, cbs), "vkAllocateCommandBuffers")) return false;
    f.upload = cbs[0];
    f.scene = cbs[1];
    f.present = cbs[2];
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!Check(vk.vkCreateFence(vk.device, &fence, nullptr, &f.fence), "vkCreateFence")) return false;
  }
  if (!MakeRing() || !MakeHeaps() || !MakePresent()) return false;

  TexturesInit(runtime->memory());
  rex::ui::SetGuestOutputExternal(true);
  r.ready = true;
  REXLOG_INFO("[native] drawing at {}x{} on {}{}", kGuestWidth * scale, kGuestHeight * scale,
              vk.rex->properties().deviceName,
              r.depth_format == VK_FORMAT_D24_UNORM_S8_UINT ? "" : " (32-bit float depth)");
  return true;
}

// BARDOWN_NATIVE_FRAMES=<present>:<count>: that many presented frames in a row, every one, written to
// logs/native/frames as ppm (screenshots on a timer skip frames, which hides anything that flips each frame)
struct FrameDump {
  uint64_t from = 0, count = 0;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* host = nullptr;
  VkDeviceSize size = 0;
};
FrameDump dump = [] {
  FrameDump d;
  if (const char* e = std::getenv("BARDOWN_NATIVE_FRAMES"))
    std::sscanf(e, "%" SCNu64 ":%" SCNu64, &d.from, &d.count);
  return d;
}();

bool DumpBuffer(VkDeviceSize size) {
  auto& vk = r.vk;
  if (dump.size >= size) return true;
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (!Check(vk.vkCreateBuffer(vk.device, &info, nullptr, &dump.buffer), "dump buffer")) return false;
  VkMemoryRequirements req;
  vk.vkGetBufferMemoryRequirements(vk.device, dump.buffer, &req);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
  if (!Check(vk.vkAllocateMemory(vk.device, &alloc, nullptr, &dump.memory), "dump memory") ||
      !Check(vk.vkBindBufferMemory(vk.device, dump.buffer, dump.memory, 0), "dump bind") ||
      !Check(vk.vkMapMemory(vk.device, dump.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&dump.host)),
             "dump map"))
    return false;
  dump.size = size;
  return true;
}

bool FlushAndRead(Image& image, std::vector<uint8_t>& out) {
  std::lock_guard lock(r.mutex);
  if (!r.ready || (image.format != VK_FORMAT_R8G8B8A8_UNORM && image.format != VK_FORMAT_B8G8R8A8_UNORM))
    return false;
  const VkDeviceSize bytes = VkDeviceSize(image.width) * image.height * 4;
  if (!DumpBuffer(bytes)) return false;
  auto& vk = r.vk;
  BeginFrame();
  Frame& f = r.frames[r.frame];
  Transition(f.scene, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy copy = {};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {image.width, image.height, 1};
  vk.vkCmdCopyImageToBuffer(f.scene, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump.buffer, 1, &copy);
  Transition(f.scene, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  vk.vkEndCommandBuffer(f.upload);
  vk.vkEndCommandBuffer(f.scene);
  {
    VkCommandBuffer cbs[2] = {f.upload, f.scene};
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = cbs;
    auto queue = vk.rex->AcquireQueue(vk.family, 0);
    Check(vk.vkQueueSubmit(queue.queue(), 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    vk.vkQueueWaitIdle(queue.queue());
  }
  out.assign(dump.host, dump.host + bytes);
  if (image.format == VK_FORMAT_B8G8R8A8_UNORM)
    for (size_t i = 0; i < out.size(); i += 4) std::swap(out[i], out[i + 2]);
  // the rest of the frame goes on in the same command buffers
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  for (VkCommandBuffer cb : {f.upload, f.scene}) {
    vk.vkResetCommandBuffer(cb, 0);
    vk.vkBeginCommandBuffer(cb, &begin);
  }
  return true;
}

void RenderPresent(Image* picture) {
  if (!r.ready) return;
  auto& vk = r.vk;
  std::lock_guard lock(r.mutex);
  BeginFrame();
  Frame& f = r.frames[r.frame];
  r.frame = (r.frame + 1) % kFrames;
  r.recording = false;

  const bool dumping = picture && dump.count && r.presents + 1 >= dump.from && r.presents + 1 < dump.from + dump.count &&
                      (picture->format == VK_FORMAT_R8G8B8A8_UNORM || picture->format == VK_FORMAT_B8G8R8A8_UNORM) &&
                      DumpBuffer(VkDeviceSize(picture->width) * picture->height * 4);
  if (dumping) {
    Transition(f.scene, *picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {picture->width, picture->height, 1};
    vk.vkCmdCopyImageToBuffer(f.scene, picture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump.buffer, 1, &copy);
  }
  if (picture) Transition(f.scene, *picture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  vk.vkEndCommandBuffer(f.upload);
  vk.vkEndCommandBuffer(f.scene);

  bool submitted = false;
  if (picture) {
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, picture->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = f.present_set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vk.vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);
    rex::ui::RefreshGuestOutputExternal(
        *vk.presenter, picture->width, picture->height, kGuestWidth, kGuestHeight,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          const auto& out = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          context.SetIs8bpc(true);
          VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
          begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
          vk.vkResetCommandBuffer(f.present, 0);
          vk.vkBeginCommandBuffer(f.present, &begin);
          RecordPresent(f.present, out, f.present_set, picture->width, picture->height);
          vk.vkEndCommandBuffer(f.present);
          VkCommandBuffer cbs[3] = {f.upload, f.scene, f.present};
          VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
          submit.commandBufferCount = 3;
          submit.pCommandBuffers = cbs;
          auto queue = vk.rex->AcquireQueue(vk.family, 0);
          submitted = Check(vk.vkQueueSubmit(queue.queue(), 1, &submit, f.fence), "vkQueueSubmit");
          return submitted;
        });
  }
  if (!submitted) {
    // nothing to show it in right now; the work still has to go, or the fence never comes back
    VkCommandBuffer cbs[2] = {f.upload, f.scene};
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = cbs;
    auto queue = vk.rex->AcquireQueue(vk.family, 0);
    Check(vk.vkQueueSubmit(queue.queue(), 1, &submit, f.fence), "vkQueueSubmit");
  }

  if (dumping) {
    vk.vkWaitForFences(vk.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    std::filesystem::create_directories("logs/native/frames");
    const std::string path = fmt::format("logs/native/frames/{:05}.ppm", r.presents + 1);
    if (FILE* out = std::fopen(path.c_str(), "wb")) {
      std::fprintf(out, "P6\n%u %u\n255\n", picture->width, picture->height);
      const bool bgra = picture->format == VK_FORMAT_B8G8R8A8_UNORM;
      std::vector<uint8_t> row(size_t(picture->width) * 3);
      for (uint32_t y = 0; y < picture->height; ++y) {
        const uint8_t* in = dump.host + size_t(y) * picture->width * 4;
        for (uint32_t x = 0; x < picture->width; ++x) {
          row[3 * x] = in[4 * x + (bgra ? 2 : 0)];
          row[3 * x + 1] = in[4 * x + 1];
          row[3 * x + 2] = in[4 * x + (bgra ? 0 : 2)];
        }
        std::fwrite(row.data(), 1, row.size(), out);
      }
      std::fclose(out);
    }
  }

  // RenderDoc's own captures stop at the window's presents, which don't line up with these frames
  ++r.presents;
  if (r.rdoc_frame && RenderDoc()) {
    // RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE, which the SDK's older copy of the header doesn't have
    void* device = *reinterpret_cast<void**>(vk.rex->vulkan_instance()->instance());
    if (r.presents == r.rdoc_frame) {
      RenderDoc()->EndFrameCapture(device, nullptr);
      REXLOG_INFO("[native] captured frame {} for RenderDoc", r.presents);
    }
    if (r.presents + 1 == r.rdoc_frame) RenderDoc()->StartFrameCapture(device, nullptr);
  }
}

}  // namespace bardown::native
