#pragma once

#include "Core/Types.h"

#include <string_view>
#include <vector>
#include <unordered_map>

// ============================================================
// Runtime type information — bridges C++26 compile-time
// reflection to runtime type queries
// ============================================================

namespace he::reflect {

// Forward: will be replaced by std::meta::info in C++26
// For now, we define the runtime representation

using TypeID = u64;       // Hashed type identifier
using PropertyID = u64;   // Hashed property identifier

// --- Property flags ---
enum class PropertyFlag : u32 {
    None            = 0,
    ReadOnly        = 1 << 0,
    Hidden          = 1 << 1,
    Deprecated      = 1 << 2,
    Serializable    = 1 << 3,
    Replicated      = 1 << 4,
};

// --- Property descriptor (runtime representation of a member) ---
struct PropertyDescriptor {
    StringView      name;
    TypeID          type;       // Property type
    usize           offset;     // Byte offset within the owning struct
    usize           size;       // Size in bytes
    u32             flags = 0;
    // Attribute data (simplified for now)
    std::vector<std::pair<StringView, String>> attributes;
};

// --- Type descriptor (runtime representation of a reflected type) ---
struct TypeDescriptor {
    StringView              name;
    TypeID                  typeID;
    usize                   size;
    // Factory: creates a default-constructed instance
    std::function<void*(void)> factory;
    // Properties
    std::vector<PropertyDescriptor> properties;
    // Base types
    std::vector<TypeID>     baseTypes;
};

// --- Global type registry ---
class TypeRegistry {
public:
    static TypeRegistry& Instance();

    // Register a type (called at static init time)
    void RegisterType(TypeDescriptor desc);

    // Query
    const TypeDescriptor* FindType(TypeID id) const;
    const TypeDescriptor* FindTypeByName(StringView name) const;

    // Iterate
    void ForEachType(std::function<void(const TypeDescriptor&)> callback) const;

private:
    std::unordered_map<TypeID, TypeDescriptor>       m_TypeMap;
    std::unordered_map<StringView, TypeID>           m_NameMap; // FIXME: string_view as key is unsafe for static strings only
};

// --- Hash utility ---
constexpr TypeID HashString(StringView str) {
    // FNV-1a 64-bit hash
    u64 hash = 14695981039346656037ull;
    for (char c : str) {
        hash ^= static_cast<u64>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace he::reflect
