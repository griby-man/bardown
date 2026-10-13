#pragma once

// the native renderer's frame: drawn on the runtime's own device, so the window, the overlays and the frame
// captures all keep working on top of it

#include <cstdint>

namespace rex {
class Runtime;
}

namespace bardown::native {

struct Image;

// the Xbox's picture; the native one is this times the resolution scale
inline constexpr uint32_t kGuestWidth = 1280, kGuestHeight = 720;

bool RenderInit(rex::Runtime* runtime, uint32_t scale);
uint32_t RenderScale();
// at the game's swap: submit the frame and show this image (left readable by shaders by the scene)
void RenderPresent(Image* picture);

// from the draw side: the image to show for the game's front buffer, and how many draws went through
Image* SwapImage(uint32_t front_texture_physical);
void DrawStats(uint64_t& draws, uint64_t& skipped);

}  // namespace bardown::native
