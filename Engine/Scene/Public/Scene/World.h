#pragma once

#include "Scene/Entity.h"
#include "Scene/Component.h"
#include "Containers/Array.h"

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <functional>

// ============================================================
// World — 实体容器 + 组件存储
//
// 用法:
//   World world;
//   Entity e = world.CreateEntity("Player");
//   world.AddComponent<TransformComponent>(e);
//   world.ForEach<TransformComponent>([](Entity e, auto& t) { ... });
// ============================================================

namespace he {

class World {
public:
    World() = default;
    ~World();

    // --- 实体管理 ---
    Entity CreateEntity(StringView name = "Entity");
    void   DestroyEntity(Entity entity);
    bool   IsValid(Entity entity) const;

    // --- 组件管理 ---
    template<typename T, typename... Args>
    T* AddComponent(Entity entity, Args&&... args);

    template<typename T>
    T* GetComponent(Entity entity);

    template<typename T>
    void RemoveComponent(Entity entity);

    template<typename T>
    bool HasComponent(Entity entity) const;

    // --- 遍历 ---
    template<typename T>
    void ForEach(std::function<void(Entity, T&)> callback);

    usize GetEntityCount() const { return m_Entities.size(); }
    void  Update(f32 deltaTime);

private:
    struct ComponentEntry {
        EntityID    entityID;
        std::unique_ptr<Component> ptr;
    };

    std::vector<ComponentEntry>& GetBucket(std::type_index type);
    void CleanupEntity(Entity entity);

    EntityID m_NextID = 1;
    TArray<Entity> m_Entities;
    std::unordered_map<std::type_index, std::vector<ComponentEntry>> m_Store;
};

// ============================================================
// 模板实现
// ============================================================

template<typename T, typename... Args>
T* World::AddComponent(Entity entity, Args&&... args) {
    if (!IsValid(entity)) return nullptr;

    auto& bucket = GetBucket(std::type_index(typeid(T)));
    for (auto& entry : bucket) {
        if (entry.entityID == entity.id) return nullptr; // 已存在
    }

    auto comp = std::make_unique<T>(std::forward<Args>(args)...);
    T* ptr = comp.get();

    ptr->m_State = ComponentState::Created;
    ptr->OnCreate();
    ptr->m_State = ComponentState::Active;
    ptr->OnStart();

    bucket.push_back({entity.id, std::move(comp)});
    return ptr;
}

template<typename T>
T* World::GetComponent(Entity entity) {
    auto& bucket = GetBucket(std::type_index(typeid(T)));
    for (auto& entry : bucket) {
        if (entry.entityID == entity.id)
            return static_cast<T*>(entry.ptr.get());
    }
    return nullptr;
}

template<typename T>
void World::RemoveComponent(Entity entity) {
    auto& bucket = GetBucket(std::type_index(typeid(T)));
    for (usize i = 0; i < bucket.size(); ++i) {
        if (bucket[i].entityID == entity.id) {
            bucket[i].ptr->OnDestroy();
            bucket[i].ptr->m_State = ComponentState::Destroyed;
            bucket[i] = std::move(bucket.back());
            bucket.pop_back();
            return;
        }
    }
}

template<typename T>
bool World::HasComponent(Entity entity) const {
    auto it = m_Store.find(std::type_index(typeid(T)));
    if (it == m_Store.end()) return false;
    for (auto& entry : it->second) {
        if (entry.entityID == entity.id) return true;
    }
    return false;
}

template<typename T>
void World::ForEach(std::function<void(Entity, T&)> callback) {
    auto& bucket = GetBucket(std::type_index(typeid(T)));
    for (auto& entry : bucket) {
        Entity e{entry.entityID};
        callback(e, *static_cast<T*>(entry.ptr.get()));
    }
}

} // namespace he
