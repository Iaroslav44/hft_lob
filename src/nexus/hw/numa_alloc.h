#pragma once
#include <cstddef>
#include <stdexcept>

#if defined(__linux__)
#include <numa.h>
#include <numaif.h>
#endif

namespace nexus::hw {

inline void* allocate_on_node(std::size_t size, int node) {
#if defined(__linux__)
    if (numa_available() < 0) {
        return ::operator new(size); // fallback
    }
    void* ptr = numa_alloc_onnode(size, node);
    if (!ptr) throw std::bad_alloc();
    return ptr;
#else
    (void)node;
    return ::operator new(size);
#endif
}

inline void free_on_node(void* ptr, std::size_t size) {
#if defined(__linux__)
    if (numa_available() < 0) {
        ::operator delete(ptr);
        return;
    }
    numa_free(ptr, size);
#else
    (void)size;
    ::operator delete(ptr);
#endif
}

} // namespace nexus::hw
