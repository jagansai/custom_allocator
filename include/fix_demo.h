#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "string_pool.h"

namespace fix_demo {

// Standard FIX field separator (Start of Heading, ASCII 0x01).
constexpr char SOH = '\x01';

template <typename T>
class ArenaOwned;

// Forward declaration for the generic object pool RAII handle.
template <typename T>
class Pooled;

// Round "value" up to the next multiple of "alignment" (power of two).
inline size_t alignUp(size_t value, size_t alignment) {
  const size_t mask = alignment - 1;
  return (value + mask) & ~mask;
}

// Simple bump-pointer arena that owns a contiguous buffer and
// hands out memory sequentially. Individual objects are not freed;
// instead the caller can reset the entire arena or reset back to
// a previous "used" mark via ArenaScope.
class ArenaAllocator {
public:
  explicit ArenaAllocator(size_t capacity)
      : buffer_(static_cast<char*>(::operator new(capacity))),
        capacity_(capacity) {}

  ~ArenaAllocator() { ::operator delete(buffer_); }

  ArenaAllocator(const ArenaAllocator&) = delete;
  ArenaAllocator& operator=(const ArenaAllocator&) = delete;

  // Discard all allocations made so far.
  void reset() noexcept { offset_ = 0; }

  // Reset the arena back to a previously observed "used()" value.
  void resetTo(size_t used) noexcept {
    if (used <= capacity_) {
      offset_ = used;
    }
  }

public:
  // Allocate and construct a single T from the arena.
  template <typename T, typename... Args>
  T* create(Args&&... args) {
    void* raw = allocate(sizeof(T), alignof(T));
    return new (raw) T(std::forward<Args>(args)...);
  }

  // Allocate and construct T, returning an ArenaOwned wrapper that
  // will run the destructor when reset or destroyed.
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

// RAII helper that remembers the arena's current "used()" value and
// resets back to that mark when the scope ends, effectively rolling
// back all allocations performed within the scope.
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

// Lightweight owning wrapper for an object constructed in an
// ArenaAllocator. It is responsible only for running the object's
// destructor; the underlying memory is managed by the arena.
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

  // Generic object pool for reusing instances of T. Objects are
  // constructed in-place inside the pool and returned to the pool
  // when the corresponding Pooled<T> handle is destroyed or reset.
  template <typename T>
  class ObjectPool {
   public:
    ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Acquire a pooled object, constructing it with the given
    // arguments if necessary. The returned Pooled<T> will
    // automatically return the object to the pool on destruction.
    template <typename... Args>
    Pooled<T> acquire(Args&&... args) {
      size_t index;
      if (!freeList_.empty()) {
        index = freeList_.back();
        freeList_.pop_back();
      } else {
        index = slots_.size();
        slots_.emplace_back();
      }

      Slot& slot = slots_[index];
      T* ptr = new (slot.storage) T(std::forward<Args>(args)...);
      slot.inUse = true;
      return Pooled<T>(this, index, ptr);
    }

   private:
    friend class Pooled<T>;

    struct Slot {
      alignas(T) unsigned char storage[sizeof(T)];
      bool inUse = false;
    };

    void release(size_t index) noexcept {
      Slot& slot = slots_[index];
      if (slot.inUse) {
        auto* ptr = reinterpret_cast<T*>(slot.storage);
        ptr->~T();
        slot.inUse = false;
        freeList_.push_back(index);
      }
    }

    std::vector<Slot> slots_;
    std::vector<size_t> freeList_;
  };

  // RAII handle for an object allocated from ObjectPool<T>. Destroying
  // or resetting the handle runs the object's destructor and returns
  // the storage back to the pool for reuse.
  template <typename T>
  class Pooled {
   public:
    Pooled() noexcept = default;

    Pooled(const Pooled&) = delete;
    Pooled& operator=(const Pooled&) = delete;

    Pooled(Pooled&& other) noexcept { moveFrom(std::move(other)); }

    Pooled& operator=(Pooled&& other) noexcept {
      if (this != &other) {
        reset();
        moveFrom(std::move(other));
      }
      return *this;
    }

    ~Pooled() { reset(); }

    T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // Release ownership and return the raw pointer without
    // returning it to the pool. The caller becomes responsible
    // for destroying the object.
    T* release() noexcept {
      T* tmp = ptr_;
      pool_ = nullptr;
      index_ = 0;
      ptr_ = nullptr;
      return tmp;
    }

    // Destroy the pooled object (if any) and return its storage
    // to the underlying ObjectPool.
    void reset() noexcept {
      if (pool_ && ptr_) {
        pool_->release(index_);
      }
      pool_ = nullptr;
      ptr_ = nullptr;
      index_ = 0;
    }

   private:
    friend class ObjectPool<T>;

    Pooled(ObjectPool<T>* pool, size_t index, T* ptr) noexcept
        : pool_(pool), index_(index), ptr_(ptr) {}

    void moveFrom(Pooled&& other) noexcept {
      pool_ = other.pool_;
      index_ = other.index_;
      ptr_ = other.ptr_;
      other.pool_ = nullptr;
      other.ptr_ = nullptr;
      other.index_ = 0;
    }

