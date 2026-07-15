#include "Scene/World.h"
#include "Scene/Transform.h"
#include "Core/Log.h"

namespace he {

World::~World() {
    // 清理所有实体
    for (auto& entity : m_Entities) {
        for (auto& [type, bucket] : m_Store) {
            for (auto& entry : bucket) {
                if (entry.entityID == entity.id) {
                    entry.ptr->OnDestroy();
                }
            }
        }
    }
    m_Entities.clear();
    m_Store.clear();
    HE_CORE_INFO("World destroyed ({} entities)", m_Entities.size());
}

Entity World::CreateEntity(StringView name) {
    EntityID id = m_NextID++;
    Entity e{id};
    m_Entities.push_back(e);
    HE_CORE_INFO("Entity #{} created [{}]", id, name);
    return e;
}

void World::DestroyEntity(Entity entity) {
    CleanupEntity(entity);
    // 从实体列表移除
    for (usize i = 0; i < m_Entities.size(); ++i) {
        if (m_Entities[i].id == entity.id) {
            m_Entities[i] = m_Entities.back();
            m_Entities.pop_back();
            break;
        }
    }
}

bool World::IsValid(Entity entity) const {
    for (auto& e : m_Entities) {
        if (e.id == entity.id) return true;
    }
    return false;
}

std::vector<World::ComponentEntry>& World::GetBucket(std::type_index type) {
    return m_Store[type];
}

void World::CleanupEntity(Entity entity) {
    for (auto& [type, bucket] : m_Store) {
        for (usize i = 0; i < bucket.size(); ++i) {
            if (bucket[i].entityID == entity.id) {
                bucket[i].ptr->OnDestroy();
                bucket[i] = std::move(bucket.back());
                bucket.pop_back();
                break;
            }
        }
    }
}

void World::Update(f32 deltaTime) {
    for (auto& [type, bucket] : m_Store) {
        for (auto& entry : bucket) {
            if (entry.ptr->IsActive()) {
                entry.ptr->OnUpdate(deltaTime);
            }
        }
    }
}

} // namespace he
