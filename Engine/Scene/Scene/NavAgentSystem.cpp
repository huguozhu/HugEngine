// ============================================================
// Scene/NavAgentSystem.cpp — 寻路代理驱动实现（C4）
// ============================================================

#include "Scene/NavAgentSystem.h"
#include "Scene/NavAgentComponent.h"
#include "Scene/NavMeshComponent.h"
#include "Scene/NavMeshSystem.h"
#include "Scene/Transform.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"

#include <cmath>

namespace he {

void NavAgentSystem::Update(World& world, SceneGraph&, f32 dt) {
    if (dt <= 0.0f) return;

    world.ForEach<NavAgentComponent>([&](Entity e, NavAgentComponent& agent) {
        auto* xf = world.GetComponent<TransformComponent>(e);
        if (!xf) return;
        if (!agent.hasTarget || agent.bReached) return;

        auto* nav = world.GetComponent<NavMeshComponent>(Entity{agent.navMeshEntity});
        if (!nav) return;

        // 1. 需要重新寻路 → FindPath
        if (agent.needsRepath || agent.path.empty()) {
            std::vector<float3> p;
            if (NavMeshSystem::FindPath(world, *nav, xf->position, agent.target, p)) {
                agent.path = std::move(p);
                agent.pathIndex = 0;
                agent.needsRepath = false;
            } else {
                agent.path.clear();
                agent.bReached = true;   // 当前不可达（可能因起点/终点阻挡）
                return;
            }
        }

        // 2. 沿路径点移动
        if (agent.pathIndex >= (int)agent.path.size()) {
            agent.bReached = true;
            return;
        }
        float3 goal = agent.path[agent.pathIndex];
        float3 toGoal = goal - xf->position;
        float dist = glm::length(toGoal);
        if (dist <= agent.arriveRadius) {
            // 到达该路径点 → 推进到下一个（且贴到点上）
            xf->position = goal;
            ++agent.pathIndex;
            if (agent.pathIndex >= (int)agent.path.size())
                agent.bReached = true;
        } else {
            float3 dir = toGoal / dist;
            xf->position += dir * (agent.speed * dt);
        }
    });
}

} // namespace he
