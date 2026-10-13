#pragma once

// the translated shaders as Vulkan modules, and the pipelines built from them and the game's render state

#include <cstdint>

#include "vk.h"

namespace bardown::native {

struct ShaderInfo {
  VkShaderModule module = VK_NULL_HANDLE;
  uint32_t spec = 0;             // spec constants the shader reads
  uint32_t inputs = 0;           // vertex shaders: the input locations it reads
  uint32_t integer_inputs = 0;   // of those, the ones declared as integers
  uint32_t outputs = 0;          // pixel shaders: the colour targets it writes
};
// nullptr when the shader isn't in the cache
const ShaderInfo* Shader(uint64_t hash);

struct VertexAttribute {
  uint8_t location = 0, binding = 0;
  uint16_t offset = 0;
  VkFormat format = VK_FORMAT_UNDEFINED;
};

// everything a pipeline is made from. it's hashed as bytes, so it's laid out without padding
struct PipelineKey {
  uint64_t vs = 0, ps = 0;
  uint32_t spec = 0;
  uint32_t blend = 0;       // RB_BLENDCONTROL0, or 0 when blending is off
  uint32_t depth = 0;       // RB_DEPTHCONTROL
  uint32_t color_mask = 0;  // RB_COLOR_MASK, the first target's four bits
  VkFormat color_format = VK_FORMAT_UNDEFINED, depth_format = VK_FORMAT_UNDEFINED;
  uint8_t topology = 0;     // VkPrimitiveTopology
  uint8_t cull = 0;         // VkCullModeFlags
  uint8_t front_cw = 0;
  uint8_t restart = 0;
  uint8_t attribute_count = 0, binding_count = 0;
  uint16_t strides[16] = {};
  uint16_t unused = 0;
  VertexAttribute attributes[16] = {};
};
static_assert(sizeof(PipelineKey) == 208, "PipelineKey has padding, and it's hashed as bytes");
VkPipeline Pipeline(const PipelineKey& key);

}  // namespace bardown::native
