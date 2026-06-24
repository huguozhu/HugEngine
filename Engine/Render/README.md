# Render (L4)

渲染管线 — RenderGraph 调度 + 前向/延迟管线 + 光照 + 后处理。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Graph | RenderGraph (Pass DAG, 拓扑排序, 资源依赖推导, 自动 Barrier 规划) | P1 ✅ |
| Pipeline | Forward (HDR+PBR), Deferred (GBuffer+LightPass), Forward+ (Clustered) | P2 |
| Lighting | Clustered Shading 光照剔除, PBR BRDF, IBL | P2 |
| Shadow | CSM + PCF + VSM | P3 |
| PostProcess | Bloom, DOF, MotionBlur, ToneMapping, AutoExposure | P3+ |

**依赖**: RHI, Shader, Scene
**关键接口**: `RenderGraph::AddPass()`, `RenderGraph::Compile()`, `RenderGraph::Execute()`
