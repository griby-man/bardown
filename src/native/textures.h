#pragma once

// the game's textures as native images, found by the fetch constant the game binds

#include <cstdint>

#include "vk.h"

namespace rex::memory {
class Memory;
}

namespace bardown::native {

// watches guest memory for CPU writes to textures already loaded
void TexturesInit(rex::memory::Memory* memory);
// logs every bind while on (BARDOWN_NATIVE_TRACE)
void TextureTrace(bool on);
// textures loaded from guest memory so far
uint64_t TextureLoads();
// time spent loading textures since the last call
double TakeTextureLoadTime();

struct TextureBinding {
  Heap heap = Heap::k2D;
  uint32_t index = 0;    // into that table; 0 = the stand-in
  uint32_t sampler = 0;  // into the sampler table
  uint64_t late = 0;     // a late texture's key, to read it again when the draw is replayed
};

// fetch = the six fetch-constant dwords, already in host order. vertex: a vertex shader reads it (bone
// matrices, rewritten every frame), so it's always checked against memory rather than trusted to the write watch
TextureBinding BindTexture(const uint32_t fetch[6], bool vertex);
// nearest, clamped, base level only: what vertex shaders read their texel-addressed data (bone matrices) with
uint32_t PointSampler();

// as a draw is replayed: a texture the game rewrites every frame is read again, once a frame, and uploaded ahead
// of the frame's draws. not at the swap after the bind: the workers record a frame ahead, so the game may not
// have written the data yet by then
void RefreshLate(uint64_t key);
// late textures found changed at the swap since the last call
uint64_t TakeLateChanges();
// textures that changed in guest memory without a write notice (BARDOWN_NATIVE_VERIFY), logged
uint64_t CheckStale();

// a texture the game writes with the CPU, by the address of its data: the native copy is dropped
void TextureWritten(uint32_t physical_address);

// a render target copied into a texture by a resolve: later binds of that address use this image
// dest_swizzle: the destination header's swizzle, the channel order the 360 wrote it in
Image* ResolveTexture(uint32_t physical_address, uint32_t width, uint32_t height, VkFormat format,
                      const uint32_t dest_words[6]);
// the image a resolve left at that address, if any
Image* ResolvedImage(uint32_t physical_address);

}  // namespace bardown::native
