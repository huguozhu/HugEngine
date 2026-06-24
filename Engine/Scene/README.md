# Scene (L5)

场景管理 — Actor-Component 架构核心。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Entity | UUID 实体、World 容器、Entity 查询 | P1 |
| Component | Component 基类、生命周期 (Create/Start/Update/Destroy)、查询索引 | P1 |
| Graph | SceneGraph 树形层级、Transform 级联、Dirty Flag 传播 | P1 |

**依赖**: Reflect, Core
**关键接口**: `Entity::AddComponent<T>()`, `World::Query<Components...>()`
