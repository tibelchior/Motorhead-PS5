// The game's OpenGL scene renderer (motorhead-linux/src/renderer/hardware/opengl_renderer.cpp), ported to
// Vulkan for the PS5. Same vertex data, same shading, same screen-space projection passes (vehicle
// shadows, headlights) over a snapshot of the depth buffer; only the API differs:
//   - the depth buffer holds 1 - near/view_depth (Vulkan's 0..1 range, so clip z is depth - near)
//   - the scene is drawn in render passes: one per run of geometry, and one per projection (each takes
//     a depth snapshot first)
//   - the finished image is read back into an SDL texture (the HUD is composed on the CPU)
#include <renderer/hardware/vulkan_renderer.hpp>

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
struct MhVulkanDevice {
  VkInstance instance;
  VkPhysicalDevice physical;
  VkDevice device;
  VkQueue queue;
  uint32_t family;
  bool anisotropy, line_fill;
};
bool mh_ps5_vulkan_device(MhVulkanDevice *out);  // ps5_present.cpp
void mh_ps5_log(const char *text);               // ps5_io.cpp
}

namespace mh::render::hardware {
namespace {

#include "vk_shaders.inc"

void log(const char *format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  char line[560];
  std::snprintf(line, sizeof(line), "[vulkan] %s", text);
  mh_ps5_log(line);
}

#define VK_FUNCTIONS(X) \
  X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
  X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
  X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
  X(vkInvalidateMappedMemoryRanges) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateRenderPass) \
  X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) \
  X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
  X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
  X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
  X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
  X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
  X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdPushConstants) \
  X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdCopyImage) \
  X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier)

struct Fn {
#define DECLARE(name) PFN_##name name = nullptr;
  VK_FUNCTIONS(DECLARE)
#undef DECLARE
};

struct Vertex { float x, y, z, r, g, b, a, u, v; };

struct Batch {
  int first = 0;
  int count = 0;
  const std::vector<mh::content::PamRgbaImage> *texture = nullptr;
  SceneBlendMode blend = SceneBlendMode::opaque;
  bool trilinear = false, bias = false, depth = true;
  std::optional<std::size_t> shadow;
  VkDescriptorSet set = VK_NULL_HANDLE;
};

// Uniforms of the projection pass (std140: every member a vec4 or an array of them).
struct Params {
  float output_size[4], view_position[4], view_right[4], view_up[4], view_forward[4];
  float world_bounds[4], cue[4], cue_color[4], vehicle_position[4];
  float body0[4], body1[4], body2[4], source0[4], direction0[4], source1[4], direction1[4];
  float cell_xz[18][4];
  float cell_y[9][4];
  int32_t mode[4];
};
static_assert(sizeof(Params) == 704, "Params must match the shaders' uniform block");

struct ScenePush {
  float output_size[4], fog_color[4], fog_range[4];
  int32_t mode;
};

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;  // bytes R,G,B,A: SDL_PIXELFORMAT_RGBA32
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr uint32_t kProjectionSlots = 64;
constexpr VkDeviceSize kSlotSize = 1024;  // a multiple of any uniform offset alignment
constexpr VkDeviceSize kArenaSize = 64u << 20;

void check(VkResult result, const char *what) {
  if (result != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan ") + what + " failed: " + std::to_string(int(result)));
}

}  // namespace

static bool g_scene_opaque = false;
extern "C" void mh_ps5_scene_opaque(bool opaque) { g_scene_opaque = opaque; }

struct VulkanRenderer::Impl {
  MhVulkanDevice dev{};
  Fn f;
  VkPhysicalDeviceMemoryProperties memory{};
  bool ready = false;
  double render_ms = 0, readback_ms = 0;
  unsigned long frames = 0;
  double sum_render = 0, sum_readback = 0;

  std::vector<std::function<void()>> cleanup;         // run in reverse order at the end
  std::vector<std::function<void()>> target_cleanup;  // the render targets (re-made on a size change)

  // fixed objects
  VkRenderPass pass_clear = VK_NULL_HANDLE, pass_load = VK_NULL_HANDLE, pass_overlay = VK_NULL_HANDLE;
  VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE, projection_layout = VK_NULL_HANDLE;
  VkPipelineLayout scene_pipeline_layout = VK_NULL_HANDLE, projection_pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule scene_vs = VK_NULL_HANDLE, scene_fs = VK_NULL_HANDLE, projection_vs = VK_NULL_HANDLE, projection_fs = VK_NULL_HANDLE;
  VkSampler bilinear = VK_NULL_HANDLE, trilinear = VK_NULL_HANDLE, nearest = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkPipeline projection_shadow = VK_NULL_HANDLE, projection_light = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkPipeline> scene_pipelines;

  // buffers written by the CPU every frame
  VkBuffer vertex_buffer = VK_NULL_HANDLE;
  VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
  void *vertex_mapped = nullptr;
  size_t vertex_capacity = 0;  // in vertices
  VkBuffer uniform_buffer = VK_NULL_HANDLE;
  VkDeviceMemory uniform_memory = VK_NULL_HANDLE;
  uint8_t *uniform_mapped = nullptr;

