#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <queue>
#include <type_traits>
#include <utility>

using Result = std::pair<double, uint32_t>;
using ResultHeap = std::priority_queue<Result>;

namespace detail {
constexpr double sqrt_newton(double x, double cur, double prev) {
    return cur == prev ? cur : sqrt_newton(x, 0.5 * (cur + x / cur), cur);
}
} // namespace detail

constexpr double const_sqrt(double x) {
    return x >= 0 && x < std::numeric_limits<double>::infinity()
               ? detail::sqrt_newton(x, x, 0)
               : std::numeric_limits<double>::quiet_NaN();
}

inline uint32_t reverse_bits(uint32_t n) {
    n = (n >> 1) & 0x55555555u | (n << 1) & 0xaaaaaaaau;
    n = (n >> 2) & 0x33333333u | (n << 2) & 0xccccccccu;
    n = (n >> 4) & 0x0f0f0f0fu | (n << 4) & 0xf0f0f0f0u;
    n = (n >> 8) & 0x00ff00ffu | (n << 8) & 0xff00ff00u;
    n = (n >> 16) & 0x0000ffffu | (n << 16) & 0xffff0000u;
    return n;
}

template <typename T, std::size_t Alignment>
struct AlignedAllocator {
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using propagate_on_container_copy_assignment = std::true_type;
    using is_always_equal = std::true_type;

    template <class U>
    struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };

    AlignedAllocator() noexcept = default;
    template <class U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        void* ptr = nullptr;
        const std::size_t bytes = n * sizeof(T);
        if (posix_memalign(&ptr, Alignment, bytes) != 0) {
            throw std::bad_alloc();
        }
        return reinterpret_cast<T*>(ptr);
    }

    void deallocate(T* p, std::size_t) noexcept {
        std::free(p);
    }
};

template <typename T, typename U, std::size_t Alignment>
inline bool operator==(const AlignedAllocator<T, Alignment>&,
                       const AlignedAllocator<U, Alignment>&) noexcept {
    return true;
}

template <typename T, typename U, std::size_t Alignment>
inline bool operator!=(const AlignedAllocator<T, Alignment>&,
                       const AlignedAllocator<U, Alignment>&) noexcept {
    return false;
}
