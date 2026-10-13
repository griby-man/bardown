#pragma once

// what the native renderer's pieces need from its setup in native.cpp

#include <cstddef>
#include <cstdint>

namespace bardown::native {

// the translated shader made from a guest shader object, by its hash; 0 when it isn't known
uint64_t GuestShaderHash(uint32_t guest_shader);
bool ShaderSpirv(uint64_t hash, const uint32_t*& code, size_t& bytes, uint32_t& spec);
// one of the game's team-colour composition shaders (drawn at load; the CPU reads the result back)
bool CompositionShader(uint64_t hash);

// true once the shaders are in and the renderer can draw
bool Drawing();
// the BARDOWN_NATIVE_PROBE log of one draw, when asked for
void ProbeDraw(const uint8_t* base, uint32_t device, const char* kind, uint32_t prim, uint32_t a, uint32_t b, uint32_t c);
uint32_t DeviceType(uint32_t device);

}  // namespace bardown::native
