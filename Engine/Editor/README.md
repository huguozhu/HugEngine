# Editor (L8)

编辑器框架 — Editor/Engine 分离架构。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Framework | Editor/Engine 进程分离, Command 模式 Undo/Redo, Transaction 系统 | P1 |
| Panels | World Outliner (Entity 树), Details Panel (反射驱动属性编辑), Content Browser | P1-2 |
| UI | Dear ImGui 封装, 自研 Docking 框架, 多视口管理 | P1 |

**依赖**: Engine (所有模块)
