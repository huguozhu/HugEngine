# DeferredPipeline 实现 Spec

> 版本: 1.0 | 日期: 2026-07-03 | 基于 HugEngine Phase 5 架构

## 1. 目标

实现 `DeferredPipeline`，继承 `IRenderPipeline`，复用已有 Shadow/GI/PostProcess 子系统。GBuffer 几何处理 + 独立全屏 Lighting Pass。

## 2. 新增文件清单

| 文件 | 说明 |
|------|------|
| `Engine/Render/Pipeline/DeferredPipeline.h` | 类声明 |
| `Engine/Render/Pipeline/DeferredPipeline.cpp` | 初始化 + BuildFrameGraph |
| `Engine/Shader/Shaders/GBuffer.vert.slang` | GBuffer 顶点着色器 |
| `Engine/Shader/Shaders/GBuffer.frag.slang` | GBuffer 片段着色器（3 MRT） |
| `Engine/Shader/Shaders/DeferredLighting.vert.slang` | 全屏三角形 |
| `Engine/Shader/Shaders/DeferredLighting.frag.slang` | PBR 光照 + IBL + RSM + Shadow |
| `Samples/04.Deferred/main.cpp` | 测试 Sample |

## 3. 类声明

```cpp
// DeferredPipeline.h
class DeferredPipeline : public IRenderPipeline {
public:
    bool Initialize(rhi::IRHIDevice* device) override;
    void Shutdown() override;
    void NextFrame() override;
    void OnResize(u32 w, u32 h) override;
    const char* GetName() const override { return "DeferredPipeline"; }

    IShadowSystem*       GetShadowSystem() override { return m_ShadowSystem.get(); }
    IGlobalIllumination* GetGI()           override { return m_GI.get(); }
    ToneMapPass*         GetToneMap()            { return m_ToneMap.get(); }

    void Render(rhi::IRHICommandList* cmd, he::World& world,
                he::SceneGraph& sg, const CameraData& camera) override;
    void SetSwapChain(rhi::IRHISwapChain* sc) { m_SwapChain = sc; }

private:
    void BuildFrameGraph(RenderGraph& rg, he::World& world,
                         he::SceneGraph& sg, const CameraData& camera);
    void CollectLights(PushConstantData& pc, he::World& world,
                       he::SceneGraph& sg, const CameraData& camera);

    // 设备 + 资源
    rhi::IRHIDevice* m_Device = nullptr;
    rhi::IRHISwapChain* m_SwapChain = nullptr;

    // GBuffer
    std::unique_ptr<rhi::IRHITexture> m_GBufferA, m_GBufferB, m_GBufferC; // MRT 0/1/2
    std::unique_ptr<rhi::IRHITexture> m_GBufferDepth;
    std::unique_ptr<rhi::IRHIPipelineState> m_GBufferPSO;    // GBuffer 几何 PSO

    // Deferred Lighting
    std::unique_ptr<rhi::IRHIPipelineState> m_LightingPSO;   // 全屏光照 PSO
    rhi::DescriptorSetLayoutHandle m_LightingLayout;
    rhi::DescriptorSetHandle       m_LightingSet;

    // 三缓冲 (复用 ForwardPipeline 模式)
    static constexpr u32 MFI = MAX_FRAMES_IN_FLIGHT;
    std::unique_ptr<rhi::IRHIBuffer> m_LightBuffers[MFI];
    std::unique_ptr<rhi::IRHIBuffer> m_ObjectBuffers[MFI];
    u32 m_CurrentFrameSlot = 0;

    // 子系统
    std::unique_ptr<IShadowSystem>       m_ShadowSystem;
    std::unique_ptr<IGlobalIllumination> m_GI;
    std::unique_ptr<GI_RSM>              m_RSM;
    std::unique_ptr<ToneMapPass>         m_ToneMap;
    std::unique_ptr<SkyboxPass>          m_Skybox;
    std::unique_ptr<SceneRenderer>       m_SceneRenderer;

    u32 m_Width = 0, m_Height = 0;
    bool m_Ready = false;
};
```

## 4. GBuffer Shader 规格

### 4.1 GBuffer.vert.slang

输入: `inPosition(loc=0)`, `inNormal(loc=1)`, `inUV(loc=2)`
SSBO: `u_Objects[objectIndex].worldMatrix`
PushConstant: `viewProjMatrix`, `objectIndex`

输出: `worldPos(loc=0)`, `worldNormal(loc=1)`, `uv(loc=2)`, `SV_Position`

### 4.2 GBuffer.frag.slang

输入: `worldPos`, `worldNormal`, `uv`
纹理采样: `u_BaseColorTex(5)`, `u_NormalTex(6)`, `u_MetallicRoughnessTex(7)`, `u_OcclusionTex(8)`
SSBO: `u_Objects[objectIndex]`

