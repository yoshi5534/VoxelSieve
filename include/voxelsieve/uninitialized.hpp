#pragma once

// Buffers that are written in full right after they are made, such as the voxels of a brick or
// the feature maps of a learned model, need not be zeroed first.

#include <memory>
#include <utility>
#include <vector>

namespace voxelsieve {

/// Allocator that leaves values uninitialised where a vector would set them to zero. Feature maps
/// and brick buffers are hundreds of MB that are overwritten anyway, often in parallel: zeroing
/// them first would take one thread through all of that memory.
template <class T>
struct UninitializedAllocator : std::allocator<T> {
  template <class U>
  struct rebind {  // NOLINT(readability-identifier-naming): the allocator interface
    using other = UninitializedAllocator<U>;
  };
  UninitializedAllocator() = default;
  template <class U>
  explicit UninitializedAllocator(const UninitializedAllocator<U>& /*other*/) noexcept {}
  template <class U>
  void construct(U* p) noexcept {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

/// A vector whose new elements are uninitialised.
template <class T>
using UninitializedVector = std::vector<T, UninitializedAllocator<T>>;

}  // namespace voxelsieve
