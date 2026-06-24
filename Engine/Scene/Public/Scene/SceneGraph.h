#pragma once

#include "Scene/Entity.h"
#include "Scene/Transform.h"
#include "Math/Math.h"
#include "Containers/Array.h"

// ============================================================
// SceneGraph — 层级变换系统
//
// 管理实体父子关系，自动级联变换。
// Dirty Flag 机制：父节点移动时，子节点标记为脏，
// 仅在需要时重新计算世界矩阵。
// ============================================================

namespace he {

class World; // 前向声明

class SceneGraph {
public:
    SceneGraph(World& world);

    /// 建立父子关系
    void SetParent(Entity child, Entity parent);
    /// 移除父关系
    void RemoveParent(Entity child);
    /// 获取父实体
    Entity GetParent(Entity entity) const;

    /// 获取世界空间变换矩阵
    float4x4 GetWorldMatrix(Entity entity);
    /// 获取世界空间位置
    float3 GetWorldPosition(Entity entity);

    /// 标记变换已修改（由系统调用）
    void MarkDirty(Entity entity);
    /// 更新所有脏变换
    void UpdateTransforms();

private:
    struct Node {
        Entity    parent = {kInvalidEntity};
        TArray<Entity> children;
        bool      dirty  = true;       // 本地变换已修改
        float4x4  localMatrix = float4x4(1.0f);
        float4x4  worldMatrix = float4x4(1.0f);
    };

    float4x4 ComputeWorldMatrix(Entity entity);

    World& m_World;
    std::unordered_map<EntityID, Node> m_Nodes;
};

} // namespace he