    ObjectPool<T>* pool_ = nullptr;
    size_t index_ = 0;
    T* ptr_ = nullptr;
  };

enum class Side : char { Buy = '1', Sell = '2' };

// Simplified in-memory representation of a FIX NewOrderSingle.
// Many of the string fields are parsed from FIX tags for the
// purposes of exercising allocation and parsing.
struct NewOrderSingle {
  std::string clOrdID;
  std::string symbol;
  Side side;
  int quantity = 0;
  double price = 0.0;
  std::string account;
  std::string orderType;
  std::string timeInForce;
  std::string transactTime;
  std::string trader;
  std::string firm;
  std::string text;
  std::string securityDesc;
  std::string currency;
  std::string arrivalTime;  // optional arrival timestamp for logging
};

inline std::string_view extractTagView(std::string_view message,
                                       std::string_view tag) {
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
  return message.substr(value_begin, value_end - value_begin);
}

inline std::string extractTagValue(std::string_view message,
                                   std::string_view tag) {
  const auto view = extractTagView(message, tag);
  if (view.empty()) {
    return {};
  }
  return std::string(view);
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

  // Additional optional fields to make the message larger
  order.account = extractTagValue(message, "1");
  order.orderType = extractTagValue(message, "40");
  order.timeInForce = extractTagValue(message, "59");
  order.transactTime = extractTagValue(message, "60");
  order.trader = extractTagValue(message, "448");
  order.firm = extractTagValue(message, "452");
  order.text = extractTagValue(message, "58");
  order.securityDesc = extractTagValue(message, "107");
  order.currency = extractTagValue(message, "15");
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

// Variant of NewOrderSingle whose string fields are backed by a
// StringPool and stored as std::string_view to avoid per-field
// heap allocations.
struct PooledNewOrderSingle {
  std::string_view clOrdID;
  std::string_view symbol;
  Side side;
  int quantity = 0;
  double price = 0.0;
  std::string_view account;
  std::string_view orderType;
  std::string_view timeInForce;
  std::string_view transactTime;
  std::string_view trader;
  std::string_view firm;
  std::string_view text;
  std::string_view securityDesc;
  std::string_view currency;
  std::string arrivalTime;  // kept as std::string for convenience
};

inline void populateNewOrder(PooledNewOrderSingle& order,
                             std::string_view message,
                             StringPool& pool) {
  order.clOrdID = pool.intern(extractTagView(message, "11"));
  order.symbol = pool.intern(extractTagView(message, "48"));

  const auto sideView = extractTagView(message, "54");
  if (!sideView.empty()) {
    order.side = Side(sideView.front());
  }

  const auto qtyView = extractTagView(message, "38");
  if (!qtyView.empty()) {
    order.quantity = std::stoi(std::string(qtyView));
  }
  const auto pxView = extractTagView(message, "44");
  if (!pxView.empty()) {
    order.price = std::stod(std::string(pxView));
  }

  order.account = pool.intern(extractTagView(message, "1"));
  order.orderType = pool.intern(extractTagView(message, "40"));
  order.timeInForce = pool.intern(extractTagView(message, "59"));
  order.transactTime = pool.intern(extractTagView(message, "60"));
  order.trader = pool.intern(extractTagView(message, "448"));
  order.firm = pool.intern(extractTagView(message, "452"));
  order.text = pool.intern(extractTagView(message, "58"));
  order.securityDesc = pool.intern(extractTagView(message, "107"));
  order.currency = pool.intern(extractTagView(message, "15"));
}

template <typename Order>
inline std::string toJsonImpl(const Order& order) {
  // Single-line JSON object suitable for line-delimited logs.
  std::string json;
  json.reserve(256);

  json += '{';
  json += "\"clOrdID\": \"";
  json.append(std::string_view(order.clOrdID));
  json += "\",";

  json += " \"symbol\": \"";
  json.append(std::string_view(order.symbol));
  json += "\",";

  json += " \"side\": \"";
  json.push_back(static_cast<char>(order.side));
  json += "\",";

  json += " \"quantity\": ";
  json += std::to_string(order.quantity);
  json += ",";

  json += " \"price\": ";
  json += std::to_string(order.price);
  json += ",";

  json += " \"account\": \"";
  json.append(std::string_view(order.account));
  json += "\",";

  json += " \"orderType\": \"";
  json.append(std::string_view(order.orderType));
  json += "\",";

  json += " \"timeInForce\": \"";
  json.append(std::string_view(order.timeInForce));
  json += "\",";

  json += " \"transactTime\": \"";
  json.append(std::string_view(order.transactTime));
  json += "\",";

  json += " \"trader\": \"";
  json.append(std::string_view(order.trader));
  json += "\",";

  json += " \"firm\": \"";
  json.append(std::string_view(order.firm));
  json += "\",";

  json += " \"text\": \"";
  json.append(std::string_view(order.text));
  json += "\",";

  json += " \"securityDesc\": \"";
  json.append(std::string_view(order.securityDesc));
  json += "\",";

  json += " \"currency\": \"";
  json.append(std::string_view(order.currency));
  json += "\",";

  json += " \"arrivalTime\": \"";
  json.append(std::string_view(order.arrivalTime));
  json += "\"";

  json += '}';
  return json;
}

inline std::string toJson(const NewOrderSingle& order) {
  return toJsonImpl(order);
}

inline std::string toJson(const PooledNewOrderSingle& order) {
  return toJsonImpl(order);
}

}  // namespace fix_demo
