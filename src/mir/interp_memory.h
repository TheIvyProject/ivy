#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ivy {
namespace mir {

// Abstract allocation — all memory accesses go through the Memory table
// so that Machine hooks can intercept them (inspired by Miri).
struct Allocation {
    uint32_t id = 0;
    std::vector<std::byte> bytes;
    std::vector<bool> initMask;   // per-byte init tracking
    bool isLive = true;           // use-after-free detection
    bool isHeap = false;          // new vs alloca
};

// Memory = table of allocations. Every pointer dereference goes through
// this table, allowing the Machine hook to validate safety.
class Memory {
public:
    uint32_t allocate(std::size_t size, bool isHeap = false);
    void deallocate(uint32_t id);  // mark dead (not erase)
    bool isLive(uint32_t id) const;
    bool isInitialized(uint32_t id, std::size_t offset) const;
    void writeByte(uint32_t id, std::size_t offset, std::byte val);
    std::byte readByte(uint32_t id, std::size_t offset);

private:
    std::unordered_map<uint32_t, Allocation> allocs_;
    uint32_t nextId_ = 1;
};

}  // namespace mir
}  // namespace ivy
