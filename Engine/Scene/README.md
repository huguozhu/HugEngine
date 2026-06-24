# Scene (L5)

场景管理 — Actor-Component 架构核心。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Entity | UUID 实体句柄 | P1 ✅ |
| Component | 基类 + 生命周期 (Create/Start/Update/Destroy) + HE_COMPONENT 宏 | P1 ✅ |
| Transform | 位置/旋转/缩放 + GetLocalMatrix() | P1 ✅ |
| World | 实体容器 + AddComponent<T>/GetComponent<T>/ForEach<T> | P1 ✅ |
| Graph | SceneGraph 层级变换 + Dirty Flag 级联 | P1 ✅ |

**依赖**: Reflect, Core
**关键接口**: `World::AddComponent<T>()`, `World::ForEach<T>(callback)`
