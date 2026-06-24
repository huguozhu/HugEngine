#pragma once

#include "Core/Types.h"

#include <cstdlib>

// ============================================================
// Memory allocator interface
// ============================================================

namespace he {

// Base allocator interface
class IAllocator {
public:
    virtual ~IAllocator() = default;

    virtual void* Allocate(usize size, usize alignment = 16) = 0;
    virtual void  Deallocate(void* ptr, usize size = 0) = 0;
    virtual usize GetAllocationSize(void* ptr) const { return 0; }

    // Utility
    template<typename T, typename... Args>
    T* New(Args&&... args) {
        void* mem = Allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    template<typename T>
    void Delete(T* ptr) {
        if (ptr) {
            ptr->~T();
            Deallocate(ptr, sizeof(T));
        }
    }
};

// Default C malloc/free allocator
class MallocAllocator : public IAllocator {
public:
    static MallocAllocator& Instance() {
        static MallocAllocator s_Instance;
        return s_Instance;
    }

    void* Allocate(usize size, usize alignment = 16) override {
        return std::aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1));
    }

    void Deallocate(void* ptr, usize /*size*/ = 0) override {
        std::free(ptr);
    }
};

// Engine default allocator
using DefaultAllocator = MallocAllocator;

// Global allocator access
inline IAllocator& GetAllocator() {
    return MallocAllocator::Instance();
}

} // namespace he

// --- New/delete overrides using engine allocator ---
#define HE_NEW(T, ...)  ::he::GetAllocator().New<T>(__VA_ARGS__)
#define HE_DELETE(ptr)  ::he::GetAllocator().Delete(ptr)
