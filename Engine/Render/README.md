# Render (L4)

渲染管线 — RenderGraph 调度 + 前向/延迟管线 + 光照 + 后处理。

| 子系统 | 说明 | Phase |
|--------|------|-------|
| Graph | RenderGraph (Pass DAG, Barrier 自动推导, 资源别名, Async Compute 调度) | P1 |
| Pipeline | Forward (HDR+PBR), Deferred (GBuffer+LightPass), Forward+ (Clustered) | P1-2 |
| Lighting | Clustered Shading 光照剔除, PBR BRDF, IBL | P1-2 |
| Shadow | CSM + PCF + VSM (后续 Phase) | P2-3 |
| PostProcess | Bloom, DOF, MotionBlur, ToneMapping, AutoExposure | P1-6 |

**依赖**: RHI, Shader, Scene
