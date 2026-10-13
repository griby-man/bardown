#pragma once

// vertex streams that stay put: converted once and kept while the game's data is unchanged, instead of being
// converted into the frame's upload space every frame

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "vk.h"

namespace bardown::native {

// the cache's lock: held across FindStream and filling a new entry, so no other thread sees it half made
std::unique_lock<std::mutex> StreamLock();

// with the lock held. true: `out` is the converted stream; if `fill` came back set, it's new and the caller
// converts `size` bytes into it. false: the game keeps changing this one, convert it into the frame's upload
// space instead. guest/guest_bytes is the game's data, hashed to tell whether it changed
bool FindStream(uint64_t key, uint32_t address, const uint8_t* guest, size_t guest_bytes, size_t size, Upload& out,
                uint8_t*& fill);

// streams found changed since the last call
uint64_t TakeStreamChanges();

}  // namespace bardown::native
