#pragma once
// PS5: the game's OpenGL scene renderer (opengl_renderer.cpp) on the console's Vulkan driver.
// It renders the scene to an offscreen image on the GPU (presenter's device, ps5_present.cpp) and
// copies the result into an SDL texture, which the game composes with its HUD as before.
#include <renderer/types.hpp>

#include <memory>

struct SDL_Renderer;
struct SDL_Texture;

namespace mh::render::hardware {

class VulkanRenderer {
public:
  VulkanRenderer();
  ~VulkanRenderer();
  VulkanRenderer(const VulkanRenderer &) = delete;
  VulkanRenderer &operator=(const VulkanRenderer &) = delete;

  // False if the console's Vulkan device is not ready or the renderer's resources cannot be made
  // (the reason is logged); the caller then uses the software renderer.
  [[nodiscard]] bool initialize();
  [[nodiscard]] bool available() const noexcept;
  // Throws on a Vulkan error.
  [[nodiscard]] SDL_Texture *render(SDL_Renderer *renderer, const SceneFrameView &frame);
  [[nodiscard]] double last_render_ms() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mh::render::hardware
