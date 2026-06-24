# Asset

资源系统 — 格式加载 + 注册表 + 异步导入管线。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| glTF | glTF 2.0 GLB + JSON 解析, PBR 材质转换, Mesh→Entity 自动装配 | P1 |
| Registry | AssetRegistry (异步扫描、依赖图、缩略图、UUID 映射) | P2 |

**依赖**: Scene, Reflect, Render
