#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory_resource>

namespace omsi::spline {

/// Thread-local arena allocator for zero-allocation spline mesh generation.
/// Pre-allocates a continuous block of memory to eliminate heap fragmentation
/// and allocation overhead across parallel job system workers.
class SplineMeshArena {
public:
    explicit SplineMeshArena(std::size_t capacity_bytes = 16 * 1024 * 1024)
        : buffer_(capacity_bytes), offset_(0) {}

    ~SplineMeshArena() = default;

    SplineMeshArena(const SplineMeshArena&) = delete;
    SplineMeshArena& operator=(const SplineMeshArena&) = delete;
    SplineMeshArena(SplineMeshArena&&) noexcept = default;
    SplineMeshArena& operator=(SplineMeshArena&&) noexcept = default;

    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        const std::size_t current_address = reinterpret_cast<std::size_t>(buffer_.data() + offset_);
        const std::size_t aligned_address = (current_address + alignment - 1) & ~(alignment - 1);
        const std::size_t padding = aligned_address - current_address;
        const std::size_t total_needed = bytes + padding;

        if (offset_ + total_needed > buffer_.size()) {
            buffer_.resize(buffer_.size() + std::max(bytes * 2, std::size_t(2 * 1024 * 1024)));
        }

        void* ptr = buffer_.data() + offset_ + padding;
        offset_ += total_needed;
        return ptr;
    }

    void reset() {
        offset_ = 0;
    }

    [[nodiscard]] std::size_t allocated_bytes() const { return offset_; }
    [[nodiscard]] std::size_t capacity_bytes() const { return buffer_.size(); }

private:
    std::vector<std::byte> buffer_;
    std::size_t offset_ = 0;
};

// Access thread-local arena for parallel job workers
[[nodiscard]] SplineMeshArena& get_thread_spline_arena();

// Stateless-compatible allocator wrapping SplineMeshArena
template <typename T>
class ArenaAllocator {
public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    ArenaAllocator() noexcept = default;

    template <typename U>
    constexpr ArenaAllocator(const ArenaAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n == 0) return nullptr;
        return static_cast<T*>(get_thread_spline_arena().allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T*, std::size_t) noexcept {
        // Bump allocator: individual deallocations are no-ops.
    }

    template <typename U>
    [[nodiscard]] bool operator==(const ArenaAllocator<U>&) const noexcept { return true; }

    template <typename U>
    [[nodiscard]] bool operator!=(const ArenaAllocator<U>&) const noexcept { return false; }
};

template <typename T>
using ArenaVector = std::vector<T, ArenaAllocator<T>>;

} // namespace omsi::spline
