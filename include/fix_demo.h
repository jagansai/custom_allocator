#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace fix_demo {

constexpr char SOH = '\x01';

template <typename T>
class ArenaOwned;

inline size_t alignUp(size_t value, size_t alignment) {
  const size_t mask = alignment - 1;
  return (value + mask) & ~mask;
}

class ArenaAllocator {
public:
  explicit ArenaAllocator(size_t capacity)
      : buffer_(static_cast<char*>(::operator new(capacity))),
        capacity_(capacity) {}

  ~ArenaAllocator() { ::operator delete(buffer_); }

  ArenaAllocator(const ArenaAllocator&) = delete;
  ArenaAllocator& operator=(const ArenaAllocator&) = delete;

  void reset() noexcept { offset_ = 0; }

  void resetTo(size_t used) noexcept {
    if (used <= capacity_) {
      offset_ = used;
    }
  }

public:
  template <typename T, typename... Args>
  T* create(Args&&... args) {
    void* raw = allocate(sizeof(T), alignof(T));
    return new (raw) T(std::forward<Args>(args)...);
  }

  template <typename T, typename... Args>
  ArenaOwned<T> createOwned(Args&&... args) {
    return ArenaOwned<T>(create<T>(std::forward<Args>(args)...));
  }

  size_t used() const noexcept { return offset_; }

private:
  void* allocate(size_t bytes, size_t alignment = alignof(std::max_align_t)) {
    const size_t aligned = alignUp(offset_, alignment);
    if (aligned + bytes > capacity_) {
      throw std::bad_alloc{};
    }
    void* ptr = buffer_ + aligned;
    offset_ = aligned + bytes;
    return ptr;
  }



private:
  char* buffer_;
  size_t capacity_;
  size_t offset_ = 0;
};

class ArenaScope {
 public:
  explicit ArenaScope(ArenaAllocator& arena)
      : arena_(&arena), savedMark_(arena.used()) {}
  ArenaScope(const ArenaScope&) = delete;
  ArenaScope& operator=(const ArenaScope&) = delete;
  ArenaScope(ArenaScope&& other) noexcept
      : arena_(other.arena_), savedMark_(other.savedMark_), active_(other.active_) {
    other.active_ = false;
  }
  ArenaScope& operator=(ArenaScope&& other) noexcept {
    if (this != &other) {
      if (active_ && arena_) {
        arena_->resetTo(savedMark_);
      }
      arena_ = other.arena_;
      savedMark_ = other.savedMark_;
      active_ = other.active_;
      other.active_ = false;
    }
    return *this;
  }
  ~ArenaScope() {
    if (active_ && arena_) {
      arena_->resetTo(savedMark_);
    }
  }
  void release() noexcept { active_ = false; }

 private:
  ArenaAllocator* arena_;
  size_t savedMark_;
  bool active_ = true;
};

template <typename T>
class ArenaOwned {
 public:
  explicit ArenaOwned(T* ptr = nullptr) : ptr_(ptr) {}
  ArenaOwned(const ArenaOwned&) = delete;
  ArenaOwned& operator=(const ArenaOwned&) = delete;
  ArenaOwned(ArenaOwned&& other) noexcept : ptr_(other.release()) {}
  ArenaOwned& operator=(ArenaOwned&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.release();
    }
    return *this;
  }
  ~ArenaOwned() { reset(); }

  void reset(T* ptr = nullptr) {
    if (ptr_) {
      ptr_->~T();
    }
    ptr_ = ptr;
  }

  T* release() noexcept {
    T* tmp = ptr_;
    ptr_ = nullptr;
    return tmp;
  }

  T* get() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  T* operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

 private:
  T* ptr_;
};

enum class Side : char { Buy = '1', Sell = '2' };

struct NewOrderSingle {
  std::string clOrdID;
  std::string symbol;
  Side side;
  int quantity = 0;
  double price = 0.0;
  std::string arrivalTime;  // optional arrival timestamp for logging
};

inline std::string extractTagValue(std::string_view message, std::string_view tag) {
  std::string pattern;
  pattern.reserve(tag.size() + 1);
  pattern += tag;
  pattern += '=';
  const size_t start = message.find(pattern);
  if (start == std::string_view::npos) {
    return {};
  }
  const size_t value_begin = start + pattern.size();
  size_t value_end = message.find(SOH, value_begin);
  if (value_end == std::string_view::npos) {
    value_end = message.size();
  }
  return std::string(message.substr(value_begin, value_end - value_begin));
}

inline void populateNewOrder(NewOrderSingle& order, std::string_view message) {
  order.clOrdID = extractTagValue(message, "11");
  order.symbol = extractTagValue(message, "48");
  const std::string side = extractTagValue(message, "54");
  if (!side.empty()) {
    order.side = Side(side.front());
  }
  const std::string qty = extractTagValue(message, "38");
  if (!qty.empty()) {
    order.quantity = std::stoi(qty);
  }
  const std::string px = extractTagValue(message, "44");
  if (!px.empty()) {
    order.price = std::stod(px);
  }
}

inline std::unique_ptr<NewOrderSingle> createOrderOnHeap(std::string_view message) {
  auto order = std::make_unique<NewOrderSingle>();
  populateNewOrder(*order, message);
  return order;
}

inline ArenaOwned<NewOrderSingle> createOrderOnArena(ArenaAllocator& arena, std::string_view message) {
  auto order = arena.createOwned<NewOrderSingle>();
  populateNewOrder(*order, message);
  return order;
}

inline std::string toJson(const NewOrderSingle& order) {
  // Single-line JSON object suitable for line-delimited logs.
  std::string json = "{";
  json += "\"clOrdID\": \"" + order.clOrdID + "\",";
  json += " \"symbol\": \"" + order.symbol + "\",";
  json += " \"side\": \"" + std::string(1, static_cast<char>(order.side)) + "\",";
  json += " \"quantity\": " + std::to_string(order.quantity) + ",";
  json += " \"price\": " + std::to_string(order.price) + ",";
  json += " \"arrivalTime\": \"" + order.arrivalTime + "\"";
  json += "}";
  return json;
}

}  // namespace fix_demo