  // textures
  struct Arena { VkDeviceMemory memory; VkDeviceSize size, used; uint32_t type; };
  std::vector<Arena> arenas;
  struct Texture { VkImage image = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; uint32_t levels = 0; };
  std::unordered_map<const void *, Texture> textures;
  Texture white;
  struct Pending { VkImage image; const std::vector<mh::content::PamRgbaImage> *levels; uint32_t count; };
  std::vector<Pending> pending;
  struct SetKey { const void *texture; bool trilinear; bool operator==(const SetKey &o) const { return texture == o.texture && trilinear == o.trilinear; } };
  struct SetHash { size_t operator()(const SetKey &k) const { return std::hash<const void *>()(k.texture) * 2 + (k.trilinear ? 1 : 0); } };
  std::unordered_map<SetKey, VkDescriptorSet, SetHash> sets;

  // render targets
  int width = 0, height = 0;
  VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE, depth_copy = VK_NULL_HANDLE;
  VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE, depth_copy_view = VK_NULL_HANDLE;
  VkFramebuffer scene_framebuffer = VK_NULL_HANDLE, overlay_framebuffer = VK_NULL_HANDLE;
  VkDescriptorSet projection_set = VK_NULL_HANDLE;
  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  uint8_t *readback_mapped = nullptr;
  bool readback_coherent = true;
  SDL_Texture *wrapped = nullptr;
  VkImageLayout depth_copy_layout = VK_IMAGE_LAYOUT_UNDEFINED;

  // per-frame
  std::vector<Vertex> vertices;
  std::vector<Batch> batches;
  bool in_pass = false, first_pass = true;
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  VkDescriptorSet bound_set = VK_NULL_HANDLE;
  float bound_bias = 1.0F;
  ScenePush push{};
  uint32_t next_slot = 0;
  size_t next_quad = 0;  // in vertices, after the scene's

  ~Impl() {
    if (!dev.device) return;
    if (f.vkDeviceWaitIdle) f.vkDeviceWaitIdle(dev.device);
    for (auto it = target_cleanup.rbegin(); it != target_cleanup.rend(); ++it) (*it)();
    for (auto it = cleanup.rbegin(); it != cleanup.rend(); ++it) (*it)();
    if (wrapped) SDL_DestroyTexture(wrapped);
  }

