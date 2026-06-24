#pragma once

#include "Core/Types.h"

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <queue>
#include <array>

// ============================================================
// Container type aliases — consistent naming across the engine
//
// Design: Wraps STL containers with engine-friendly names.
// Future: Replace with custom implementations for better
// debug visualization, memory tracking, and SIMD alignment.
// ============================================================

namespace he {

// --- Dynamic array ---
template<typename T>
using TArray = std::vector<T>;

// --- Hash map ---
template<typename K, typename V>
using TMap = std::unordered_map<K, V>;

// --- Hash set ---
template<typename T>
using THashSet = std::unordered_set<T>;

// --- Double-ended queue ---
template<typename T>
using TDeque = std::deque<T>;

// --- Priority queue ---
template<typename T>
using TPriorityQueue = std::priority_queue<T>;

// --- Fixed-size array ---
template<typename T, usize N>
using TFixedArray = std::array<T, N>;

// --- Utility ---
template<typename T>
void TArrayRemoveSwap(TArray<T>& arr, usize index) {
    HE_ASSERT(index < arr.size());
    arr[index] = std::move(arr.back());
    arr.pop_back();
}

template<typename T>
bool TArrayContains(const TArray<T>& arr, const T& value) {
    return std::find(arr.begin(), arr.end(), value) != arr.end();
}

// --- Tiny inline vector (stack-allocated small buffer) ---
template<typename T, usize InlineCapacity = 16>
class TInlineVec {
public:
    void push_back(const T& val) {
        if (m_Size < InlineCapacity) {
            m_Inline[m_Size++] = val;
        } else {
            if (m_Heap.empty()) {
                m_Heap.reserve(InlineCapacity * 2);
                for (usize i = 0; i < InlineCapacity; ++i)
                    m_Heap.push_back(std::move(m_Inline[i]));
            }
            m_Heap.push_back(val);
            m_Size++;
        }
    }

    T& operator[](usize i) {
        return (i < InlineCapacity && m_Heap.empty()) ? m_Inline[i] : m_Heap[i];
    }

    const T& operator[](usize i) const {
        return (i < InlineCapacity && m_Heap.empty()) ? m_Inline[i] : m_Heap[i];
    }

    usize size()  const { return m_Size; }
    bool  empty() const { return m_Size == 0; }
    void  clear()       { m_Size = 0; m_Heap.clear(); }

    T* begin() { return m_Heap.empty() ? m_Inline : m_Heap.data(); }
    T* end()   { return begin() + m_Size; }

private:
    usize       m_Size = 0;
    T           m_Inline[InlineCapacity];
    TArray<T>   m_Heap;
};

} // namespace he
