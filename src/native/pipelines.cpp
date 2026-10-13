#include "pipelines.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#include "native.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace bardown::native {

namespace {

// the game's worker threads draw too
std::recursive_mutex mutex;
std::unordered_map<uint64_t, std::unique_ptr<ShaderInfo>> shaders;
std::unordered_map<uint64_t, VkPipeline> pipelines;

// reads what the stage takes in and gives out, by location, from the SPIR-V itself
void Reflect(const uint32_t* code, size_t words, ShaderInfo& info) {
  enum : uint32_t {
    kEntryPoint = 15, kTypeInt = 21, kTypeVector = 23, kTypePointer = 32, kVariable = 59, kDecorate = 71,
    kLocation = 30, kInput = 1, kOutput = 3, kVertex = 0
  };
  std::unordered_map<uint32_t, uint32_t> location, pointee, component;
  std::unordered_map<uint32_t, bool> is_int;
  struct Var {
    uint32_t id, type, storage;
  };
  std::vector<Var> vars;
  uint32_t model = 4;
  for (size_t i = 5; i < words;) {
    const uint32_t op = code[i] & 0xFFFF, n = code[i] >> 16;
    if (!n || i + n > words) break;
    const uint32_t* a = code + i + 1;
    switch (op) {
      case kEntryPoint:
        model = a[0];
        break;
      case kDecorate:
        if (a[1] == kLocation) location[a[0]] = a[2];
        break;
      case kTypeInt:
        is_int[a[0]] = true;
        break;
      case kTypeVector:
        component[a[0]] = a[1];
        break;
      case kTypePointer:
        pointee[a[0]] = a[2];
        break;
      case kVariable:
        vars.push_back({a[1], a[0], a[2]});
        break;
      default:
        break;
    }
    i += n;
  }
  for (const Var& v : vars) {
    auto loc = location.find(v.id);
    if (loc == location.end() || loc->second >= 32) continue;
    const uint32_t bit = 1u << loc->second;
    if (model == kVertex && v.storage == kInput) {
      info.inputs |= bit;
      uint32_t type = pointee[v.type];
      if (auto c = component.find(type); c != component.end()) type = c->second;
      if (is_int.count(type)) info.integer_inputs |= bit;
    } else if (model != kVertex && v.storage == kOutput) {
      info.outputs |= bit;
    }
  }
}

VkBlendFactor Factor(uint32_t f) {
  switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp Op(uint32_t op) {
  switch (op) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

// xenos compare functions are numbered the way Vulkan's are
VkCompareOp Compare(uint32_t f) { return VkCompareOp(f & 7); }

VkStencilOp StencilOp(uint32_t op) {
  static constexpr VkStencilOp ops[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                         VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                         VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                         VK_STENCIL_OP_DECREMENT_AND_WRAP};
  return ops[op & 7];
}

VkPipeline Make(const PipelineKey& key) {
  const ShaderInfo* vs = Shader(key.vs);
  const ShaderInfo* ps = key.ps ? Shader(key.ps) : nullptr;
  if (!vs || (key.ps && !ps)) return VK_NULL_HANDLE;

  VkSpecializationMapEntry entry = {0, 0, 4};
  VkSpecializationInfo spec = {1, &entry, 4, &key.spec};
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs->module;
  stages[0].pName = "main";
  stages[0].pSpecializationInfo = &spec;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps ? ps->module : VK_NULL_HANDLE;
  stages[1].pName = "main";
  stages[1].pSpecializationInfo = &spec;

  VkVertexInputBindingDescription bindings[16];
  for (uint32_t i = 0; i < key.binding_count; ++i)
    bindings[i] = {i, key.strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[16];
  for (uint32_t i = 0; i < key.attribute_count; ++i) {
    const auto& a = key.attributes[i];
    attributes[i] = {a.location, a.binding, a.format, a.offset};
  }
  VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  input.vertexBindingDescriptionCount = key.binding_count;
  input.pVertexBindingDescriptions = bindings;
  input.vertexAttributeDescriptionCount = key.attribute_count;
  input.pVertexAttributeDescriptions = attributes;

  VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VkPrimitiveTopology(key.topology);
  assembly.primitiveRestartEnable = key.restart;
  VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.cullMode = key.cull;
  raster.frontFace = key.front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  if (key.depth_format != VK_FORMAT_UNDEFINED) {
    depth.depthTestEnable = (key.depth >> 1) & 1;
    depth.depthWriteEnable = (key.depth >> 2) & 1;
    depth.depthCompareOp = Compare(key.depth >> 4);
    depth.stencilTestEnable = key.depth & 1;
    depth.front = {StencilOp(key.depth >> 11), StencilOp(key.depth >> 14), StencilOp(key.depth >> 17),
                   Compare(key.depth >> 8), 0xFF, 0xFF, 0};
    depth.back = (key.depth >> 7) & 1 ? VkStencilOpState{StencilOp(key.depth >> 23), StencilOp(key.depth >> 26),
                                                          StencilOp(key.depth >> 29), Compare(key.depth >> 20),
                                                          0xFF, 0xFF, 0}
                                      : depth.front;
  }

  VkPipelineColorBlendAttachmentState attachment = {};
  attachment.colorWriteMask = key.color_mask & 0xF;
  if (key.blend) {
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = Factor(key.blend & 0x1F);
    attachment.colorBlendOp = Op(key.blend >> 5 & 7);
    attachment.dstColorBlendFactor = Factor(key.blend >> 8 & 0x1F);
    attachment.srcAlphaBlendFactor = Factor(key.blend >> 16 & 0x1F);
    attachment.alphaBlendOp = Op(key.blend >> 21 & 7);
    attachment.dstAlphaBlendFactor = Factor(key.blend >> 24 & 0x1F);
  }
  VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = key.color_format != VK_FORMAT_UNDEFINED ? 1 : 0;
  blend.pAttachments = &attachment;

  // stencil reference and masks come from RB_STENCILREFMASK(_BF) at each draw
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                     VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                     VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 6;
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = blend.attachmentCount;
  rendering.pColorAttachmentFormats = &key.color_format;
  rendering.depthAttachmentFormat = key.depth_format;
  rendering.stencilAttachmentFormat =
      key.depth_format == VK_FORMAT_D24_UNORM_S8_UINT || key.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT
          ? key.depth_format
          : VK_FORMAT_UNDEFINED;

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = ps ? 2 : 1;
  info.pStages = stages;
  info.pVertexInputState = &input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = ShaderLayout();
  VkPipeline pipeline = VK_NULL_HANDLE;
  Check(Api().vkCreateGraphicsPipelines(Api().device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "pipeline");
  return pipeline;
}

}  // namespace

const ShaderInfo* Shader(uint64_t hash) {
  std::lock_guard lock(mutex);
  if (auto it = shaders.find(hash); it != shaders.end()) return it->second.get();
  const uint32_t* code;
  size_t bytes;
  uint32_t spec;
  std::unique_ptr<ShaderInfo> info;
  if (ShaderSpirv(hash, code, bytes, spec) && bytes) {
    info = std::make_unique<ShaderInfo>();
    info->module = MakeModule(code, bytes);
    info->spec = spec;
    Reflect(code, bytes / 4, *info);
    if (!info->module) info.reset();
  }
  return shaders.emplace(hash, std::move(info)).first->second.get();
}

VkPipeline Pipeline(const PipelineKey& key) {
  std::lock_guard lock(mutex);
  const uint64_t hash = XXH3_64bits(&key, sizeof(key));
  if (auto it = pipelines.find(hash); it != pipelines.end()) return it->second;
  VkPipeline pipeline = Make(key);
  if (!pipeline) REXLOG_WARN("[native] couldn't build a pipeline for shaders {:016X} {:016X}", key.vs, key.ps);
  pipelines.emplace(hash, pipeline);
  return pipeline;
}

}  // namespace bardown::native