  // ---- memory
  bool find_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t &index) const {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) { index = i; return true; }
    return false;
  }
  VkDeviceMemory allocate(const VkMemoryRequirements &req, VkMemoryPropertyFlags flags, uint32_t *type_out = nullptr) {
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = req.size;
    if (!find_type(req.memoryTypeBits, flags, info.memoryTypeIndex)) throw std::runtime_error("Vulkan: no suitable memory type");
    if (type_out) *type_out = info.memoryTypeIndex;
    VkDeviceMemory memory_out = VK_NULL_HANDLE;
    check(f.vkAllocateMemory(dev.device, &info, nullptr, &memory_out), "vkAllocateMemory");
    return memory_out;
  }
  // Device-local memory for the textures, carved from big blocks.
  void arena_bind_image(VkImage image) {
    VkMemoryRequirements req{};
    f.vkGetImageMemoryRequirements(dev.device, image, &req);
    uint32_t type = 0;
    if (!find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) throw std::runtime_error("Vulkan: no device-local memory");
    for (auto &arena : arenas) {
      if (arena.type != type) continue;
      const VkDeviceSize offset = (arena.used + req.alignment - 1) / req.alignment * req.alignment;
      if (offset + req.size <= arena.size) {
        check(f.vkBindImageMemory(dev.device, image, arena.memory, offset), "vkBindImageMemory");
        arena.used = offset + req.size;
        return;
      }
    }
    VkMemoryRequirements block = req;
    block.size = std::max(kArenaSize, req.size);
    uint32_t type_used = 0;
    const VkDeviceMemory memory_block = allocate(block, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_used);
    arenas.push_back({memory_block, block.size, req.size, type_used});
    cleanup.push_back([this, memory_block] { f.vkFreeMemory(dev.device, memory_block, nullptr); });
    check(f.vkBindImageMemory(dev.device, image, memory_block, 0), "vkBindImageMemory");
  }
  struct HostBuffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void *mapped = nullptr; bool coherent = true; };
  HostBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool want_cached) {
    HostBuffer b;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(f.vkCreateBuffer(dev.device, &info, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    f.vkGetBufferMemoryRequirements(dev.device, b.buffer, &req);
    const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    std::vector<VkMemoryPropertyFlags> options;
    if (want_cached) { options.push_back(visible | cached | coherent); options.push_back(visible | cached); }
    options.push_back(visible | coherent);
    uint32_t index = 0;
    VkMemoryPropertyFlags chosen = 0;
    for (auto flags : options) if (find_type(req.memoryTypeBits, flags, index)) { chosen = flags; break; }
    if (!chosen) throw std::runtime_error("Vulkan: no host-visible memory");
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = index;
    check(f.vkAllocateMemory(dev.device, &alloc, nullptr, &b.memory), "vkAllocateMemory");
    check(f.vkBindBufferMemory(dev.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    check(f.vkMapMemory(dev.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    b.coherent = (chosen & coherent) != 0;
    return b;
  }
  void free_host_buffer(HostBuffer &b) {
    if (b.memory) { f.vkUnmapMemory(dev.device, b.memory); }
    if (b.buffer) f.vkDestroyBuffer(dev.device, b.buffer, nullptr);
    if (b.memory) f.vkFreeMemory(dev.device, b.memory, nullptr);
    b = {};
  }

  // ---- fixed objects
  VkShaderModule module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    check(f.vkCreateShaderModule(dev.device, &info, nullptr, &m), "vkCreateShaderModule");
    cleanup.push_back([this, m] { f.vkDestroyShaderModule(dev.device, m, nullptr); });
    return m;
  }
  VkRenderPass render_pass(bool with_depth, bool clear) {
    std::array<VkAttachmentDescription, 2> attachments{};
    for (auto &a : attachments) {
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      a.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    attachments[0].format = kColorFormat;
    attachments[1].format = kDepthFormat;
    if (!clear) attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = with_depth ? &depth_ref : nullptr;
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = deps[0].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    deps[1] = deps[0];
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = with_depth ? 2u : 1u;
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps.data();
    VkRenderPass pass = VK_NULL_HANDLE;
    check(f.vkCreateRenderPass(dev.device, &info, nullptr, &pass), "vkCreateRenderPass");
    cleanup.push_back([this, pass] { f.vkDestroyRenderPass(dev.device, pass, nullptr); });
    return pass;
  }
  VkSampler sampler(bool mipmaps, bool nearest_filter) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = nearest_filter ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = nearest_filter ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = mipmaps ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const auto wrap = nearest_filter ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeU = info.addressModeV = info.addressModeW = wrap;
    info.maxLod = mipmaps ? 1000.0F : 0.0F;
    if (mipmaps && dev.anisotropy) { info.anisotropyEnable = VK_TRUE; info.maxAnisotropy = 16.0F; }
    VkSampler s = VK_NULL_HANDLE;
    check(f.vkCreateSampler(dev.device, &info, nullptr, &s), "vkCreateSampler");
    cleanup.push_back([this, s] { f.vkDestroySampler(dev.device, s, nullptr); });
    return s;
  }
  VkDescriptorSetLayout set_layout(bool with_uniforms) {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = with_uniforms ? 2u : 1u;
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    check(f.vkCreateDescriptorSetLayout(dev.device, &info, nullptr, &layout), "vkCreateDescriptorSetLayout");
    cleanup.push_back([this, layout] { f.vkDestroyDescriptorSetLayout(dev.device, layout, nullptr); });
    return layout;
  }
  VkPipelineLayout pipeline_layout(VkDescriptorSetLayout set, bool with_push) {
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePush)};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 1;
    info.pSetLayouts = &set;
    info.pushConstantRangeCount = with_push ? 1u : 0u;
    info.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(f.vkCreatePipelineLayout(dev.device, &info, nullptr, &layout), "vkCreatePipelineLayout");
    cleanup.push_back([this, layout] { f.vkDestroyPipelineLayout(dev.device, layout, nullptr); });
    return layout;
  }
  // blend: 0 opaque, 1 alpha, 2 additive, 3 none (hidden-line), 4 one-one (headlights)
  VkPipeline make_pipeline(bool projection, bool depth_test, bool depth_write, int blend, bool line) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 projection ? projection_vs : scene_vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 projection ? projection_fs : scene_fs, "main", nullptr};
    const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const std::array<VkVertexInputAttributeDescription, 3> attributes{{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, r)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)}}};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = projection ? 1u : 3u;
    input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = VK_TRUE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_state.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_state.depthWriteEnable = (depth_test && depth_write) ? VK_TRUE : VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blend == 1 || blend == 2 || blend == 4) {
      attachment.blendEnable = VK_TRUE;
      const VkBlendFactor source = blend == 4 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
      const VkBlendFactor destination = blend == 1 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
      // The image is premultiplied: where nothing opaque is drawn it stays transparent so the game's
      // sky (drawn by SDL beneath the scene) shows through. Alpha blending accumulates coverage
      // ("over"); additive light leaves the alpha alone (it only adds colour).
      attachment.srcColorBlendFactor = source;
      attachment.dstColorBlendFactor = destination;
      attachment.srcAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
      attachment.dstAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
      attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &attachment;
    const std::array<VkDynamicState, 3> dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = uint32_t(dynamic.size());
    dynamic_state.pDynamicStates = dynamic.data();
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages.data();
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = projection ? nullptr : &depth_state;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic_state;
    info.layout = projection ? projection_pipeline_layout : scene_pipeline_layout;
    info.renderPass = projection ? pass_overlay : pass_clear;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(f.vkCreateGraphicsPipelines(dev.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    cleanup.push_back([this, pipeline] { f.vkDestroyPipeline(dev.device, pipeline, nullptr); });
    return pipeline;
  }
  VkPipeline scene_pipeline(bool depth_test, bool depth_write, int blend, bool line) {
    const uint32_t key = (depth_test ? 1u : 0u) | (depth_write ? 2u : 0u) | (uint32_t(blend) << 2) | (line ? 32u : 0u);
    auto found = scene_pipelines.find(key);
    if (found != scene_pipelines.end()) return found->second;
    const VkPipeline pipeline = make_pipeline(false, depth_test, depth_write, blend, line);
    scene_pipelines.emplace(key, pipeline);
    return pipeline;
  }

  bool setup() {
    if (!mh_ps5_vulkan_device(&dev)) {
      log("the presenter's device is not ready");
      return false;
    }
    auto get = vk_icdGetInstanceProcAddr;
    f.vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get(dev.instance, "vkGetDeviceProcAddr"));
    f.vkGetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get(dev.instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!f.vkGetDeviceProcAddr || !f.vkGetPhysicalDeviceMemoryProperties) { log("instance functions missing"); return false; }
#define LOAD(name) \
    if (!f.name) f.name = reinterpret_cast<PFN_##name>(f.vkGetDeviceProcAddr(dev.device, #name)); \
    if (!f.name) { log("device function " #name " missing"); return false; }
    // (the two instance functions are already loaded: skip them)
#define LOAD_DEVICE(name) LOAD(name)
    VK_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
#undef LOAD
    f.vkGetPhysicalDeviceMemoryProperties(dev.physical, &memory);

    scene_vs = module(scene_vert_spv, sizeof(scene_vert_spv));
    scene_fs = module(scene_frag_spv, sizeof(scene_frag_spv));
    projection_vs = module(proj_vert_spv, sizeof(proj_vert_spv));
    projection_fs = module(proj_frag_spv, sizeof(proj_frag_spv));
    pass_clear = render_pass(true, true);
    pass_load = render_pass(true, false);
    pass_overlay = render_pass(false, false);
    scene_layout = set_layout(false);
    projection_layout = set_layout(true);
    scene_pipeline_layout = pipeline_layout(scene_layout, true);
    projection_pipeline_layout = pipeline_layout(projection_layout, false);
    bilinear = sampler(false, false);
    trilinear = sampler(true, false);
    nearest = sampler(false, true);
    projection_shadow = make_pipeline(true, false, false, 0, false);
    projection_light = make_pipeline(true, false, false, 4, false);

    constexpr uint32_t kMaxSets = 16384;
    const std::array<VkDescriptorPoolSize, 2> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets + 8},
                                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8}}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = kMaxSets + 8;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes.data();
    check(f.vkCreateDescriptorPool(dev.device, &pool_info, nullptr, &pool), "vkCreateDescriptorPool");
    cleanup.push_back([this] { f.vkDestroyDescriptorPool(dev.device, pool, nullptr); });

    VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = dev.family;
    check(f.vkCreateCommandPool(dev.device, &command_pool_info, nullptr, &command_pool), "vkCreateCommandPool");
    cleanup.push_back([this] { f.vkDestroyCommandPool(dev.device, command_pool, nullptr); });
    VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    check(f.vkAllocateCommandBuffers(dev.device, &allocate_info, &cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(f.vkCreateFence(dev.device, &fence_info, nullptr, &fence), "vkCreateFence");
    cleanup.push_back([this] { f.vkDestroyFence(dev.device, fence, nullptr); });

    auto uniforms = host_buffer(kSlotSize * kProjectionSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false);
    uniform_buffer = uniforms.buffer;
    uniform_memory = uniforms.memory;
    uniform_mapped = static_cast<uint8_t *>(uniforms.mapped);
    cleanup.push_back([this, uniforms]() mutable { free_host_buffer(uniforms); });
    grow_vertices(1 << 16);

    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &projection_layout;
    check(f.vkAllocateDescriptorSets(dev.device, &set_info, &projection_set), "vkAllocateDescriptorSets");

    // The 1x1 white texture for untextured polygons.
    white = make_texture(1, 1, 1);
    static const std::array<uint8_t, 4> pixel{255, 255, 255, 255};
    upload_now_white(pixel.data());
    ready = true;
    return true;
  }

  // ---- vertex buffer
  void grow_vertices(size_t count) {
    if (vertex_buffer) {
      f.vkDeviceWaitIdle(dev.device);
      HostBuffer old{vertex_buffer, vertex_memory, vertex_mapped, true};
      free_host_buffer(old);
    }
    auto buffer = host_buffer(count * sizeof(Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    vertex_buffer = buffer.buffer;
    vertex_memory = buffer.memory;
    vertex_mapped = buffer.mapped;
    vertex_capacity = count;
    if (!vertex_cleanup_registered) {
      vertex_cleanup_registered = true;
      cleanup.push_back([this] { HostBuffer b{vertex_buffer, vertex_memory, vertex_mapped, true}; free_host_buffer(b); });
    }
  }
  bool vertex_cleanup_registered = false;

  // ---- textures
  Texture make_texture(uint32_t w, uint32_t h, uint32_t levels) {
    Texture t;
    t.levels = levels;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {w, h, 1};
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(f.vkCreateImage(dev.device, &info, nullptr, &t.image), "vkCreateImage");
    const VkImage image = t.image;
    cleanup.push_back([this, image] { f.vkDestroyImage(dev.device, image, nullptr); });
    arena_bind_image(t.image);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    check(f.vkCreateImageView(dev.device, &view, nullptr, &t.view), "vkCreateImageView");
    const VkImageView v = t.view;
    cleanup.push_back([this, v] { f.vkDestroyImageView(dev.device, v, nullptr); });
    return t;
  }
  void image_barrier(VkImage image, VkImageAspectFlags aspect, uint32_t levels, VkImageLayout from, VkImageLayout to,
                     VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, levels, 0, 1};
    f.vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
  }
  // The white texture is uploaded with its own one-off submission at start-up.
  void upload_now_white(const uint8_t *pixel) {
    auto staging = host_buffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    std::memcpy(staging.mapped, pixel, 4);
    begin_commands();
    image_barrier(white.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {1, 1, 1};
    f.vkCmdCopyBufferToImage(cmd, staging.buffer, white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(white.image, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    submit_and_wait();
    free_host_buffer(staging);
  }
  const Texture &texture(const std::vector<mh::content::PamRgbaImage> *levels) {
    if (!levels || levels->empty()) return white;
    auto found = textures.find(levels);
    if (found != textures.end()) return found->second;
    const auto &base = levels->front();
    // Only the levels that form a proper chain (each half the size of the last) are used.
    uint32_t count = 0;
    for (const auto &image : *levels) {
      const uint32_t want_w = std::max(1u, uint32_t(base.width) >> count), want_h = std::max(1u, uint32_t(base.height) >> count);
      if (uint32_t(image.width) != want_w || uint32_t(image.height) != want_h) break;
      ++count;
    }
    count = std::max(count, 1u);
    Texture t = make_texture(uint32_t(base.width), uint32_t(base.height), count);
    pending.push_back({t.image, levels, count});
    return textures.emplace(levels, t).first->second;
  }
  VkDescriptorSet scene_set(const std::vector<mh::content::PamRgbaImage> *levels, bool want_trilinear) {
    const Texture &t = texture(levels);
    const SetKey key{levels && !levels->empty() ? static_cast<const void *>(levels) : nullptr, want_trilinear && t.levels > 1};
    auto found = sets.find(key);
    if (found != sets.end()) return found->second;
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &scene_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(f.vkAllocateDescriptorSets(dev.device, &info, &set), "vkAllocateDescriptorSets (textures)");
    VkDescriptorImageInfo image{key.trilinear ? trilinear : bilinear, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    f.vkUpdateDescriptorSets(dev.device, 1, &write, 0, nullptr);
    sets.emplace(key, set);
    return set;
  }
  // Records the uploads of the textures first used this frame.
  void flush_uploads() {
    if (pending.empty()) return;
    VkDeviceSize total = 0;
    for (const auto &p : pending)
      for (uint32_t i = 0; i < p.count; ++i) total += (*p.levels)[i].rgba.size();
    auto staging = host_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
    auto *dst = static_cast<uint8_t *>(staging.mapped);
    VkDeviceSize offset = 0;
    for (const auto &p : pending) {
      image_barrier(p.image, VK_IMAGE_ASPECT_COLOR_BIT, p.count, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      std::vector<VkBufferImageCopy> copies;
      for (uint32_t i = 0; i < p.count; ++i) {
        const auto &image = (*p.levels)[i];
        std::memcpy(dst + offset, image.rgba.data(), image.rgba.size());
        VkBufferImageCopy copy{};
        copy.bufferOffset = offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        copy.imageExtent = {uint32_t(image.width), uint32_t(image.height), 1};
        copies.push_back(copy);
        offset += image.rgba.size();
      }
      f.vkCmdCopyBufferToImage(cmd, staging.buffer, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
      image_barrier(p.image, VK_IMAGE_ASPECT_COLOR_BIT, p.count, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    pending.clear();
    staging_to_free.push_back(staging);
  }
  std::vector<HostBuffer> staging_to_free;

  // ---- commands
  void begin_commands() {
    check(f.vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(f.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
  }
  void submit_and_wait() {
    check(f.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(f.vkResetFences(dev.device, 1, &fence), "vkResetFences");
    check(f.vkQueueSubmit(dev.queue, 1, &submit, fence), "vkQueueSubmit");
    check(f.vkWaitForFences(dev.device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "vkWaitForFences");
  }

  // ---- render targets
  void make_targets(SDL_Renderer *renderer, int w, int h) {
    f.vkDeviceWaitIdle(dev.device);
    for (auto it = target_cleanup.rbegin(); it != target_cleanup.rend(); ++it) (*it)();
    target_cleanup.clear();
    if (wrapped) { SDL_DestroyTexture(wrapped); wrapped = nullptr; }
    width = w;
    height = h;
    auto image = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage &out, VkImageView &view_out) {
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = format;
      info.extent = {uint32_t(w), uint32_t(h), 1};
      info.mipLevels = info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = usage;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      check(f.vkCreateImage(dev.device, &info, nullptr, &out), "vkCreateImage (target)");
      const VkImage created = out;
      target_cleanup.push_back([this, created] { f.vkDestroyImage(dev.device, created, nullptr); });
      VkMemoryRequirements req{};
      f.vkGetImageMemoryRequirements(dev.device, out, &req);
      const VkDeviceMemory memory_block = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      target_cleanup.push_back([this, memory_block] { f.vkFreeMemory(dev.device, memory_block, nullptr); });
      check(f.vkBindImageMemory(dev.device, out, memory_block, 0), "vkBindImageMemory (target)");
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = out;
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = format;
      view.subresourceRange = {aspect, 0, 1, 0, 1};
      check(f.vkCreateImageView(dev.device, &view, nullptr, &view_out), "vkCreateImageView (target)");
      const VkImageView created_view = view_out;
      target_cleanup.push_back([this, created_view] { f.vkDestroyImageView(dev.device, created_view, nullptr); });
    };
    image(kColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, color, color_view);
    image(kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, depth, depth_view);
    image(kDepthFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, depth_copy, depth_copy_view);
    depth_copy_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    auto framebuffer = [&](VkRenderPass pass, std::vector<VkImageView> views, VkFramebuffer &out) {
      VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      info.renderPass = pass;
      info.attachmentCount = uint32_t(views.size());
      info.pAttachments = views.data();
      info.width = uint32_t(w);
      info.height = uint32_t(h);
      info.layers = 1;
      check(f.vkCreateFramebuffer(dev.device, &info, nullptr, &out), "vkCreateFramebuffer");
      const VkFramebuffer created = out;
      target_cleanup.push_back([this, created] { f.vkDestroyFramebuffer(dev.device, created, nullptr); });
    };
    framebuffer(pass_clear, {color_view, depth_view}, scene_framebuffer);
    framebuffer(pass_overlay, {color_view}, overlay_framebuffer);

    // The projection pass's descriptors: the depth snapshot and the uniform slots.
    VkDescriptorImageInfo depth_info{nearest, depth_copy_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo uniform_info{uniform_buffer, 0, sizeof(Params)};
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = projection_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &depth_info;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = projection_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[1].pBufferInfo = &uniform_info;
    f.vkUpdateDescriptorSets(dev.device, 2, writes.data(), 0, nullptr);

    auto buffer = host_buffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    readback = buffer.buffer;
    readback_memory = buffer.memory;
    readback_mapped = static_cast<uint8_t *>(buffer.mapped);
    readback_coherent = buffer.coherent;
    target_cleanup.push_back([this, buffer]() mutable { free_host_buffer(buffer); });

    wrapped = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!wrapped) throw std::runtime_error(std::string("Vulkan scene texture: ") + SDL_GetError());
    // Premultiplied (see the blend states): composed over the sky the game drew beneath the scene.
    SDL_SetTextureBlendMode(wrapped, g_scene_opaque ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    log("render targets %dx%d, readback memory %s", w, h, readback_coherent ? "coherent" : "cached (invalidated per frame)");
  }

  // ---- frame
  void build(const SceneFrameView &frame) {
    vertices.clear();
    batches.clear();
    for (const auto &c : *frame.commands) {
      if (c.shadow_projection_index) { Batch b; b.shadow = c.shadow_projection_index; batches.push_back(b); continue; }
      if (c.vertex_count < 3 || c.vertex_count > 4) continue;
      const int first = static_cast<int>(vertices.size());
      constexpr std::array<unsigned, 6> indices{0, 1, 2, 0, 2, 3};
      const int count = c.vertex_count == 4 ? 6 : 3;
      for (int j = 0; j < count; j++) {
        const auto i = indices[static_cast<std::size_t>(j)];
        const auto &s = c.vertices[i];
        vertices.push_back({s.position.x, s.position.y, static_cast<float>(c.view_depths[i]),
                            s.color.r, s.color.g, s.color.b, s.color.a, s.tex_coord.x, s.tex_coord.y});
      }
      if (!batches.empty()) {
        auto &b = batches.back();
        if (!b.shadow && b.first + b.count == first && b.texture == c.texture_levels && b.blend == c.blend_mode &&
            b.trilinear == c.trilinear_filtering && b.bias == c.coplanar_depth_bias && b.depth == c.depth_test) {
          b.count += count;
          continue;
        }
      }
      Batch b;
      b.first = first;
      b.count = count;
      b.texture = c.texture_levels;
      b.blend = c.blend_mode;
      b.trilinear = c.trilinear_filtering;
      b.bias = c.coplanar_depth_bias;
      b.depth = c.depth_test;
      batches.push_back(b);
    }
  }
  void set_viewport() {
    const VkViewport viewport{0, 0, float(width), float(height), 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {uint32_t(width), uint32_t(height)}};
    f.vkCmdSetViewport(cmd, 0, 1, &viewport);
    f.vkCmdSetScissor(cmd, 0, 1, &scissor);
  }
  void begin_pass(VkRenderPass pass, VkFramebuffer fb) {
    VkClearValue clears[2]{};
    clears[0].color = {{0.0F, 0.0F, 0.0F, 0.0F}};
    clears[1].depthStencil = {1.0F, 0};
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = pass;
    info.framebuffer = fb;
    info.renderArea = {{0, 0}, {uint32_t(width), uint32_t(height)}};
    info.clearValueCount = 2;
    info.pClearValues = clears;
    f.vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    in_pass = true;
    bound_pipeline = VK_NULL_HANDLE;
    bound_set = VK_NULL_HANDLE;
    bound_bias = 1.0F;
    set_viewport();
    const VkDeviceSize zero = 0;
    f.vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &zero);
  }
  void enter_scene() {
    if (in_pass) return;
    begin_pass(first_pass ? pass_clear : pass_load, scene_framebuffer);
    first_pass = false;
  }
  void leave_pass() {
    if (!in_pass) return;
    f.vkCmdEndRenderPass(cmd);
    in_pass = false;
  }
  void draw_batch(const Batch &b, bool tron, bool line) {
    enter_scene();
    const bool depth_write = b.depth && (b.blend == SceneBlendMode::opaque || tron) && !line;
    const int blend = tron ? 3 : static_cast<int>(b.blend);
    const VkPipeline pipeline = scene_pipeline(b.depth, depth_write, blend, line);
    if (pipeline != bound_pipeline) {
      f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      bound_pipeline = pipeline;
    }
    if (b.set != bound_set) {
      f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipeline_layout, 0, 1, &b.set, 0, nullptr);
      bound_set = b.set;
    }
    const float bias = (b.bias || line) ? -1.0F : 0.0F;
    if (bias != bound_bias) {
      f.vkCmdSetDepthBias(cmd, bias, 0.0F, 0.0F);
      bound_bias = bias;
    }
    push.mode = tron ? (line ? 4 : 3) : static_cast<int>(b.blend);
    f.vkCmdPushConstants(cmd, scene_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    f.vkCmdDraw(cmd, uint32_t(b.count), 1, uint32_t(b.first), 0);
  }
  template <class V> static void put3(float (&out)[4], const V &v, float w = 0) {
    out[0] = static_cast<float>(v[0]);
    out[1] = static_cast<float>(v[1]);
    out[2] = static_cast<float>(v[2]);
    out[3] = w;
  }
  void snapshot_depth() {
    image_barrier(depth, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (depth_copy_layout == VK_IMAGE_LAYOUT_UNDEFINED)
      image_barrier(depth_copy, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    else
      image_barrier(depth_copy, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy copy{};
    copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.extent = {uint32_t(width), uint32_t(height), 1};
    f.vkCmdCopyImage(cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(depth, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    image_barrier(depth_copy, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    depth_copy_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  // One projection: the depth is snapshotted, then a rectangle is drawn over the scene reading it.
  void projection_pass(const Params &params, int left, int right, int top, int bottom, bool light) {
    if (next_slot >= kProjectionSlots) return;  // more projections than the frame has room for
    leave_pass();
    snapshot_depth();
    const uint32_t slot = next_slot++;
    std::memcpy(uniform_mapped + slot * kSlotSize, &params, sizeof(Params));
    const float l = float(left), r = float(right + 1), t = float(top), b = float(bottom + 1);
    const std::array<Vertex, 6> quad{{{l, t, 1, 0, 0, 0, 0, 0, 0}, {r, t, 1, 0, 0, 0, 0, 0, 0}, {r, b, 1, 0, 0, 0, 0, 0, 0},
                                      {l, t, 1, 0, 0, 0, 0, 0, 0}, {r, b, 1, 0, 0, 0, 0, 0, 0}, {l, b, 1, 0, 0, 0, 0, 0, 0}}};
    const size_t first = next_quad;
    std::memcpy(static_cast<Vertex *>(vertex_mapped) + first, quad.data(), sizeof(quad));
    next_quad += 6;
    begin_pass(pass_overlay, overlay_framebuffer);
    f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, light ? projection_light : projection_shadow);
    const uint32_t offset = uint32_t(slot * kSlotSize);
    f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, projection_pipeline_layout, 0, 1, &projection_set, 1, &offset);
    f.vkCmdSetDepthBias(cmd, 0.0F, 0.0F, 0.0F);
    f.vkCmdDraw(cmd, 6, 1, uint32_t(first), 0);
    leave_pass();
  }
  Params base_params(const PerspectiveView &view, double distance, double start, bool fog, const std::array<float, 3> &color_value) {
    Params p{};
    p.output_size[0] = float(width);
    p.output_size[1] = float(height);
    p.output_size[2] = view.center_x;
    p.output_size[3] = float(view.near_plane);
    put3(p.view_position, view.position, view.center_y);
    put3(p.view_right, view.right);
    put3(p.view_up, view.up);
    put3(p.view_forward, view.forward);
    p.cue[0] = float(distance);
    p.cue[1] = float(start);
    p.cue[2] = fog ? 1.0F : 0.0F;
    p.cue[3] = view.focal_length;
    put3(p.cue_color, color_value);
    return p;
  }
  void shadow(const VehicleShadowDepthProjection &p) {
    Params u = base_params(p.view, p.render_distance, p.cue_start, p.cue_enabled,
                           {p.cue_color[0] / 255, p.cue_color[1] / 255, p.cue_color[2] / 255});
    u.mode[0] = 0;
    u.world_bounds[0] = float(p.minimum_world_x);
    u.world_bounds[1] = float(p.maximum_world_x);
    u.world_bounds[2] = float(p.minimum_world_z);
    u.world_bounds[3] = float(p.maximum_world_z);
    for (std::size_t i = 0; i < p.cells.size() && i < 9; ++i) {
      const auto &c = p.cells[i];
      u.cell_xz[i * 2][0] = float(c.world_xz[0][0]);
      u.cell_xz[i * 2][1] = float(c.world_xz[0][1]);
      u.cell_xz[i * 2][2] = float(c.world_xz[1][0]);
      u.cell_xz[i * 2][3] = float(c.world_xz[1][1]);
      u.cell_xz[i * 2 + 1][0] = float(c.world_xz[2][0]);
      u.cell_xz[i * 2 + 1][1] = float(c.world_xz[2][1]);
      u.cell_xz[i * 2 + 1][2] = float(c.world_xz[3][0]);
      u.cell_xz[i * 2 + 1][3] = float(c.world_xz[3][1]);
      u.cell_y[i][0] = float(c.minimum_receiver_y);
      u.cell_y[i][1] = float(c.maximum_receiver_y);
      u.cell_y[i][2] = c.valid ? 1.0F : 0.0F;
    }
    projection_pass(u, p.minimum_x, p.maximum_x, p.minimum_y, p.maximum_y, false);
  }
  void headlight(const HeadlightDepthProjection &p) {
    Params u = base_params(p.view, p.render_distance, p.cue_start, p.cue_enabled, {0, 0, 0});
    u.mode[0] = 1;
    put3(u.vehicle_position, p.vehicle.world_position);
    put3(u.body0, p.vehicle.body_basis[0]);
    put3(u.body1, p.vehicle.body_basis[1]);
    put3(u.body2, p.vehicle.body_basis[2]);
    put3(u.source0, p.sources[0].center);
    put3(u.direction0, p.sources[0].direction);
    put3(u.source1, p.sources[1].center);
    put3(u.direction1, p.sources[1].direction);
    projection_pass(u, p.minimum_x, p.maximum_x, p.minimum_y, p.maximum_y, true);
  }

  SDL_Texture *render(SDL_Renderer *renderer, const SceneFrameView &frame) {
    const auto started = std::chrono::steady_clock::now();
    if (width != frame.width || height != frame.height || !wrapped) make_targets(renderer, frame.width, frame.height);
    SDL_SetTextureBlendMode(wrapped, g_scene_opaque ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    build(frame);
    size_t projections = 0;
    if (!frame.tron_hidden_line) {
      for (const auto &b : batches)
        if (b.shadow && *b.shadow < frame.shadow_projections->size()) ++projections;
      projections += frame.headlight_projections->size();
    }
    const size_t needed = vertices.size() + 6 * projections;
    if (needed > vertex_capacity) grow_vertices(std::max(needed * 3 / 2, vertex_capacity * 2));
    if (!vertices.empty()) std::memcpy(vertex_mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    for (auto &b : batches)
      if (!b.shadow) b.set = scene_set(b.texture, b.trilinear && b.texture && b.texture->size() > 1);

    begin_commands();
    flush_uploads();
    in_pass = false;
    first_pass = true;
    next_slot = 0;
    next_quad = vertices.size();
    push = {};
    push.output_size[0] = float(width);
    push.output_size[1] = float(height);
    push.output_size[2] = float(frame.near_plane);
    for (int i = 0; i < 3; ++i) push.fog_color[i] = frame.fog_color[std::size_t(i)] / 255;
    push.fog_color[3] = 1;
    push.fog_range[0] = float(frame.fog_start);
    push.fog_range[1] = float(frame.fog_end);
    push.fog_range[2] = frame.fog_enabled ? 1.0F : 0.0F;

    enter_scene();  // the clearing pass, even if nothing is drawn
    for (const auto &b : batches) {
      if (b.shadow) {
        if (!frame.tron_hidden_line && *b.shadow < frame.shadow_projections->size()) shadow(frame.shadow_projections->at(*b.shadow));
      } else {
        draw_batch(b, frame.tron_hidden_line, false);
      }
    }
    if (frame.tron_hidden_line) {
      if (dev.line_fill)
        for (const auto &b : batches)
          if (!b.shadow) draw_batch(b, true, true);
    } else {
      for (const auto &p : *frame.headlight_projections) headlight(p);
    }
    leave_pass();

    // The finished image to the CPU.
    image_barrier(color, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {uint32_t(width), uint32_t(height), 1};
    f.vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
    VkBufferMemoryBarrier to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = readback;
    to_host.size = VK_WHOLE_SIZE;
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &to_host, 0, nullptr);
    submit_and_wait();
    for (auto &s : staging_to_free) free_host_buffer(s);
    staging_to_free.clear();
    const auto gpu_done = std::chrono::steady_clock::now();
    if (!readback_coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = readback_memory;
      range.size = VK_WHOLE_SIZE;
      check(f.vkInvalidateMappedMemoryRanges(dev.device, 1, &range), "vkInvalidateMappedMemoryRanges");
    }
    if (!SDL_UpdateTexture(wrapped, nullptr, readback_mapped, width * 4)) throw std::runtime_error(std::string("Vulkan scene upload: ") + SDL_GetError());
    const auto finished = std::chrono::steady_clock::now();
    render_ms = std::chrono::duration<double, std::milli>(gpu_done - started).count();
    readback_ms = std::chrono::duration<double, std::milli>(finished - gpu_done).count();
    sum_render += render_ms;
    sum_readback += readback_ms;
    if (++frames % 300 == 0) {
      log("%lu frames: scene %.1f ms (CPU build + GPU wait), readback %.1f ms, %zu textures", frames, sum_render / 300, sum_readback / 300, textures.size());
      sum_render = sum_readback = 0;
    }
    return wrapped;
  }
};

VulkanRenderer::VulkanRenderer() : impl_(std::make_unique<Impl>()) {}
VulkanRenderer::~VulkanRenderer() = default;
bool VulkanRenderer::initialize() {
  if (impl_->ready) return true;
  try {
    if (!impl_->setup()) return false;
  } catch (const std::exception &error) {
    log("initialisation failed: %s", error.what());
    return false;
  }
  log("renderer ready (anisotropy %s, hidden-line outlines %s)", impl_->dev.anisotropy ? "on" : "off", impl_->dev.line_fill ? "on" : "off");
  return true;
}
bool VulkanRenderer::available() const noexcept { return impl_->ready; }
SDL_Texture *VulkanRenderer::render(SDL_Renderer *renderer, const SceneFrameView &frame) { return impl_->render(renderer, frame); }
double VulkanRenderer::last_render_ms() const noexcept { return impl_->render_ms + impl_->readback_ms; }

} // namespace mh::render::hardware
