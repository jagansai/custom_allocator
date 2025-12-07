#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace fix_demo {

// Simple append-only string pool that allocates large blocks and
// returns std::string_view pointing into stable storage. Memory
// is reclaimed only when the pool is destroyed or cleared.
class StringPool {
public:
  explicit StringPool(std::size_t blockSize = 64 * 1024)
      : blockSize_(blockSize) {}

  StringPool(const StringPool&) = delete;
  StringPool& operator=(const StringPool&) = delete;

  // Copy the given string_view into the pool and return a view
  // into the pooled storage. Empty inputs return an empty view
  // without allocating.
  std::string_view intern(std::string_view value) {
    if (value.empty()) {
      return {};
    }
    char* dst = allocate(value.size());
    std::memcpy(dst, value.data(), value.size());
    return std::string_view(dst, value.size());
  }

  // Drop all currently allocated blocks.
  void clear() noexcept { blocks_.clear(); }

private:
  struct Block {
    std::unique_ptr<char[]> data;
    std::size_t capacity = 0;
    std::size_t used = 0;
  };

  char* allocate(std::size_t n) {
    if (blocks_.empty() || blocks_.back().used + n > blocks_.back().capacity) {
      const std::size_t capacity = n > blockSize_ ? n : blockSize_;
      Block block;
      block.data = std::make_unique<char[]>(capacity);
      block.capacity = capacity;
      block.used = 0;
      blocks_.push_back(std::move(block));
    }

    Block& block = blocks_.back();
    char* ptr = block.data.get() + block.used;
    block.used += n;
    return ptr;
  }

  std::size_t blockSize_;
  std::vector<Block> blocks_;
};

}  // namespace fix_demo