MRT 输出:
```
layout(location=0): float4(albedo.rgb, metallic)     // RGBA16_FLOAT
layout(location=1): float4(N*0.5+0.5, roughness)      // RGBA16_FLOAT
layout(location=2): float4(emissive.rgb, ao)           // RGBA16_FLOAT
```

Alpha mask: `if (alpha < alphaCutoff) discard;`

## 5. Deferred Lighting Shader 规格

### 5.1 DeferredLighting.vert.slang

全屏三角形 (SV_VertexID), 输出 UV: `float2(vid*2, vid%2) * 2.0` (3 顶点大三角形)
输出: `UV(loc=0)`, `SV_Position`

### 5.2 DeferredLighting.frag.slang

#### 输入纹理 (Descriptor Set)
| Binding | 类型 | 内容 |
|---------|------|------|
| 0 | Texture2D | u_GBufferA (albedo+metallic) |
| 1 | Texture2D | u_GBufferB (normal+roughness) |
| 2 | Texture2D | u_GBufferC (emissive+ao) |
| 3 | Texture2D | u_Depth (D32_FLOAT) |
| 4 | Texture2D | u_ShadowMap0 (CSM cascade 0) |
| 5 | Texture2D | u_ShadowMap1 (CSM cascade 1) |
| 6 | Texture2D | u_ShadowMap2 (CSM cascade 2) |
| 7 | TextureCube | u_IrradianceMap |
| 8 | TextureCube | u_PrefilterMap |
| 9 | Texture2D | u_BRDF_LUT |
| 10 | Texture2D | u_RSMPositionMap |
| 11 | Texture2D | u_RSMFluxMap |

#### SSBO
| Binding | 类型 | 内容 |
|---------|------|------|
| 12 | StructuredBuffer<GPULight> | u_Lights |
| 13 | StructuredBuffer<GPUShadowData> | u_ShadowData |

#### Push Constants
| 字段 | 大小 | 说明 |
|------|------|------|
| float4x4 invViewProj | 64B | 逆 VP（深度反算世界坐标） |
| float4 cameraPos | 16B | 相机世界位置 |
| uint lightCount | 4B | 有效光源数 |
| float iblIntensity | 4B | IBL 强度倍率 |

#### 伪代码
```glsl
void main() {
    // 1. 反算世界坐标
    float depth = u_Depth.Sample(uv).r;
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 worldPos4 = mul(invViewProj, clipPos);
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // 2. 读取 GBuffer
    float4 gbA = u_GBufferA.Sample(uv);  // albedo.rgb, metallic
    float4 gbB = u_GBufferB.Sample(uv);  // N*0.5+0.5, roughness
    float4 gbC = u_GBufferC.Sample(uv);  // emissive.rgb, ao

    float3 albedo = gbA.rgb;
    float metallic = gbA.a;
    float3 N = normalize(gbB.rgb * 2.0 - 1.0);
    float roughness = gbB.a;
    float ao = gbC.a;

    float3 V = normalize(cameraPos.xyz - worldPos);
    float3 color = float3(0.0);

    // 3. 直接光照循环
    for (uint i = 0; i < lightCount; i++) {
        GPULight light = u_Lights[i];
        // ... 与 Forward PBR 相同的类型分派 ...
        // 阴影采样（u_ShadowData + u_ShadowMapX）
        color += PBR_BRDF(albedo, metallic, roughness, N, V, L) * radiance * shadow;
    }

    // 4. IBL
    float3 F0 = lerp(0.04, albedo, metallic);
    float3 F = F_SchlickRoughness(max(dot(N,V),0), F0, roughness, metallic);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    color += kD * u_IrradianceMap.Sample(N).rgb * albedo * iblIntensity;
    float3 R = reflect(-V, N);
    color += u_PrefilterMap.SampleLevel(R, roughness * 4.0).rgb *
             (F * envBRDF.r + envBRDF.g) * iblIntensity;

    // 5. RSM 间接
    // ... (与 Forward 相同逻辑)

    color *= ao;
    color += gbC.rgb; // emissive
    return float4(color, 1.0);
}
```

## 6. DeferredPipeline::Initialize

