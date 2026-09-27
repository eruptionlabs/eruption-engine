#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>

namespace eruption {

// Simple STL-compatible allocator that guarantees a minimum alignment.
template <typename T, std::size_t Alignment>
struct AlignedAllocator {
    static_assert(Alignment >= alignof(T), "Alignment must be at least alignof(T)");
    using value_type = T;

    AlignedAllocator() = default;
    template <typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) {}

    T* allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T)) throw std::bad_alloc();
        std::size_t bytes = n * sizeof(T);
        void* ptr = std::aligned_alloc(Alignment, bytes);
        if (!ptr) throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, std::size_t) noexcept {
        std::free(ptr);
    }

    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new(static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <typename U>
    void destroy(U* p) {
        p->~U();
    }

    template <typename U>
    struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };
};

template <typename T, std::size_t A, typename U, std::size_t B>
bool operator==(const AlignedAllocator<T, A>&, const AlignedAllocator<U, B>&) {
    return A == B;
}

template <typename T, std::size_t A, typename U, std::size_t B>
bool operator!=(const AlignedAllocator<T, A>&, const AlignedAllocator<U, B>&) {
    return A != B;
}

} // namespace eruption
