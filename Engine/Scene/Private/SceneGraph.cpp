#include "Scene/SceneGraph.h"
#include "Scene/World.h"

namespace he {

SceneGraph::SceneGraph(World& world)
    : m_World(world) {
}

void SceneGraph::SetParent(Entity child, Entity parent) {
    // 移除旧父关系
    RemoveParent(child);

    m_Nodes[child.id].parent = parent;
    m_Nodes[parent.id].children.push_back(child);
    MarkDirty(child);
}

void SceneGraph::RemoveParent(Entity child) {
    auto it = m_Nodes.find(child.id);
    if (it == m_Nodes.end()) return;

    Entity oldParent = it->second.parent;
    if (oldParent.IsValid()) {
        auto& siblings = m_Nodes[oldParent.id].children;
        for (usize i = 0; i < siblings.size(); ++i) {
            if (siblings[i].id == child.id) {
                siblings[i] = siblings.back();
                siblings.pop_back();
                break;
            }
        }
        it->second.parent = {kInvalidEntity};
    }
}

Entity SceneGraph::GetParent(Entity entity) const {
    auto it = m_Nodes.find(entity.id);
    return (it != m_Nodes.end()) ? it->second.parent : Entity{kInvalidEntity};
}

float4x4 SceneGraph::GetWorldMatrix(Entity entity) {
    auto* tf = m_World.GetComponent<TransformComponent>(entity);
    if (!tf) return float4x4(1.0f);

    auto& node = m_Nodes[entity.id];
    node.localMatrix = tf->GetLocalMatrix();

    return ComputeWorldMatrix(entity);
}

float3 SceneGraph::GetWorldPosition(Entity entity) {
    float4x4 world = GetWorldMatrix(entity);
    return float3(world[3]);  // 第四列是平移
}

void SceneGraph::MarkDirty(Entity entity) {
    auto& node = m_Nodes[entity.id];
    node.dirty = true;
    // 级联标记子节点
    for (auto& child : node.children) {
        MarkDirty(child);
    }
}

void SceneGraph::UpdateTransforms() {
    // 每帧全量更新世界矩阵（后续可优化为仅 dirty 节点）
    for (auto& [id, node] : m_Nodes) {
        auto* tf = m_World.GetComponent<TransformComponent>(Entity{id});
        if (tf) node.localMatrix = tf->GetLocalMatrix();
        ComputeWorldMatrix(Entity{id});
    }
}

float4x4 SceneGraph::ComputeWorldMatrix(Entity entity) {
    auto& node = m_Nodes[entity.id];
    Entity parent = node.parent;
    if (parent.IsValid()) {
        node.worldMatrix = ComputeWorldMatrix(parent) * node.localMatrix;
    } else {
        node.worldMatrix = node.localMatrix;
    }
    node.dirty = false;
    return node.worldMatrix;
}

} // namespace he