```cpp
bool DeferredPipeline::Initialize(rhi::IRHIDevice* device) {
    // 1. 创建 GBuffer 纹理 (MRT 3×RGBA16 + D32)
    //    - 尺寸 = 初始 SwapChain 尺寸（OnResize 重建）
    //    - m_GBufferA/B/C: RGBA16_FLOAT, RenderTarget | ShaderResource
    //    - m_GBufferDepth: D32_FLOAT, DepthStencil

    // 2. 创建三缓冲 (Lights + Objects)
    //    - 与 ForwardPipeline 相同模式

    // 3. 创建子系统
    //    - ShadowSystem, GI_IBL, GI_RSM, ToneMapPass, SkyboxPass, SceneRenderer
    //    - 全部 Initialize

    // 4. 创建 GBuffer PSO
    //    - VS: GBuffer.vert, FS: GBuffer.frag
    //    - colorAttachmentCount=3, colorFormats={RGBA16,RGBA16,RGBA16}
    //    - depthFormat=D32, depthTest/Write=true
    //    - vertexLayout = StaticVertex (position+normal+uv)

    // 5. 创建 Lighting PSO
    //    - VS: DeferredLighting.vert, FS: DeferredLighting.frag
    //    - colorAttachmentCount=1, colorFormats={RGBA16} (输出到 HDR target)
    //    - depthTest=false, depthWrite=false
    //    - descriptorSetLayout: GBuffer MRT + Shadow + IBL + RSM

    // 6. 创建 HDR 目标 (RGBA16_FLOAT + D32)
    //    - Lighting Pass 输出到 HDR target
    //    - ToneMap 采样 HDR → SwapChain

    return true;
}
```

## 7. BuildFrameGraph 编排

```cpp
void DeferredPipeline::BuildFrameGraph(RenderGraph& rg, ...) {
    rg.SetSwapChain(m_SwapChain);

    // 资源导入
    auto gbA = rg.CreateTexture("GBufferA", {RGBA16, w, h, RT|SRV});
    auto gbB = rg.CreateTexture("GBufferB", {RGBA16, w, h, RT|SRV});
    auto gbC = rg.CreateTexture("GBufferC", {RGBA16, w, h, RT|SRV});
    auto gbD = rg.CreateTexture("GBufferDepth", {D32, w, h, DS|SRV});
    auto hdrColor = rg.CreateTexture("HDR", {RGBA16, w, h, RT|SRV});
    auto hdrDepth = rg.CreateTexture("HDRDepth", {D32, w, h, DS});
    auto backBuf = rg.ImportBackBuffer();

    // 子系统纹理（Shadow/IBL/RSM）
    auto sm0 = rg.ImportTexture("Shadow0", m_ShadowSystem->GetShadowMap(0));
    // ...

    // 编排
    rg.AddPass("Shadow", {}, {sm0,sm1,sm2}, [...]{ ... });   // 阴影
    rg.AddPass("GI_IBL", {}, {irr,pref,lut}, [...]{ ... });  // IBL
    rg.AddPass("GI_RSM", {sm0}, {rsmPos,rsmFlux}, [...]{ ... });
    rg.AddPass("GBuffer", {}, {gbA,gbB,gbC,gbD}, [...]{     // GBuffer
        c->SetPipeline(m_GBufferPSO.get());
        c->BeginOffscreenPassMRT({gbA,gbB,gbC}, gbD, w, h);
        SceneRenderer::Prepare(...);
        for (auto& di : drawItems) { c->DrawIndexed(...); }
        c->EndOffscreenPass();
    });
    rg.AddPass("Lighting", {gbA,gbB,gbC,gbD,sm0,irr,pref,lut,rsmPos,rsmFlux},
        {hdrColor, hdrDepth}, [...]{
        c->SetPipeline(m_LightingPSO.get());
        c->BeginOffscreenPass(hdrColor, hdrDepth, w, h);
        c->Draw(3); // fullscreen triangle
        c->EndOffscreenPass();
    });
    rg.AddPass("Skybox", {gbD}, {hdrColor}, [...]{ RenderSkybox(...); });
    rg.AddPass("ToneMap", {hdrColor}, {backBuf}, [...]{ ... });
}
```

## 8. 与 ForwardPipeline 代码复用

| 组件 | 复用方式 |
|------|----------|
| ShadowSystem | 直接使用，ForwardPipeline 同款 Initialize 模式 |
| GI_IBL + GI_RSM | 直接使用 |
| ToneMapPass + SkyboxPass | 直接使用 |
| SceneRenderer | 直接使用 |
| CollectLights | 复制逻辑（Light SSBO 填充） |
| 三缓冲管理 | 复制 ForwardPipeline 模式 |

## 9. 实施顺序

| Step | 产出 | 验证 |
|:---:|------|------|
| 1 | GBuffer.vert/frag + DeferredLighting.vert/frag | slangc 编译通过 |
| 2 | DeferredPipeline.h + Initialize() | 编译 + 资源创建日志 |
| 3 | DeferredPipeline::BuildFrameGraph (仅 GBuffer→Lighting→ToneMap) | 04.Deferred 黑屏→有画面 |
| 4 | 添加 Shadow 复用 | 阴影可见 |
| 5 | 添加 IBL 复用 | 环境光照可见 |
| 6 | 添加 RSM 复用 | RSM 间接光 |
| 7 | 添加 Skybox | 天空盒 |
| 8 | 集成到 CMake + Sample | 04.Deferred 完整运行 |
