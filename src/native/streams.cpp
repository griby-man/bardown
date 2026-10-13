#include "streams.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/logging.h>

namespace bardown::native {

namespace {

constexpr VkDeviceSize kChunk = VkDeviceSize(64) << 20;
// changed this many times and it's treated as per-frame data from then on
constexpr uint32_t kDynamicAfter = 3;

struct Chunk {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* host = nullptr;
  VkDeviceAddress address = 0;
  std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
};

struct Entry {
  size_t chunk = 0;
  VkDeviceSize offset = 0, size = 0;
  uint64_t hash = 0;     // of the guest data it was converted from
  uint64_t checked = 0;  // the frame it was last hashed in
};

struct Freed {
  uint64_t frame = 0;
  size_t chunk = 0;
  VkDeviceSize offset = 0, size = 0;
};

struct Cache {
  std::mutex mutex;
  std::vector<Chunk> chunks;
  std::unordered_map<uint64_t, Entry> entries;
  std::unordered_map<uint32_t, uint32_t> changes;  // by guest address
  std::vector<Freed> freed;                        // waiting out the frames in flight
  uint64_t collected = 0;
  uint64_t changed = 0;
};
Cache c;

bool AddChunk() {
  auto& vk = Api();
  Chunk ch;
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = kChunk;
  info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (!Check(vk.vkCreateBuffer(vk.device, &info, nullptr, &ch.buffer), "stream buffer")) return false;
  VkMemoryRequirements req;
  vk.vkGetBufferMemoryRequirements(vk.device, ch.buffer, &req);
  VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = &flags;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex =
      MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (!Check(vk.vkAllocateMemory(vk.device, &alloc, nullptr, &ch.memory), "stream memory") ||
      !Check(vk.vkBindBufferMemory(vk.device, ch.buffer, ch.memory, 0), "stream bind") ||
      !Check(vk.vkMapMemory(vk.device, ch.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&ch.host)),
             "stream map"))
    return false;
  VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = ch.buffer;
  ch.address = vk.vkGetBufferDeviceAddress(vk.device, &address);
  ch.free.emplace(0, kChunk);
  c.chunks.push_back(std::move(ch));
  return true;
}

// first fit, 256-byte aligned
bool Take(VkDeviceSize size, size_t& chunk, VkDeviceSize& offset) {
  size = (size + 255) & ~VkDeviceSize(255);
  if (size > kChunk) return false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (size_t i = 0; i < c.chunks.size(); ++i) {
      auto& free = c.chunks[i].free;
      for (auto it = free.begin(); it != free.end(); ++it) {
        if (it->second < size) continue;
        chunk = i;
        offset = it->first;
        const VkDeviceSize rest = it->second - size;
        free.erase(it);
        if (rest) free.emplace(offset + size, rest);
        return true;
      }
    }
    // 1 GB of meshes is more than a frame of this game holds; past that, fall back to per-frame copies
    if (c.chunks.size() >= 16 || !AddChunk()) return false;
  }
  return false;
}

void Give(size_t chunk, VkDeviceSize offset, VkDeviceSize size) {
  size = (size + 255) & ~VkDeviceSize(255);
  auto& free = c.chunks[chunk].free;
  auto it = free.emplace(offset, size).first;
  if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first) {
    it->second += next->second;
    free.erase(next);
  }
  if (it != free.begin()) {
    auto prev = std::prev(it);
    if (prev->first + prev->second == it->first) {
      prev->second += it->second;
      free.erase(it);
    }
  }
}

// three frames, like the upload space: a stream dropped now may still be read by frames in flight
void Collect(uint64_t frame) {
  if (c.collected == frame) return;
  c.collected = frame;
  auto keep = std::stable_partition(c.freed.begin(), c.freed.end(), [&](const Freed& f) { return f.frame + 3 >= frame; });
  for (auto it = keep; it != c.freed.end(); ++it) Give(it->chunk, it->offset, it->size);
  c.freed.erase(keep, c.freed.end());
}

Upload At(const Entry& e) {
  const Chunk& ch = c.chunks[e.chunk];
  return {ch.host + e.offset, ch.buffer, e.offset, ch.address + e.offset};
}

}  // namespace

std::unique_lock<std::mutex> StreamLock() { return std::unique_lock(c.mutex); }

bool FindStream(uint64_t key, uint32_t address, const uint8_t* guest, size_t guest_bytes, size_t size, Upload& out,
                uint8_t*& fill) {
  fill = nullptr;
  const uint64_t frame = FrameNumber();
  Collect(frame);
  auto it = c.entries.find(key);
  if (it != c.entries.end() && it->second.checked == frame) {
    out = At(it->second);
    return true;
  }
  auto dynamic = [&] {
    auto ch = c.changes.find(address);
    return ch != c.changes.end() && ch->second >= kDynamicAfter;
  };
  if (it == c.entries.end() && dynamic()) return false;
  // the game can rewrite vertex data whenever it likes, so it's checked once a frame
  // rather than trusted until a write is noticed
  const uint64_t hash = XXH3_64bits(guest, guest_bytes);
  if (it != c.entries.end()) {
    if (it->second.hash == hash) {
      it->second.checked = frame;
      out = At(it->second);
      return true;
    }
    ++c.changed;
    ++c.changes[address];
    c.freed.push_back({frame, it->second.chunk, it->second.offset, it->second.size});
    c.entries.erase(it);
  }
  if (dynamic()) return false;
  size_t chunk;
  VkDeviceSize offset;
  if (!Take(size, chunk, offset)) return false;
  const Entry& e = c.entries.emplace(key, Entry{chunk, offset, size, hash, frame}).first->second;
  out = At(e);
  fill = out.host;
  return true;
}

uint64_t TakeStreamChanges() {
  std::lock_guard lock(c.mutex);
  const uint64_t changed = c.changed;
  c.changed = 0;
  return changed;
}

}  // namespace bardown::native
