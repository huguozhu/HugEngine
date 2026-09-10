// ============================================================
// 02.Cube — HugEngine PBR 前向渲染管线演示
//
// 使用逐物体 Push Constants 的 PBR 渲染：
//   Engine → RHI Vulkan → ForwardPipeline → 场景遍历
//
// 创建多个带不同材质的立方体，展示 PBR 效果。
// 自由相机：WASD + 鼠标右键旋转
// ============================================================

#include "Core/Core.h"
#include "Core/CVar.h"
#include "Core/Engine.h"
#include "Platform/Window.h"
#include "RHI/RHI.h"
#include "Pipeline/ForwardPipeline.h"
#include "Pipeline/DeferredPipeline.h"
#include "Pipeline/HybridRTPipeline.h"
#include "Pipeline/PathTracingPipeline.h"
#include "Pipeline/PTQualityCVars.h"
#include "Pipeline/CameraController.h"
#include "Pipeline/Camera.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/CubeComponent.h"
#include "Scene/SphereComponent.h"
#include "Scene/Transform.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/PhysicalSkyComponent.h"
#include "Scene/ParticleComponent.h"
#include "Scene/BillboardComponent.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/TextRenderSystem.h"
#include "Scene/DecalComponent.h"
#include "Scene/CollisionComponent.h"
#include "Scene/CollisionSystem.h"
#include "Scene/CharacterMovementComponent.h"
#include "Scene/MovementSystem.h"
#include "Scene/InstancedMeshComponent.h"
#include "Scene/SkeletalMeshComponent.h"
#include "Scene/SkeletalMeshSystem.h"
#include "Scene/SplineComponent.h"
#include "Scene/SplineMeshComponent.h"
#include "Scene/SplineMeshSystem.h"
#include "Physics/Physics/RigidBodyComponent.h"
#include "Physics/Physics/PhysicsSystem.h"
#include "Scene/NavMeshComponent.h"
#include "Scene/NavMeshSystem.h"
#include "Scene/NavAgentComponent.h"
#include "Scene/NavAgentSystem.h"
#include "Asset/glTFLoader.h"
#include "Editor/ImGuiIntegration.h"
#include "imgui.h"

#include <cmath>
#include <cstring>
#include <vector>
#include <fstream>
#include <filesystem>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

using namespace he;

// ============================================================
// 渲染管线模式 CVar
// ============================================================
// 渲染管线模式 CVar（0=Forward, 1=Deferred, 2=HybridRT, 3=PathTrace）
// 默认 0 = 前向渲染（与示例标题一致；其他模式可在面板切换）
he::CVar<int> cvPipelineMode("r.Pipeline.Mode", 0, "渲染管线模式 0=Forward 1=Deferred 2=HybridRT 3=PathTrace");
// 物理天空开关（1=用 Preetham 物理天空替代 Cubemap 天空盒；需 Forward 模式 r.Pipeline.Mode 0 才可见）
he::CVar<int> cvPhysicalSkyEnable("r.PhysicalSky.Enable", 1, "1=使用 Preetham 物理天空（替代 Cubemap 天空盒）");
// HDR 输出开关（1=SwapChain 请求 A2B10G10R10 + HDR10 ST.2084；需 HDR10 显示器/扩展支持，否则自动回退 SDR）
he::CVar<int> cvHDR("r.HDR.Enable", 0, "1=启用 HDR10 输出（需显示器/扩展支持，否则自动回退 SDR）");

// ============================================================
// 相机配置读写（简易 key=value 格式）
// ============================================================
static String g_ConfigPath = String(HUGE_CONTENT_DIR) + "Config/02_Cube.cfg";

static std::unordered_map<String, String> LoadConfigFile(const String& path) {
    std::unordered_map<String, String> map;
    std::ifstream f(path);
    if (!f.is_open()) return map;
    String line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == String::npos) continue;
        String key = line.substr(0, eq);
        String val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        map[key] = val;
    }
    return map;
}

static void SaveConfigFile(const String& path,
                            const std::unordered_map<String, String>& map) {
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path);
    if (!f.is_open()) return;
    for (auto& [k, v] : map)
        f << k << "=" << v << "\n";
}

static float GetFloat(const std::unordered_map<String, String>& m,
                      const String& key, float def = 0.0f) {
    auto it = m.find(key);
    return (it != m.end()) ? std::stof(it->second) : def;
}

// ============================================================
// 辅助：创建带材质的形状实体
// ============================================================
Entity CreateShapeEntity(World& world, SceneGraph& sg,
                         const float3& position, const float3& scale,
                         const float4& baseColor, float metallic, float roughness,
                         bool sphere = false)
{
    Entity e = world.CreateEntity(sphere ? "Sphere" : "Cube");

    auto* xform = world.AddComponent<TransformComponent>(e);
    xform->position = position;
    xform->scale    = scale;

    MeshComponent* mesh;
    if (sphere) {
        auto* sc = world.AddComponent<SphereComponent>(e);
        sc->radius = 0.5f;
        mesh = static_cast<MeshComponent*>(sc);
    } else {
        auto* cc = world.AddComponent<CubeComponent>(e);
        cc->halfExtent = 0.5f;
        mesh = static_cast<MeshComponent*>(cc);
    }

    mesh->baseColorFactor  = baseColor;
    mesh->metallicFactor   = metallic;
    mesh->roughnessFactor  = roughness;

    sg.SetParent(e, Entity{kInvalidEntity});
    return e;
}

// ============================================================
// 主函数
// ============================================================
int main() {
    // --- 1. 引擎启动 ---
    EngineConfig config;
    config.appName      = "HugEngine — PBR Forward Pipeline";
    config.windowWidth  = 1920;
    config.windowHeight = 1080;
    config.enableVSync  = true;

    Engine engine(config);
    engine.Initialize();

    // --- 2. 创建 RHI 设备 ---
    rhi::DeviceInitDesc rhiDesc;
    rhiDesc.backend          = rhi::Backend::Vulkan;
    // 注意：本机（Intel Arc B370 + 该版驱动 + Vulkan SDK 1.4.341 验证层）存在
    // 预先存在的驱动编译器崩溃（HEAD 同样复现）：开启验证层时 igc-default64.dll
    // 在 shader 编译线程随机 SIGSEGV。关闭验证层后稳定（RT 管线编译约 20s/个）。
    // 在 NVIDIA/其他稳定驱动上可恢复为 true 以获得验证信息。
    rhiDesc.enableValidation = false;
    rhiDesc.windowHandle     = engine.GetWindow()->GetNativeHandleRaw();

    auto device = rhi::CreateDevice(rhiDesc.backend);
    device->Initialize(rhiDesc);
    rhi::SetDevice(device.get());

    // --- 3. 创建 SwapChain ---
    // HDR 开关：开启时 SwapChain 优先选 A2B10G10R10 + HDR10 ST.2084（需显示器/扩展支持，否则自动回退 SDR）
    auto swapchain = device->CreateSwapChain({
        .windowHandle = engine.GetWindow()->GetNativeHandleRaw(),
        .width  = engine.GetWindow()->GetWidth(),
        .height = engine.GetWindow()->GetHeight(),
        .vsync  = true,
        .hdr    = cvHDR.Get() == 1,
    });
    // 一次性验证日志：打印交换链实际协商到的颜色格式
    HE_CORE_INFO("[SwapChain] 颜色格式: {}", swapchain->GetColorFormat() == rhi::Format::A2B10G10R10_UNORM_PACK32
        ? "A2B10G10R10 (HDR10)" : "BGRA8 (SDR)");

    // --- 4. 初始化场景 ---
    World world;
    SceneGraph sceneGraph(world);

    // 地板（深灰色立方体，粗糙，长宽 2x）+ 碰撞体（B3 角色地面检测用）
    Entity floorEntity = CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 0.0f, 0.0f), float3(200.0f, 0.2f, 200.0f),
        float4(0.3f, 0.3f, 0.35f, 1.0f), 0.0f, 0.9f);
    {
        auto* fc = world.AddComponent<CollisionComponent>(floorEntity);
        fc->shape       = CollisionShape::AABB;
        fc->halfExtents = float3(100.0f, 0.1f, 100.0f);   // 与视觉尺寸一致
    }

    // 金球（金属，光滑）
    CreateShapeEntity(world, sceneGraph,
        float3(-1.5f, 5.0f, 0.0f), float3(0.8f),
        float4(1.0f, 0.72f, 0.0f, 1.0f), 1.0f, 0.15f, true);

    // 物理演示彩球（Jolt C2）：RigidBodyComponent 动态球从高处下落，撞击地面弹跳
    {
        const float3 ballColors[4] = {
            float3(1.0f, 0.2f, 0.2f), float3(0.2f, 1.0f, 0.3f),
            float3(0.2f, 0.4f, 1.0f), float3(1.0f, 0.9f, 0.2f),
        };
        for (int i = 0; i < 4; ++i) {
            Entity ball = world.CreateEntity("PhysBall");
            world.AddComponent<TransformComponent>(ball);
            auto* sphere = world.AddComponent<SphereComponent>(ball);
            sphere->baseColorFactor = float4(ballColors[i], 1.0f);
            sphere->radius = 0.4f;
            sphere->segmentCount = 16;
            sphere->ringCount = 8;
            sphere->castShadow = true;
            sphere->OnCreate();
            auto* bxf = world.GetComponent<TransformComponent>(ball);
            bxf->position = float3(-3.0f + i * 2.0f, 4.0f + i, -5.0f);   // 相机前方偏低，下落可见
            auto* rb = world.AddComponent<RigidBodyComponent>(ball);
            rb->shape = 0;
            rb->radius = 0.4f;
            rb->isDynamic = true;
            rb->mass = 1.0f;
            rb->friction = 0.4f;
            rb->restitution = 0.3f + i * 0.15f;
            // 不同弹性，弹跳高度区分;
            sceneGraph.SetParent(ball, Entity{kInvalidEntity});
        }
    }

    // 寻路演示（NavMesh C4）：NavMeshComponent + NavAgent 沿网格绕过阻挡墙走到目标
    Entity navEntity;
    Entity navAgentEntity;
    {
        navEntity = world.CreateEntity("NavMesh");
        world.AddComponent<TransformComponent>(navEntity);
        auto* nav = world.AddComponent<NavMeshComponent>(navEntity);
        nav->Resize(20, 20, 1.0f);   // 20x20 格，每格 1m
        nav->origin = float3(-10.0f, 0.0f, -20.0f);   // 网格覆盖 z ∈ [-20,0]（相机前方）
        // 中间阻挡墙（col 5..14, row 10）→ 需绕行
        for (int c = 5; c <= 14; ++c) nav->SetBlocked(c, 10, true);
        sceneGraph.SetParent(navEntity, Entity{kInvalidEntity});

        navAgentEntity = world.CreateEntity("NavAgent");
        auto* axf = world.AddComponent<TransformComponent>(navAgentEntity);
        axf->position = float3(-9.0f, 0.0f, -19.0f);   // 网格 (1,1)
        auto* sphere = world.AddComponent<SphereComponent>(navAgentEntity);
        sphere->baseColorFactor = float4(0.6f, 0.0f, 1.0f, 1.0f);   // 紫色寻路代理
        sphere->radius = 0.3f;
        sphere->segmentCount = 16;
        sphere->ringCount = 8;
        sphere->castShadow = true;
        sphere->OnCreate();
        auto* agent = world.AddComponent<NavAgentComponent>(navAgentEntity);
        agent->navMeshEntity = navEntity;
        agent->speed = 4.0f;
        agent->SetTarget(float3(8.0f, 0.0f, -2.0f));   // 目标（墙对侧，网格 (18,18)）→ 绕行
        sceneGraph.SetParent(navAgentEntity, Entity{kInvalidEntity});
    }
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 4.0f, 0.0f), float3(0.8f),
        float4(0.85f, 0.45f, 0.2f, 1.0f), 0.95f, 0.4f, true);

    // 蓝色塑料立方体（非金属，光滑）
    CreateShapeEntity(world, sceneGraph,
        float3(1.5f, 3.0f, 0.0f), float3(0.8f),
        float4(0.2f, 0.5f, 1.0f, 1.0f), 0.0f, 0.2f);

    // 红色橡胶立方体（非金属，粗糙）
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 6.0f, 1.5f), float3(0.7f),
        float4(0.9f, 0.15f, 0.1f, 1.0f), 0.0f, 0.85f);

    // 白色陶瓷球
    CreateShapeEntity(world, sceneGraph,
        float3(0.0f, 5.2f, -1.5f), float3(0.6f),
        float4(0.95f, 0.93f, 0.88f, 1.0f), 0.0f, 0.35f, true);

    // --- 广告牌（A4：始终面向相机的四边形，调试标记/粒子替代演示）---
    {
        // 大号品红不透明广告牌（醒目可见，验证朝向正确性）
        Entity be = world.CreateEntity("Billboard");
        world.AddComponent<TransformComponent>(be);
        auto* bb = world.AddComponent<BillboardComponent>(be);
        bb->size = float2(4.0f, 4.0f);
        bb->baseColorFactor = float4(1.0f, 0.0f, 1.0f, 1.0f);
        auto* bx = world.GetComponent<TransformComponent>(be);
        if (bx) bx->position = float3(0.0f, 4.0f, -6.0f);
        sceneGraph.SetParent(be, Entity{kInvalidEntity});

        // 半透明青色广告牌（粒子替代风格；天空背景下对比度低，转视角可见）
        Entity be2 = world.CreateEntity("BillboardCyan");
        world.AddComponent<TransformComponent>(be2);
        auto* bb2 = world.AddComponent<BillboardComponent>(be2);
        bb2->size = float2(2.0f, 2.0f);
        bb2->baseColorFactor = float4(0.2f, 0.8f, 1.0f, 0.75f);
        auto* bx2 = world.GetComponent<TransformComponent>(be2);
        if (bx2) bx2->position = float3(3.5f, 5.0f, -2.0f);
        sceneGraph.SetParent(be2, Entity{kInvalidEntity});
    }

    // --- 3D 文字（A5：始终面向相机的世界空间标签，CJK 走系统字体兜底）---
    Entity fpsTextEntity;  // 主循环中每 0.5 秒更新文字，验证脏标记 → 重栅格化路径
    {
        Entity te = world.CreateEntity("TextLabel");
        world.AddComponent<TransformComponent>(te);
        auto* tr = world.AddComponent<TextRenderComponent>(te);
        tr->text      = "HugEngine 广告牌";
        tr->textColor = float4(1.0f, 0.9f, 0.2f, 1.0f);
        tr->fontSize  = 64.0f;
        auto* tx = world.GetComponent<TransformComponent>(te);
        if (tx) tx->position = float3(3.5f, 7.0f, -2.0f);
        sceneGraph.SetParent(te, Entity{kInvalidEntity});

        Entity fe = world.CreateEntity("TextFPS");
        world.AddComponent<TransformComponent>(fe);
        auto* fps = world.AddComponent<TextRenderComponent>(fe);
        fps->text      = "FPS: --";
        fps->textColor = float4(0.4f, 1.0f, 0.6f, 1.0f);
        fps->fontSize  = 40.0f;
        auto* fx = world.GetComponent<TransformComponent>(fe);
        if (fx) fx->position = float3(-3.0f, 8.0f, -2.0f);
        sceneGraph.SetParent(fe, Entity{kInvalidEntity});
        fpsTextEntity = fe;
    }

    // --- 碰撞检测演示（B5：IJKL 移动探测球，碰到橙色盒变绿）---
    Entity collisionBoxEntity;
    Entity collisionProbeEntity;
    {
        // 静态碰撞盒：视觉立方体 + AABB 碰撞体（尺寸一致）
        collisionBoxEntity = world.CreateEntity("CollisionBox");
        world.AddComponent<TransformComponent>(collisionBoxEntity);
        auto* cb = world.AddComponent<CubeComponent>(collisionBoxEntity);
        cb->halfExtent = 1.0f;
        cb->baseColorFactor = float4(0.9f, 0.6f, 0.2f, 1.0f);   // 橙色
        auto* cc = world.AddComponent<CollisionComponent>(collisionBoxEntity);
        cc->shape       = CollisionShape::AABB;
        cc->halfExtents = float3(1.0f);
        auto* bx = world.GetComponent<TransformComponent>(collisionBoxEntity);
        if (bx) bx->position = float3(6.0f, 1.0f, -4.0f);
        sceneGraph.SetParent(collisionBoxEntity, Entity{kInvalidEntity});

        // 探测球：红 = 未重叠，绿 = 重叠（每帧 Overlap 驱动颜色）
        collisionProbeEntity = world.CreateEntity("CollisionProbe");
        world.AddComponent<TransformComponent>(collisionProbeEntity);
        auto* ps = world.AddComponent<SphereComponent>(collisionProbeEntity);
        ps->radius = 0.5f;
        ps->baseColorFactor = float4(1.0f, 0.2f, 0.2f, 1.0f);
        ps->castShadow = false;
        ps->OnCreate();
        auto* pc = world.AddComponent<CollisionComponent>(collisionProbeEntity);
        pc->shape  = CollisionShape::Sphere;
        pc->radius = 0.5f;
        auto* px = world.GetComponent<TransformComponent>(collisionProbeEntity);
        if (px) px->position = float3(6.0f, 1.0f, 0.0f);   // 初始距盒 4 米（未重叠）
        sceneGraph.SetParent(collisionProbeEntity, Entity{kInvalidEntity});
    }

    // --- 角色移动演示（B3：方向键移动 + 空格跳 + R 跑，落在地板上）---
    Entity characterEntity;
    {
        characterEntity = world.CreateEntity("Character");
        world.AddComponent<TransformComponent>(characterEntity);
        auto* vis = world.AddComponent<SphereComponent>(characterEntity);
        vis->radius = 0.4f;
        vis->baseColorFactor = float4(0.3f, 0.7f, 1.0f, 1.0f);   // 浅蓝
        vis->castShadow = false;
        vis->OnCreate();
        auto* cc = world.AddComponent<CollisionComponent>(characterEntity);
        cc->shape  = CollisionShape::Capsule;   // 与视觉球同半径的胶囊
        cc->radius = 0.4f;
        cc->height = 1.6f;
        auto* cm = world.AddComponent<CharacterMovementComponent>(characterEntity);
        (void)cm;   // 默认参数即可
        auto* cx = world.GetComponent<TransformComponent>(characterEntity);
        if (cx) cx->position = float3(0.0f, 3.0f, 3.0f);   // 悬空出生，落到地板
        sceneGraph.SetParent(characterEntity, Entity{kInvalidEntity});
    }

    // --- 实例化网格（B1：100×100 = 10000 实例，单次 DrawIndexed）---
    Entity instancedEntity;
    std::vector<float4x4> instancedTransformsBackup;   // 面板显隐开关用
    {
        instancedEntity = world.CreateEntity("InstancedGrid");
        world.AddComponent<TransformComponent>(instancedEntity);
        auto* im = world.AddComponent<InstancedMeshComponent>(instancedEntity);
        im->baseColorFactor = float4(0.3f, 0.9f, 0.4f, 1.0f);   // 绿色
        im->metallicFactor  = 0.3f;
        im->roughnessFactor = 0.6f;
        const int kGrid = 100;
        for (int ix = 0; ix < kGrid; ++ix) {
            for (int iz = 0; iz < kGrid; ++iz) {
                float4x4 m(1.0f);
                m[0][0] = m[1][1] = m[2][2] = 0.5f;   // 边长 0.5 米
                m[3] = float4(-75.0f + ix * 1.5f, 0.35f, -75.0f + iz * 1.5f, 1.0f);
                instancedTransformsBackup.push_back(m);
            }
        }
        im->SetInstanceTransforms(instancedTransformsBackup);
        sceneGraph.SetParent(instancedEntity, Entity{kInvalidEntity});
        HE_CORE_INFO("实例化网格: {} 实例（单次 DrawIndexed）", im->GetInstanceCount());
    }

    // --- 骨骼网格（C1c：Fox 模型加载 + GPU 蒙皮动画播放演示）---
    Entity skeletalEntity;
    {
        he::asset::glTFResult foxResult = he::asset::LoadGLTF(
            world, sceneGraph, String(HUGE_CONTENT_DIR) + "Models/Skeletal/Fox.glb");
        if (foxResult.success && !foxResult.skeletons.empty()) {
            auto foxSkeleton = foxResult.skeletons[0];
            skeletalEntity = world.CreateEntity("SkeletalFox");
            world.AddComponent<TransformComponent>(skeletalEntity);
            auto* sm = world.AddComponent<SkeletalMeshComponent>(skeletalEntity);
            sm->SetSkeleton(foxSkeleton);
            sm->PlayClip(2, true);   // 默认播 Run（动作幅度最大）
            sm->baseColorFactor = float4(0.95f, 0.55f, 0.2f, 1.0f);   // 橙色狐狸
            sm->metallicFactor  = 0.0f;
            sm->roughnessFactor = 0.6f;
            auto* sx = world.GetComponent<TransformComponent>(skeletalEntity);
            if (sx) sx->position = float3(4.0f, 0.3f, -6.0f);
            sceneGraph.SetParent(skeletalEntity, Entity{kInvalidEntity});
            HE_CORE_INFO("骨骼网格已创建: {}（{} 关节, {} 剪辑）",
                foxSkeleton->name, foxSkeleton->joints.size(), foxSkeleton->clips.size());
        } else {
            HE_CORE_WARN("Fox 模型加载失败，跳过骨骼网格演示: {}", foxResult.error);
        }
    }

    // --- 样条网格（B2 遗留：沿样条生成的条带，如道路/赛道）---
    Entity splineRoadEntity;
    {
        // 样条控制点：一条绕场景的弯曲路径（切向留零 → 自动 Catmull-Rom 光滑）
        Entity splineEntity = world.CreateEntity("RoadSpline");
        auto* spline = world.AddComponent<SplineComponent>(splineEntity);
        spline->AddPoint(float3(-10.0f, 0.06f,  6.0f));
        spline->AddPoint(float3( -3.0f, 0.06f,  9.0f));
        spline->AddPoint(float3(  4.0f, 0.06f,  6.0f));
        spline->AddPoint(float3(  8.0f, 0.06f, -2.0f));
        spline->AddPoint(float3(  2.0f, 0.06f, -9.0f));
        spline->AddPoint(float3( -6.0f, 0.06f, -8.0f));
        sceneGraph.SetParent(splineEntity, Entity{kInvalidEntity});

        // 条带网格：宽 2 米、48 段，关联上面的样条（网格由 SplineMeshSystem 生成）
        splineRoadEntity = world.CreateEntity("RoadMesh");
        world.AddComponent<TransformComponent>(splineRoadEntity);
        auto* road = world.AddComponent<SplineMeshComponent>(splineRoadEntity);
        road->splineEntity     = splineEntity.id;
        road->width            = 2.0f;
        road->segments         = 48;
        road->uvTiling         = 0.5f;
        road->baseColorFactor  = float4(0.22f, 0.22f, 0.26f, 1.0f);   // 深灰路面
        road->roughnessFactor  = 0.9f;
        road->metallicFactor   = 0.0f;
        sceneGraph.SetParent(splineRoadEntity, Entity{kInvalidEntity});
        HE_CORE_INFO("样条网格（B2）: 沿 {} 控制点样条生成 {} 段条带",
                     spline->points.size(), road->segments);
    }

    // --- 方向光（恢复启用：测试 CSM 阴影，点光源已注释）---
    Entity mainLightEntity;
    DirectionalLight* mainDL = nullptr;
    {
        mainLightEntity = world.CreateEntity("DirectionalLight");
        world.AddComponent<TransformComponent>(mainLightEntity);
        mainDL = world.AddComponent<DirectionalLight>(mainLightEntity);
        mainDL->direction = float3(0.5f, -1.0f, 1.0f);
        mainDL->color     = float3(1.0f, 0.95f, 0.85f);
        mainDL->intensity = 5.0f;
        mainDL->castShadow = true;
        mainDL->syncWithPhysicalSky = false;   // 物理天空已注释，不启用天空同步
        mainDL->shadowBias = 0.0015f;   // 深度偏移（CSM 深度范围动态适配后的小偏移；调大测试地板自影）
        mainDL->shadowNormalBias = 0.05f;   // 法线偏移（沿法线偏移采样点，避免地板自影）
        sceneGraph.SetParent(mainLightEntity, Entity{kInvalidEntity});
    }

    // --- 聚光灯（测试 Spot 阴影）---
    Entity spotLightSphere;  // 可视化球体
    {
        Entity slEntity = world.CreateEntity("SpotLight");
        world.AddComponent<TransformComponent>(slEntity);
        auto* sl = world.AddComponent<SpotLight>(slEntity);
        sl->color      = float3(1.0f, 0.9f, 0.7f);   // 暖白
        sl->intensity  = 25.0f;
        sl->range      = 20.0f;
        sl->direction  = float3(-0.3f, -1.0f, 0.2f);  // 朝下偏 -x/z，照射金球区
        sl->innerConeAngle = 0.35f;
        sl->outerConeAngle = 0.6f;
        sl->castShadow = true;
        sl->shadowBias = 0.005f;
        auto* slTransform = world.GetComponent<TransformComponent>(slEntity);
        if (slTransform) slTransform->position = float3(0.5f, 12.0f, 2.0f);
        sceneGraph.SetParent(slEntity, Entity{kInvalidEntity});

        // 可视化球体
        spotLightSphere = world.CreateEntity("SpotLightSphere");
        world.AddComponent<TransformComponent>(spotLightSphere);
        auto* sphere = world.AddComponent<SphereComponent>(spotLightSphere);
        sphere->baseColorFactor = float4(0.0, 0.6, 1.0, 1.0);   // 蓝青色（区别于点光红球）
        sphere->radius = 0.15f;
        sphere->segmentCount = 12;
        sphere->ringCount = 6;
        sphere->castShadow = false;   // 光源可视化球不投射阴影
        sphere->OnCreate();
        auto* sphereTransform = world.GetComponent<TransformComponent>(spotLightSphere);
        if (sphereTransform) sphereTransform->position = float3(0.5f, 12.0f, 2.0f);
        sceneGraph.SetParent(spotLightSphere, Entity{kInvalidEntity});
    }

    // --- 矩形面光（测试 Rect 光照，带可视化球）---
    Entity rectLightSphere;  // 可视化球体
    {
        Entity rlEntity = world.CreateEntity("RectLight");
        world.AddComponent<TransformComponent>(rlEntity);
        auto* rl = world.AddComponent<RectLight>(rlEntity);
        rl->color      = float3(1.0f, 0.8f, 0.6f);   // 暖色
        rl->intensity  = 15.0f;
        rl->width      = 4.0f;
        rl->height     = 2.0f;
        rl->normal     = float3(0.0f, -1.0f, 0.0f);   // 朝下（照亮下方地板/金球区）
        rl->range      = 15.0f;
        rl->castShadow = true;   // Phase 2：Rect 阴影
        rl->softness   = 0.5f;   // 软阴影
        auto* rlTransform = world.GetComponent<TransformComponent>(rlEntity);
        if (rlTransform) rlTransform->position = float3(-2.0f, 6.0f, 4.0f);
        sceneGraph.SetParent(rlEntity, Entity{kInvalidEntity});

        // 可视化球体（绿色，区别于点光红球 / Spot 蓝球）
        rectLightSphere = world.CreateEntity("RectLightSphere");
        world.AddComponent<TransformComponent>(rectLightSphere);
        auto* sphere = world.AddComponent<SphereComponent>(rectLightSphere);
        sphere->baseColorFactor = float4(0.0, 1.0, 0.3, 1.0);
        sphere->radius = 0.15f;
        sphere->segmentCount = 12;
        sphere->ringCount = 6;
        sphere->castShadow = false;   // 光源可视化球不投射阴影
        sphere->OnCreate();
        auto* sphereTransform = world.GetComponent<TransformComponent>(rectLightSphere);
        if (sphereTransform) sphereTransform->position = float3(-2.0f, 6.0f, 4.0f);
        sceneGraph.SetParent(rectLightSphere, Entity{kInvalidEntity});
    }

    // --- 点光源（可调节，带可视化球）---
    Entity pointLightEntity;
    Entity pointLightSphere;  // 可视化球体
    {
        pointLightEntity = world.CreateEntity("PointLight");
        world.AddComponent<TransformComponent>(pointLightEntity);
        auto* pl = world.AddComponent<PointLight>(pointLightEntity);
        pl->color      = float3(1.0f, 0.6f, 0.3f);  // 暖橙色
        pl->intensity  = 30.0f;
        pl->range      = 15.0f;
        pl->castShadow = true;
        pl->shadowBias = 0.005f;
        auto* plTransform = world.GetComponent<TransformComponent>(pointLightEntity);
        if (plTransform) plTransform->position = float3(2.0f, 4.0f, 3.0f);
        sceneGraph.SetParent(pointLightEntity, Entity{kInvalidEntity});

        // 可视化球体
        pointLightSphere = world.CreateEntity("PointLightSphere");
        world.AddComponent<TransformComponent>(pointLightSphere);
        auto* sphere = world.AddComponent<SphereComponent>(pointLightSphere);
        sphere->baseColorFactor = float4(1.0, 0.0, 0.0, 1.0);
        sphere->radius = 0.15f;
        sphere->segmentCount = 12;
        sphere->ringCount = 6;
        sphere->castShadow = false;   // 光源可视化球不投射阴影
        sphere->OnCreate();
        auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphere);
        if (sphereTransform) sphereTransform->position = float3(2.0f, 4.0f, 3.0f);
        sceneGraph.SetParent(pointLightSphere, Entity{kInvalidEntity});
    }

    // --- 天空盒（从 skybox 目录加载 6 面纹理）---
    // 【已注释】测试点光阴影：移除天空盒（连同物理天空，场景无天空光照）
    if (true) {
        String skyDir = String(HUGE_CONTENT_DIR) + "Textures/skybox/";
        const char* faceFiles[6] = {
            "daylight0.png", "daylight1.png", "daylight2.png",
            "daylight3.png", "daylight4.png", "daylight5.png",
        };
        std::vector<u8> allFaces;
        u32 faceW = 0, faceH = 0;

        for (u32 f = 0; f < 6; ++f) {
            String path = skyDir + faceFiles[f];
            int w, h, ch;
            u8* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
            if (!pixels) { HE_CORE_WARN("Skybox face {} 加载失败: {}", f, path); break; }
            if (f == 0) { faceW = (u32)w; faceH = (u32)h; }
            usize byteSize = faceW * faceH * 4;
            allFaces.insert(allFaces.end(), pixels, pixels + byteSize);
            stbi_image_free(pixels);
        }

        if (!allFaces.empty()) {
            rhi::TextureDesc cmDesc;
            cmDesc.format=rhi::Format::RGBA8_UNORM;
            cmDesc.width=faceW;
            cmDesc.height=faceH;
            cmDesc.mipLevels=1;
            cmDesc.arrayLayers=6;
            cmDesc.usage=rhi::TextureUsage::ShaderResource|rhi::TextureUsage::Cubemap|rhi::TextureUsage::TransferDst;
            cmDesc.initialData=allFaces.data();
            auto cm = device->CreateTexture(cmDesc);
            rhi::SamplerDesc s;
            s.minFilter=s.magFilter=rhi::FilterMode::Linear;
            s.addressU=s.addressV=s.addressW=rhi::AddressMode::ClampToEdge;
            auto cs = device->CreateSampler(s);
            Entity e = world.CreateEntity("Skybox");
            world.AddComponent<TransformComponent>(e);
            auto* sc = world.AddComponent<SkyboxComponent>(e);
            sc->SetCubemap(std::move(cm), std::move(cs));
            sceneGraph.SetParent(e, Entity{kInvalidEntity});
        }
    }

    // --- 物理天空（Preetham 解析模型，可选，替代 Cubemap 天空盒）---
    // 需 r.PhysicalSky.Enable 1 + Forward 模式（r.Pipeline.Mode 0）才可见
    // 【已注释】测试点光阴影：移除天空环境光干扰（场景只剩点光照明）
    if (cvPhysicalSkyEnable.Get() == 1) {
        //Entity pe = world.CreateEntity("PhysicalSky");
        //world.AddComponent<TransformComponent>(pe);
        //auto* ps = world.AddComponent<PhysicalSkyComponent>(pe);
        //// 太阳方向对齐方向光意图 (0.5,-1,1)；方向光被禁用（测试点光阴影）时用默认太阳方向
        //ps->sunDirection = mainDL
        //    ? glm::normalize(-mainDL->direction)
        //    : float3(0.0f, 0.6f, 0.4f);
        //ps->turbidity    = 4.0f;   // 大气浑浊度（1=极清，5=霾，10=浓霾）
        //ps->groundAlbedo = 0.1f;   // 地面反照率
        //ps->intensity    = 1.0f;   // 天空整体亮度倍率
        //ps->sunIntensity = 1.0f;   // 太阳盘亮度倍率
        //sceneGraph.SetParent(pe, Entity{kInvalidEntity});
    }

    HE_CORE_INFO("Scene created: {} entities", world.GetEntityCount());

    // --- 5. 初始化前向管线 + 延迟管线 + 混合 RT 管线 + 全路径追踪管线 ---
    render::ForwardPipeline  forwardPipeline;
    render::DeferredPipeline deferredPipeline;
    render::HybridRTPipeline hybridPipeline;
    render::PathTracingPipeline pathTracingPipeline;
    forwardPipeline.Initialize(device.get());
    forwardPipeline.SetUseRenderGraph(false);
    forwardPipeline.SetMultiThreadedRecording(false);
    forwardPipeline.SetSwapChain(swapchain.get());
    forwardPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    deferredPipeline.Initialize(device.get());
    deferredPipeline.SetSwapChain(swapchain.get());
    deferredPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    hybridPipeline.Initialize(device.get());
    hybridPipeline.SetSwapChain(swapchain.get());
    hybridPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    // 全路径追踪管线（r.Pipeline.Mode=3，设备支持 RT 时才可用）
    pathTracingPipeline.Initialize(device.get());
    pathTracingPipeline.SetSwapChain(swapchain.get());
    pathTracingPipeline.OnResize(swapchain->GetWidth(), swapchain->GetHeight());

    // --- 贴花（A3：地板路面标线式棋盘格纹理片，半透明混合）---
    // 纹理注册须在管线初始化之后（bindless 槽 0-3 保留给管线占位纹理）；
    // 纹理/采样器由 main 作用域持有整个运行期（bindless 堆只存裸指针）
    std::unique_ptr<rhi::IRHITexture> decalTextureOwn;
    std::unique_ptr<rhi::IRHISampler> decalSamplerOwn;
    {
        String texPath = String(HUGE_CONTENT_DIR) + "Generated/tex_gen_checker.png";
        int w = 0, h = 0, ch = 0;
        u8* pixels = stbi_load(texPath.c_str(), &w, &h, &ch, 4);
        if (pixels) {
            rhi::TextureDesc td;
            td.format = rhi::Format::RGBA8_UNORM;
            td.width = (u32)w;
            td.height = (u32)h;
            td.mipLevels = 1;
            td.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
            td.initialData = pixels;
            decalTextureOwn = device->CreateTexture(td);
            stbi_image_free(pixels);

            rhi::SamplerDesc sd;
            sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
            sd.addressU = sd.addressV = rhi::AddressMode::Repeat;
            decalSamplerOwn = device->CreateSampler(sd);

            // materialID = baseColor 槽句柄；textureMask 只置 bit0，其余槽不会被采样
            auto* heap = device->GetBindlessHeap();
            u32 matID = heap->RegisterTexture(decalTextureOwn.get(), decalSamplerOwn.get());

            Entity de = world.CreateEntity("CheckerDecal");
            world.AddComponent<TransformComponent>(de);
            auto* decal = world.AddComponent<DecalComponent>(de);
            decal->size         = float2(6.0f, 6.0f);
            decal->opacity      = 0.6f;
            decal->decalTexture = texPath;
            decal->materialID   = matID;
            decal->OnCreate();
            auto* dx = world.GetComponent<TransformComponent>(de);
            if (dx) {
                // 平放于地面（地板顶面 y=0.1，贴花略抬高避免 z-fighting），法线朝上
                dx->position = float3(0.0f, 0.12f, 0.0f);
                dx->rotation = glm::angleAxis(glm::radians(-90.0f), float3(1.0f, 0.0f, 0.0f));
            }
            sceneGraph.SetParent(de, Entity{kInvalidEntity});
            HE_CORE_INFO("贴花已创建: {} ({}x{})", texPath, w, h);
        } else {
            HE_CORE_WARN("贴花纹理加载失败（跳过）: {}", texPath);
        }
    }

    // 启动时默认关闭 GPU 剔除和 CPU 视锥剔除
    forwardPipeline.GetGPUCulling().enabled = false;
    deferredPipeline.GetGPUCulling().enabled = false;
    hybridPipeline.GetGPUCulling().enabled = false;
    forwardPipeline.GetSceneRenderer().enableFrustumCull = false;
    deferredPipeline.GetSceneRenderer().enableFrustumCull = false;

    // ============================================================
    // 5.5 粒子系统测试
    // ============================================================
    if (0)
    {
        Entity particleEntity = world.CreateEntity("TestParticle");
        auto* ptTransform = world.AddComponent<TransformComponent>(particleEntity);
        ptTransform->position = float3(0.0f, 1.0f, 0.0f);  // 场景几何体同一高度，容易看到
        auto* pc = world.AddComponent<ParticleComponent>(particleEntity);
        pc->GetParam().particlesPerSec  = 100.0f;
        pc->GetParam().minLifeTime      = 1.0f;
        pc->GetParam().maxLifeTime      = 3.0f;
        pc->GetParam().minInitSpeed     = 3.0f;
        pc->GetParam().maxInitSpeed     = 8.0f;
        pc->GetParam().emitShape        = EmitShapeType::Sphere;
        pc->GetParam().sphereRadius     = 2.0f;   // 扩散半径
        pc->GetParam().emitDirectionType = EmitDirectionType::Uniform_3D;  // 全方向
        pc->GetParam().gravity          = float3(0, 0, 0);   // 无重力，向四周飞散
        pc->GetParam().duration         = -1.0f;  // 无限持续
        pc->GetParam().minSize          = 0.15f;   // 最小粒子
        pc->GetParam().maxSize          = 0.5f;    // 最大粒子
        pc->GetParam().startColor       = float4(1.0f, 0.9f, 0.2f, 1.0f);  // 出生：亮金色
        pc->GetParam().endColor         = float4(1.0f, 0.3f, 0.05f, 0.0f); // 死亡：暗红透明
        pc->Play();
        sceneGraph.SetParent(particleEntity, Entity{kInvalidEntity});

        // 注册到延迟渲染管线、混合 RT 管线与全路径追踪管线
        // （粒子系统在 Deferred / PathTracing 模式下生效；HybridRT 暂未接入渲染）
        u32 pid = deferredPipeline.GetParticleRenderer().RegisterComponent(pc, device.get());
        deferredPipeline.AddParticleComponent(pid);
        hybridPipeline.AddParticleComponent(pid);
        u32 ptPid = pathTracingPipeline.GetParticleRenderer().RegisterComponent(pc, device.get());
        pathTracingPipeline.AddParticleComponent(ptPid);

        HE_CORE_INFO("粒子系统已注册: Deferred/HybridRT id={} PT id={} maxParticles={}",
                     pid, ptPid, pc->GetMaxParticles());
    }

    // --- 6. 创建命令列表 ---
    auto cmdList = device->CreateCommandList();
    cmdList->SetSwapChain(swapchain.get());
    cmdList->SetPipeline(forwardPipeline.GetPipelineState());

    // --- 6.5. 获取 GLFW 窗口句柄 ---
    GLFWwindow* glfwWin = engine.GetWindow()->GetNativeHandle();

    // --- 6.6. ImGui ---
    editor::ImGuiIntegration imgui;
    imgui.Initialize(glfwWin, device.get(), swapchain.get());

    // --- 7. 相机 ---
    render::CameraController camCtrl;
    camCtrl.SetAspectRatio(
        static_cast<float>(swapchain->GetWidth()),
        static_cast<float>(swapchain->GetHeight()));

    // 从配置文件加载全部面板参数（相机 + 渲染模式 + 天空盒 + 各光源）
    auto cfgData = LoadConfigFile(g_ConfigPath);
    if (!cfgData.empty()) {
        // 相机
        camCtrl.SetPosition(float3(
            GetFloat(cfgData, "cam_pos_x", 0.0f),
            GetFloat(cfgData, "cam_pos_y", 3.0f),
            GetFloat(cfgData, "cam_pos_z", 0.0f)));
        camCtrl.SetOrientation(
            GetFloat(cfgData, "cam_yaw", -1.57f),
            GetFloat(cfgData, "cam_pitch", -0.1f));
        auto& cam = camCtrl.GetCamera();
        cam.nearPlane = GetFloat(cfgData, "cam_near", cam.nearPlane);
        cam.farPlane  = GetFloat(cfgData, "cam_far",  cam.farPlane);
        cam.fov       = GetFloat(cfgData, "cam_fov",  cam.fov);
        // 渲染模式
        cvPipelineMode.Set(static_cast<int>(GetFloat(cfgData, "render.pipelineMode", static_cast<float>(cvPipelineMode.Get()))));
        // 天空盒
        world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sb) {
            sb.enabled = GetFloat(cfgData, "skybox.enabled", sb.enabled ? 1.0f : 0.0f) > 0.5f;
        });
        // 方向光
        world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& dl) {
            dl.enabled    = GetFloat(cfgData, "dir.enabled", dl.enabled ? 1.0f : 0.0f) > 0.5f;
            dl.direction  = glm::normalize(float3(
                GetFloat(cfgData, "dir.dir_x", dl.direction.x),
                GetFloat(cfgData, "dir.dir_y", dl.direction.y),
                GetFloat(cfgData, "dir.dir_z", dl.direction.z)));
            dl.color      = float3(
                GetFloat(cfgData, "dir.color_r", dl.color.x),
                GetFloat(cfgData, "dir.color_g", dl.color.y),
                GetFloat(cfgData, "dir.color_b", dl.color.z));
            dl.intensity  = GetFloat(cfgData, "dir.intensity", dl.intensity);
            dl.castShadow = GetFloat(cfgData, "dir.castShadow", dl.castShadow ? 1.0f : 0.0f) > 0.5f;
            dl.shadowBias = GetFloat(cfgData, "dir.bias", dl.shadowBias);
            dl.shadowNormalBias = GetFloat(cfgData, "dir.normalBias", dl.shadowNormalBias);
            dl.shadowStrength   = GetFloat(cfgData, "dir.strength", dl.shadowStrength);
        });
        // 点光源
        world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
            pl.enabled    = GetFloat(cfgData, "pt.enabled", pl.enabled ? 1.0f : 0.0f) > 0.5f;
            if (auto* t = world.GetComponent<TransformComponent>(e)) {
                t->position = float3(
                    GetFloat(cfgData, "pt.pos_x", t->position.x),
                    GetFloat(cfgData, "pt.pos_y", t->position.y),
                    GetFloat(cfgData, "pt.pos_z", t->position.z));
            }
            pl.color      = float3(
                GetFloat(cfgData, "pt.color_r", pl.color.x),
                GetFloat(cfgData, "pt.color_g", pl.color.y),
                GetFloat(cfgData, "pt.color_b", pl.color.z));
            pl.intensity  = GetFloat(cfgData, "pt.intensity", pl.intensity);
            pl.range      = GetFloat(cfgData, "pt.range", pl.range);
            pl.castShadow = GetFloat(cfgData, "pt.castShadow", pl.castShadow ? 1.0f : 0.0f) > 0.5f;
            pl.shadowBias = GetFloat(cfgData, "pt.bias", pl.shadowBias);
        });
        // 聚光灯
        world.ForEach<he::SpotLight>([&](he::Entity e, he::SpotLight& sl) {
            sl.enabled = GetFloat(cfgData, "spot.enabled", sl.enabled ? 1.0f : 0.0f) > 0.5f;
            if (auto* t = world.GetComponent<TransformComponent>(e)) {
                t->position = float3(
                    GetFloat(cfgData, "spot.pos_x", t->position.x),
                    GetFloat(cfgData, "spot.pos_y", t->position.y),
                    GetFloat(cfgData, "spot.pos_z", t->position.z));
            }
            sl.direction = glm::normalize(float3(
                GetFloat(cfgData, "spot.dir_x", sl.direction.x),
                GetFloat(cfgData, "spot.dir_y", sl.direction.y),
                GetFloat(cfgData, "spot.dir_z", sl.direction.z)));
            sl.color      = float3(
                GetFloat(cfgData, "spot.color_r", sl.color.x),
                GetFloat(cfgData, "spot.color_g", sl.color.y),
                GetFloat(cfgData, "spot.color_b", sl.color.z));
            sl.intensity = GetFloat(cfgData, "spot.intensity", sl.intensity);
            sl.range     = GetFloat(cfgData, "spot.range", sl.range);
            sl.innerConeAngle = GetFloat(cfgData, "spot.inner", sl.innerConeAngle);
            sl.outerConeAngle = GetFloat(cfgData, "spot.outer", sl.outerConeAngle);
            sl.castShadow = GetFloat(cfgData, "spot.castShadow", sl.castShadow ? 1.0f : 0.0f) > 0.5f;
            sl.shadowBias = GetFloat(cfgData, "spot.bias", sl.shadowBias);
        });
        // 矩形面光
        world.ForEach<he::RectLight>([&](he::Entity e, he::RectLight& rl) {
            rl.enabled = GetFloat(cfgData, "rect.enabled", rl.enabled ? 1.0f : 0.0f) > 0.5f;
            if (auto* t = world.GetComponent<TransformComponent>(e)) {
                t->position = float3(
                    GetFloat(cfgData, "rect.pos_x", t->position.x),
                    GetFloat(cfgData, "rect.pos_y", t->position.y),
                    GetFloat(cfgData, "rect.pos_z", t->position.z));
            }
            rl.normal = glm::normalize(float3(
                GetFloat(cfgData, "rect.normal_x", rl.normal.x),
                GetFloat(cfgData, "rect.normal_y", rl.normal.y),
                GetFloat(cfgData, "rect.normal_z", rl.normal.z)));
            rl.color      = float3(
                GetFloat(cfgData, "rect.color_r", rl.color.x),
                GetFloat(cfgData, "rect.color_g", rl.color.y),
                GetFloat(cfgData, "rect.color_b", rl.color.z));
            rl.intensity = GetFloat(cfgData, "rect.intensity", rl.intensity);
            rl.width     = GetFloat(cfgData, "rect.width", rl.width);
            rl.height    = GetFloat(cfgData, "rect.height", rl.height);
            rl.range     = GetFloat(cfgData, "rect.range", rl.range);
            rl.softness  = GetFloat(cfgData, "rect.softness", rl.softness);
            rl.castShadow = GetFloat(cfgData, "rect.castShadow", rl.castShadow ? 1.0f : 0.0f) > 0.5f;
        });
        // GI 强度
        if (auto* gi = forwardPipeline.GetGI()) {
            auto gs = gi->GetSettings();
            gs.intensity = GetFloat(cfgData, "gi.intensity", gs.intensity);
            gi->SetSettings(gs);
        }
        HE_CORE_INFO("加载面板参数: {}", g_ConfigPath);
    }

    // 鼠标状态
    bool   rightMouseDown = false;
    double lastMouseX = 0.0, lastMouseY = 0.0;

    // --- 8. 窗口调整回调 ---
    engine.GetWindow()->SetResizeCallback([&](u32 w, u32 h) {
        if (w == 0 || h == 0) return;
        swapchain->Resize(w, h);
        cmdList->SetSwapChain(swapchain.get());
        forwardPipeline.OnResize(w, h);
        deferredPipeline.OnResize(w, h);
        hybridPipeline.OnResize(w, h);
        pathTracingPipeline.OnResize(w, h);
        camCtrl.SetAspectRatio(static_cast<float>(w), static_cast<float>(h));
    });

    // --- 9. 主循环 ---
    HE_CORE_INFO("02.Cube demo started — WASD=移动, 右键拖拽=旋转, 滚轮=缩放, Shift=加速");
    u64  frameIndex = 0;
    f64  lastTime   = glfwGetTime();

    while (!engine.GetWindow()->ShouldClose()) {
        // 计算帧时间
        f64 now       = glfwGetTime();
        f32 deltaTime = static_cast<f32>(now - lastTime);
        lastTime      = now;

        engine.GetWindow()->PollEvents();

        if (!swapchain->AcquireNextImage())
            continue;

        // ============================================================
        // 相机控制
        // ============================================================
        {
            // --- 鼠标旋转（右键拖拽）---
            bool mouseDown = glfwGetMouseButton(glfwWin, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

            if (mouseDown && !rightMouseDown) {
                rightMouseDown = true;
                glfwGetCursorPos(glfwWin, &lastMouseX, &lastMouseY);
                glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            } else if (!mouseDown && rightMouseDown) {
                rightMouseDown = false;
                glfwSetInputMode(glfwWin, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            } else if (mouseDown && rightMouseDown) {
                double cx, cy;
                glfwGetCursorPos(glfwWin, &cx, &cy);
                float dx = static_cast<float>(cx - lastMouseX);
                float dy = static_cast<float>(cy - lastMouseY);
                lastMouseX = cx;
                lastMouseY = cy;

                camCtrl.Rotate(dx * 0.003f, -dy * 0.003f);  // 上推鼠标(dy<0) → pitch增大
            }

            // --- 键盘移动 ---
            render::CameraController::MoveInput moveIn;
            moveIn.forward  = glfwGetKey(glfwWin, GLFW_KEY_W) == GLFW_PRESS;
            moveIn.backward = glfwGetKey(glfwWin, GLFW_KEY_S) == GLFW_PRESS;
            moveIn.left     = glfwGetKey(glfwWin, GLFW_KEY_A) == GLFW_PRESS;
            moveIn.right    = glfwGetKey(glfwWin, GLFW_KEY_D) == GLFW_PRESS;
            moveIn.up       = glfwGetKey(glfwWin, GLFW_KEY_E) == GLFW_PRESS;
            moveIn.down     = glfwGetKey(glfwWin, GLFW_KEY_Q) == GLFW_PRESS;
            moveIn.sprint   = glfwGetKey(glfwWin, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;

            camCtrl.Update(deltaTime, moveIn);
        }

        // ============================================================
        // 碰撞检测演示（B5）：IJKL 移动探测球，重叠变绿
        // ============================================================
        {
            auto* px = world.GetComponent<TransformComponent>(collisionProbeEntity);
            if (px) {
                float3 move(0.0f);
                if (glfwGetKey(glfwWin, GLFW_KEY_J) == GLFW_PRESS) move.x -= 1.0f;
                if (glfwGetKey(glfwWin, GLFW_KEY_L) == GLFW_PRESS) move.x += 1.0f;
                if (glfwGetKey(glfwWin, GLFW_KEY_I) == GLFW_PRESS) move.z += 1.0f;
                if (glfwGetKey(glfwWin, GLFW_KEY_K) == GLFW_PRESS) move.z -= 1.0f;
                if (glm::dot(move, move) > 0.0f)
                    px->position += glm::normalize(move) * 5.0f * deltaTime;

                // 每帧检测：探测球（Sphere）vs 碰撞盒（AABB）
                bool overlap = he::CollisionSystem::Overlap(world, collisionProbeEntity, collisionBoxEntity);
                auto* ps = world.GetComponent<SphereComponent>(collisionProbeEntity);
                if (ps) {
                    ps->baseColorFactor = overlap
                        ? float4(0.2f, 1.0f, 0.3f, 1.0f)    // 绿 = 重叠
                        : float4(1.0f, 0.2f, 0.2f, 1.0f);   // 红 = 未重叠
                }
                static bool s_LastOverlap = true;
                if (overlap != s_LastOverlap) {
                    s_LastOverlap = overlap;
                    HE_CORE_INFO("[Collision] 探测球与碰撞盒 {} 重叠",
                        overlap ? "开始" : "结束");
                }
            }
        }

        // ============================================================
        // 角色移动演示（B3）：方向键移动 + 空格跳 + R 跑
        // ============================================================
        {
            auto* cm = world.GetComponent<CharacterMovementComponent>(characterEntity);
            if (cm) {
                cm->inputDirection = float2(0.0f);
                if (glfwGetKey(glfwWin, GLFW_KEY_LEFT)  == GLFW_PRESS) cm->inputDirection.x -= 1.0f;
                if (glfwGetKey(glfwWin, GLFW_KEY_RIGHT) == GLFW_PRESS) cm->inputDirection.x += 1.0f;
                if (glfwGetKey(glfwWin, GLFW_KEY_UP)    == GLFW_PRESS) cm->inputDirection.y -= 1.0f;
                if (glfwGetKey(glfwWin, GLFW_KEY_DOWN)  == GLFW_PRESS) cm->inputDirection.y += 1.0f;
                cm->bRunning   = glfwGetKey(glfwWin, GLFW_KEY_R) == GLFW_PRESS;
                if (glfwGetKey(glfwWin, GLFW_KEY_SPACE) == GLFW_PRESS) cm->bWantsJump = true;
            }
            he::MovementSystem::Update(world, deltaTime);
        }

        // 物理系统（Jolt C2）：RigidBody 刚体 + CollisionComponent 静态碰撞体步进/回写
        he::physics::PhysicsSystem::Update(world, sceneGraph, deltaTime);

        // 寻路系统（C4）：NavAgent 沿 NavMesh A* 路径移动
        he::NavAgentSystem::Update(world, sceneGraph, deltaTime);

        // 样条网格（B2 遗留）：样条数据变化时重建条带几何（版本脏检测，未变化则零成本）
        he::SplineMeshSystem::Update(world, device.get());

        // 帧相机解析（S0.4）：场景含主相机实体时优先使用，否则回退自由相机
        // 本示例场景无 CameraComponent 实体，行为与之前一致（始终走 CameraController 回退）
        render::CameraData frameCamera = render::ResolveFrameCamera(world, camCtrl.GetCamera());

        // 3D 文字系统：脏标记 → 重栅格化 → 纹理更新（A5）
        he::TextRenderSystem::Update(world, device.get());

        // 骨骼动画系统（C1c）：剪辑时间推进 + 关节蒙皮矩阵
        he::SkeletalMeshSystem::Update(world, deltaTime);

        // 每 2 秒刷新 FPS 文字（验证实时文字更新路径；
        // bindless 堆 append-only，更新会追加槽位，故降低刷新频率）
        {
            static f32 s_TextTimer = 0.0f;
            s_TextTimer += deltaTime;
            if (s_TextTimer >= 2.0f) {
                s_TextTimer = 0.0f;
                if (auto* tr = world.GetComponent<TextRenderComponent>(fpsTextEntity)) {
                    f64 fps = 1.0 / (deltaTime > 0 ? deltaTime : 0.016);
                    tr->text = "FPS: " + std::to_string((int)fps);
                }
            }
        }

        // 交换链实际颜色格式（SDR=BGRA8，HDR=A2B10G10R10），主循环开头取一次
        rhi::Format backFmt = swapchain->GetColorFormat();

        // 渲染一帧
        cmdList->Begin();

        // --- Forward 模式 ---
        if (cvPipelineMode.Get() == 0) {
            forwardPipeline.NextFrame();

            auto* shadowSys = forwardPipeline.GetShadowSystem();
            shadowSys->SetRenderResources(
                forwardPipeline.GetCurrentShadowObjectBuffer(),
                forwardPipeline.GetCurrentShadowBuffer(),
                forwardPipeline.GetCurrentDescSet());

            render::SubsystemContext shadowCtx;
            shadowCtx.world = &world;
            shadowCtx.sceneGraph = &sceneGraph;
            shadowCtx.camera = &frameCamera;
            he::SyncPhysicalSkyToSun(world);   // 在阴影烘焙前同步太阳方向，保证阴影/光照同向
            shadowSys->Update(shadowCtx);

            forwardPipeline.Render(cmdList.get(), world, sceneGraph, frameCamera);
            // pass 级调试标记：BackBuffer 合成（ToneMap + ImGui），RenderDoc 可识别
            cmdList->BeginDebugLabel("ToneMap + ImGui (BackBuffer)");
            cmdList->BeginRenderPass(1, backFmt);
            forwardPipeline.RenderToneMapPass(cmdList.get());
        }
        // --- Deferred 模式 ---
        else if (cvPipelineMode.Get() == 1) {
            deferredPipeline.NextFrame();
            deferredPipeline.Render(cmdList.get(), world, sceneGraph, frameCamera, deltaTime);
            // ImGui 叠加：Deferred 已写 BackBuffer，Load 保留内容
            cmdList->BeginDebugLabel("Deferred + ImGui (BackBuffer)");
            cmdList->BeginRenderPass(1, backFmt,
                rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);
        }
        // --- Hybrid RT 模式 ---
        else if (cvPipelineMode.Get() == 2) {
            hybridPipeline.NextFrame();
            hybridPipeline.Render(cmdList.get(), world, sceneGraph, frameCamera, deltaTime);
            // ImGui 叠加：管线已写 BackBuffer，Load 保留内容
            cmdList->BeginDebugLabel("HybridRT + ImGui (BackBuffer)");
            cmdList->BeginRenderPass(1, backFmt,
                rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);
        }
        // --- 全路径追踪模式（Level 2: PT 参考） ---
        else if (cvPipelineMode.Get() == 3) {
            pathTracingPipeline.NextFrame();
            pathTracingPipeline.Render(cmdList.get(), world, sceneGraph, frameCamera, deltaTime);
            // ImGui 叠加：管线已写 BackBuffer，Load 保留内容
            cmdList->BeginDebugLabel("PathTrace + ImGui (BackBuffer)");
            cmdList->BeginRenderPass(1, backFmt,
                rhi::Format::Unknown, nullptr, rhi::LoadOp::Load);
        }
        imgui.BeginFrame();
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Once);
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("HugEngine");
        ImGui::Text("FPS: %.0f", 1.0f / (deltaTime > 0 ? deltaTime : 0.016f));
        // ============================================================
        // 相机信息（可编辑）
        // ============================================================
        ImGui::SeparatorText("相机");
        auto& cam = camCtrl.GetCamera();

        // 位置
        ImGui::DragFloat3("位置", &cam.position[0], 1.0f);

        // 旋转（Yaw / Pitch 角度制）
        float yawDeg   = glm::degrees(camCtrl.GetYaw());
        float pitchDeg = glm::degrees(camCtrl.GetPitch());
        bool yawChanged   = ImGui::SliderFloat("Yaw",   &yawDeg,   -180.0f, 180.0f, "%.1f°");
        bool pitchChanged = ImGui::SliderFloat("Pitch", &pitchDeg, -89.0f,  89.0f,  "%.1f°");
        if (yawChanged || pitchChanged)
            camCtrl.SetOrientation(glm::radians(yawDeg), glm::radians(pitchDeg));

        // 朝向 / 上方向（只读，由 Yaw/Pitch 导出）
        ImGui::Text("朝向: (%.2f, %.2f, %.2f)", cam.forward.x, cam.forward.y, cam.forward.z);

        // FOV、裁剪面
        ImGui::SliderFloat("FOV", &cam.fov, 10.0f, 120.0f, "%.0f°");
        ImGui::DragFloat("近裁剪面", &cam.nearPlane, 0.01f, 0.001f, 10.0f, "%.3f",
            ImGuiSliderFlags_Logarithmic);
        ImGui::DragFloat("远裁剪面", &cam.farPlane, 10.0f, 10.0f, 50000.0f, "%.0f");

        // 宽高比 / 移动速度
        ImGui::Text("宽高比: %.2f", cam.aspectRatio);
        float speed = camCtrl.GetMoveSpeed();
        if (ImGui::DragFloat("移动速度", &speed, 1.0f, 0.1f, 500.0f, "%.1f"))
            camCtrl.SetMoveSpeed(speed);

        // 碰撞检测演示状态（B5）
        {
            bool overlap = he::CollisionSystem::Overlap(world, collisionProbeEntity, collisionBoxEntity);
            ImGui::SeparatorText("碰撞检测 (B5)");
            ImGui::TextWrapped("IJKL 移动红球，碰到橙色碰撞盒变绿");
            ImGui::TextColored(overlap ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "当前状态: %s", overlap ? "重叠" : "未重叠");
        }

        // 实例化网格显隐（B1：万级实例 FPS 对比）
        if (auto* im = world.GetComponent<InstancedMeshComponent>(instancedEntity)) {
            ImGui::SeparatorText("实例化网格 (B1)");
            bool visible = im->GetInstanceCount() > 0;
            if (ImGui::Checkbox("显示 10000 实例", &visible)) {
                im->SetInstanceTransforms(visible ? instancedTransformsBackup
                                                  : std::vector<float4x4>{});
            }
        }

        // 骨骼网格演示状态（C1c）
        if (auto* sm = world.GetComponent<SkeletalMeshComponent>(skeletalEntity)) {
            ImGui::SeparatorText("骨骼网格 (C1c)");
            const char* clipNames[] = { "绑定姿势", "Survey", "Walk", "Run" };
            int cur = sm->currentClip + 1;   // -1(绑定) → 0
            if (ImGui::Combo("剪辑", &cur, clipNames, 4)) {
                if (cur == 0) sm->currentClip = -1;
                else sm->PlayClip(cur - 1, sm->looping);
            }
            ImGui::Checkbox("播放", &sm->playing);
            ImGui::SameLine();
            ImGui::Checkbox("循环", &sm->looping);
            ImGui::SliderFloat("速度", &sm->playSpeed, 0.1f, 3.0f);
            ImGui::Text("时间: %.2fs | 关节: %d", sm->clipTime,
                (int)sm->jointWorldMatrices.size());
        }

        // 角色移动演示状态（B3）
        {
            auto* cm = world.GetComponent<CharacterMovementComponent>(characterEntity);
            if (cm) {
                ImGui::SeparatorText("角色移动 (B3)");
                ImGui::TextWrapped("方向键移动浅蓝球，空格跳跃，R 跑");
                ImGui::TextColored(cm->bOnGround ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                    "状态: %s", cm->bOnGround ? "地面" : "空中");
                ImGui::Text("速度: (%.1f, %.1f, %.1f) m/s",
                    cm->velocity.x, cm->velocity.y, cm->velocity.z);
            }
        }

        // 物理系统 (C2)：激活刚体数
        ImGui::SeparatorText("物理 (C2)");
        ImGui::Text("激活刚体: %d", he::physics::PhysicsSystem::GetActiveBodyCount());

        // 渲染模式切换（读 CVar → Combo → 写回 CVar）
        int mode = cvPipelineMode.Get();
        ImGui::SeparatorText("渲染模式");

        // 下拉项：索引与模式值一一对应（0/1/2/3 连续）
        const char* modeNames[] = {
            "Forward 前向渲染",
            "Deferred 延迟渲染",
            "Hybrid RT 混合光追",
            "PathTrace 全路径追踪",
        };

        // 无 RT 能力时仅提供前两种模式
        const int itemCount = device->GetCaps().supportsRayTracing ? 4 : 2;

        // 无 RT 能力且 CVar 停留在 RT 模式(2/3)时回退到 Deferred，避免下拉框越界
        if (itemCount == 2 && mode >= 2) {
            mode = 1;
            cvPipelineMode.Set(mode);
        }

        // 仅在用户改动时写回 CVar
        if (ImGui::Combo("渲染管线", &mode, modeNames, itemCount))
            cvPipelineMode.Set(mode);

        // GPU 剔除开关
        ImGui::Spacing();
        bool gpuCullOn = forwardPipeline.GetGPUCulling().enabled;
        if (ImGui::Checkbox("GPU Culling", &gpuCullOn)) {
            forwardPipeline.GetGPUCulling().enabled = gpuCullOn;
            deferredPipeline.GetGPUCulling().enabled = gpuCullOn;
            hybridPipeline.GetGPUCulling().enabled = gpuCullOn;
        }
        // CPU 视锥剔除开关
        bool cpuCullOn = forwardPipeline.GetSceneRenderer().enableFrustumCull;
        if (ImGui::Checkbox("CPU Frustum Cull", &cpuCullOn)) {
            forwardPipeline.GetSceneRenderer().enableFrustumCull = cpuCullOn;
            deferredPipeline.GetSceneRenderer().enableFrustumCull = cpuCullOn;
        }

        // Hybrid RT 效果开关（仅 HybridRT 模式显示）
        if (cvPipelineMode.Get() == 2 && device->GetCaps().supportsRayTracing) {
            ImGui::SeparatorText("Hybrid RT 效果");
            bool rtShadowOn = hybridPipeline.IsRTShadowEnabled();
            if (ImGui::Checkbox("RT Shadow##RTShadow", &rtShadowOn))
                hybridPipeline.SetRTShadowEnabled(rtShadowOn);
            bool rtAOOn = hybridPipeline.IsRTAOEnabled();
            if (ImGui::Checkbox("RT AO##RTAO", &rtAOOn))
                hybridPipeline.SetRTAOEnabled(rtAOOn);
            bool rtReflOn = hybridPipeline.IsRTReflectionEnabled();
            if (ImGui::Checkbox("RT Reflection##RTReflection", &rtReflOn))
                hybridPipeline.SetRTReflectionEnabled(rtReflOn);
            bool rtGIOn = hybridPipeline.IsRTGIEnabled();
            if (ImGui::Checkbox("RT GI##RTGI", &rtGIOn))
                hybridPipeline.SetRTGIEnabled(rtGIOn);
        }

        // 天空盒开关（SkyboxPass 读取 enabled 决定是否渲染）
        world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sb) {
            ImGui::SeparatorText("天空盒");
            ImGui::Checkbox("启用##Skybox", &sb.enabled);
        });

        // GI 控制
        auto* gi = forwardPipeline.GetGI();
        if (gi) {
            ImGui::SeparatorText("GI");
            auto settings = gi->GetSettings();
            ImGui::Text("Mode: %s", settings.mode == render::GIMode::IBL ? "IBL" : "None");
            float intensity = settings.intensity;
            if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 3.0f, "%.2f")) {
                settings.intensity = intensity;
                gi->SetSettings(settings);
            }
        }

        // 方向光控制
        if (mainDL) {
            ImGui::SeparatorText("方向光");

            ImGui::Checkbox("启用##DLEnabled", &mainDL->enabled);

            float3 dir = mainDL->direction;
            if (ImGui::SliderFloat3("方向##DLDir", &dir[0], -1.0f, 1.0f, "%.2f")) {
                if (glm::dot(dir, dir) > 0.0001f)
                    mainDL->direction = glm::normalize(dir);
            }

            ImGui::ColorEdit3("颜色##DLColor", &mainDL->color[0]);

            ImGui::DragFloat("强度##DLIntensity", &mainDL->intensity, 0.1f, 0.0f, 50.0f, "%.1f");

            bool shadowOn = mainDL->castShadow;
            if (ImGui::Checkbox("投射阴影##DLShadow", &shadowOn))
                mainDL->castShadow = shadowOn;

            if (mainDL->castShadow) {
                ImGui::Indent(12.0f);
                ImGui::DragFloat("深度偏移##DLBias", &mainDL->shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::DragFloat("法线偏移##DLNormalBias", &mainDL->shadowNormalBias, 0.001f, 0.0f, 0.5f, "%.3f");
                ImGui::SliderFloat("阴影强度##DLShadowStr", &mainDL->shadowStrength, 0.0f, 1.0f, "%.2f");
                ImGui::Unindent(12.0f);
            }
        }

        // 点光源控制
        world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
            ImGui::SeparatorText("点光源");

            ImGui::Checkbox("启用##PTEnabled", &pl.enabled);

            auto* plTransform = world.GetComponent<TransformComponent>(e);
            if (plTransform) {
                if (ImGui::DragFloat3("位置##PTPos", &plTransform->position[0], 0.1f)) {
                    // 同步可视化球体位置
                    auto* sphereTransform = world.GetComponent<TransformComponent>(pointLightSphere);
                    if (sphereTransform)
                        sphereTransform->position = plTransform->position;
                }
            }

            ImGui::ColorEdit3("颜色##PTColor", &pl.color[0]);
            ImGui::DragFloat("强度##PTIntensity", &pl.intensity, 0.5f, 0.0f, 200.0f, "%.1f");
            ImGui::DragFloat("范围##PTRange", &pl.range, 0.5f, 0.5f, 50.0f, "%.1f");

            bool ptShadow = pl.castShadow;
            if (ImGui::Checkbox("投射阴影##PTShadow", &ptShadow))
                pl.castShadow = ptShadow;

            if (pl.castShadow) {
                ImGui::Indent(12.0f);
                ImGui::DragFloat("深度偏移##PTBias", &pl.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::Unindent(12.0f);
            }
        });

        // 聚光灯控制
        world.ForEach<he::SpotLight>([&](he::Entity e, he::SpotLight& sl) {
            ImGui::SeparatorText("聚光灯");

            ImGui::Checkbox("启用##SLEnabled", &sl.enabled);

            auto* slTransform = world.GetComponent<TransformComponent>(e);
            if (slTransform) {
                if (ImGui::DragFloat3("位置##SLPos", &slTransform->position[0], 0.1f)) {
                    // 同步可视化球体位置
                    auto* sphereTransform = world.GetComponent<TransformComponent>(spotLightSphere);
                    if (sphereTransform)
                        sphereTransform->position = slTransform->position;
                }
            }

            ImGui::SliderFloat3("方向##SLDir", &sl.direction[0], -1.0f, 1.0f, "%.2f");
            ImGui::ColorEdit3("颜色##SLColor", &sl.color[0]);
            ImGui::DragFloat("强度##SLIntensity", &sl.intensity, 0.5f, 0.0f, 200.0f, "%.1f");
            ImGui::DragFloat("范围##SLRange", &sl.range, 0.5f, 0.5f, 60.0f, "%.1f");
            ImGui::SliderFloat("内锥角##SLInner", &sl.innerConeAngle, 0.05f, 1.5f, "%.3f");
            ImGui::SliderFloat("外锥角##SLOuter", &sl.outerConeAngle, 0.1f, 1.6f, "%.3f");

            bool slShadow = sl.castShadow;
            if (ImGui::Checkbox("投射阴影##SLShadow", &slShadow))
                sl.castShadow = slShadow;

            if (sl.castShadow) {
                ImGui::Indent(12.0f);
                ImGui::DragFloat("深度偏移##SLBias", &sl.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::Unindent(12.0f);
            }
        });

        // 矩形面光控制
        world.ForEach<he::RectLight>([&](he::Entity e, he::RectLight& rl) {
            ImGui::SeparatorText("矩形面光");

            ImGui::Checkbox("启用##RLEnabled", &rl.enabled);

            auto* rlTransform = world.GetComponent<TransformComponent>(e);
            if (rlTransform) {
                if (ImGui::DragFloat3("位置##RLPos", &rlTransform->position[0], 0.1f)) {
                    // 同步可视化球体位置
                    auto* sphereTransform = world.GetComponent<TransformComponent>(rectLightSphere);
                    if (sphereTransform)
                        sphereTransform->position = rlTransform->position;
                }
            }

            ImGui::SliderFloat3("法线##RLNormal", &rl.normal[0], -1.0f, 1.0f, "%.2f");
            ImGui::ColorEdit3("颜色##RLColor", &rl.color[0]);
            ImGui::DragFloat("强度##RLIntensity", &rl.intensity, 0.5f, 0.0f, 200.0f, "%.1f");
            ImGui::DragFloat("宽度##RLWidth", &rl.width, 0.1f, 0.2f, 20.0f, "%.2f");
            ImGui::DragFloat("高度##RLHeight", &rl.height, 0.1f, 0.2f, 20.0f, "%.2f");
            ImGui::DragFloat("范围##RLRange", &rl.range, 0.5f, 0.5f, 60.0f, "%.1f");
            ImGui::SliderFloat("软阴影##RLSoftness", &rl.softness, 0.0f, 1.0f, "%.2f");
        });

        // PT 质量面板（仅 PathTrace 模式显示）
        if (cvPipelineMode.Get() == 3 && device->GetCaps().supportsRayTracing) {
            using namespace he::render;  // 直接读写 r.PT.Atrous.* CVar（本块作用域）
            ImGui::SeparatorText("PT 质量");
            int spp = pathTracingPipeline.GetPTSampleCount();
            if (ImGui::SliderInt("SPP", &spp, 1, 8)) pathTracingPipeline.SetPTSampleCount(spp);
            int bounces = pathTracingPipeline.GetPTMaxBounces();
            if (ImGui::SliderInt("Bounces", &bounces, 1, 8)) pathTracingPipeline.SetPTMaxBounces(bounces);
            bool denoiseOn = pathTracingPipeline.IsPTDenoise();
            if (ImGui::Checkbox("时域降噪", &denoiseOn))
                pathTracingPipeline.SetPTDenoise(denoiseOn);
            // A-Trous 空间滤波（时域降噪之后的多迭代边缘感知滤波）
            bool atrousOn = pathTracingPipeline.IsPTAtrous();
            if (ImGui::Checkbox("A-Trous 空间滤波", &atrousOn))
                pathTracingPipeline.SetPTAtrous(atrousOn);
            int atrousIter = cvPTAtrousIterations.Get();
            if (ImGui::SliderInt("A-Trous 迭代", &atrousIter, 1, 5)) {
                cvPTAtrousIterations.Set(atrousIter);
            }
            float atrousSigmaC = cvPTAtrousSigmaColor.Get();
            if (ImGui::SliderFloat("A-Trous 颜色 σ", &atrousSigmaC, 0.05f, 2.0f, "%.2f"))
                cvPTAtrousSigmaColor.Set(atrousSigmaC);
            float atrousClamp = cvPTAtrousClamp.Get();
            if (ImGui::SliderFloat("A-Trous 火萤钳制", &atrousClamp, 0.0f, 10.0f, "%.1f"))
                cvPTAtrousClamp.Set(atrousClamp);
            bool restirOn = pathTracingPipeline.IsPTReSTIR();
            if (ImGui::Checkbox("ReSTIR DI", &restirOn))
                pathTracingPipeline.SetPTReSTIR(restirOn);
            bool misOn = pathTracingPipeline.IsPTMIS();
            if (ImGui::Checkbox("MIS", &misOn))
                pathTracingPipeline.SetPTMIS(misOn);
            bool rrOn = pathTracingPipeline.IsPTRoulette();
            if (ImGui::Checkbox("俄罗斯轮盘赌", &rrOn))
                pathTracingPipeline.SetPTRoulette(rrOn);
            ImGui::Text("蓄水池: %s", pathTracingPipeline.IsReservoirReady() ? "有效" : "预热中/无效");
        }

        ImGui::End();
        imgui.EndFrame(cmdList.get());
        cmdList->EndDebugLabel();  // 闭合 BackBuffer pass 级标记
        cmdList->EndRenderPass();  // 关闭 ImGui RP（RG 和 non-RG 都需要）
        cmdList->End();

        device->Submit(cmdList.get());
        swapchain->Present(true);
        frameIndex++;

        // 每秒更新窗口标题，显示 FPS
        static f64  titleTimer  = 0.0;
        static u64  titleFrame  = 0;
        titleTimer += deltaTime;
        titleFrame++;
        if (titleTimer >= 0.5) {
            f64 fps = static_cast<f64>(titleFrame) / titleTimer;
            char buf[256];
            snprintf(buf, sizeof(buf),
                "HugEngine — PBR | FPS: %.0f | Pos: (%.1f, %.1f, %.1f) "
                "| 右键拖拽旋转 WASD移动",
                fps,
                camCtrl.GetCamera().position.x, camCtrl.GetCamera().position.y, camCtrl.GetCamera().position.z);
            glfwSetWindowTitle(glfwWin, buf);
            titleTimer = 0.0;
            titleFrame = 0;
        }
    }

    // 保存全部面板参数（相机 + 渲染模式 + 天空盒 + 各光源）到 ini
    {
        std::unordered_map<String, String> out;
        // 相机
        out["cam_pos_x"] = std::to_string(camCtrl.GetCamera().position.x);
        out["cam_pos_y"] = std::to_string(camCtrl.GetCamera().position.y);
        out["cam_pos_z"] = std::to_string(camCtrl.GetCamera().position.z);
        out["cam_yaw"]   = std::to_string(camCtrl.GetYaw());
        out["cam_pitch"] = std::to_string(camCtrl.GetPitch());
        out["cam_near"]  = std::to_string(camCtrl.GetCamera().nearPlane);
        out["cam_far"]   = std::to_string(camCtrl.GetCamera().farPlane);
        out["cam_fov"]   = std::to_string(camCtrl.GetCamera().fov);
        // 渲染模式
        out["render.pipelineMode"] = std::to_string(cvPipelineMode.Get());
        // 天空盒
        world.ForEach<he::SkyboxComponent>([&](he::Entity, he::SkyboxComponent& sb) {
            out["skybox.enabled"] = sb.enabled ? "1" : "0";
        });
        // 方向光
        world.ForEach<he::DirectionalLight>([&](he::Entity, he::DirectionalLight& dl) {
            out["dir.enabled"]    = dl.enabled ? "1" : "0";
            out["dir.dir_x"]      = std::to_string(dl.direction.x);
            out["dir.dir_y"]      = std::to_string(dl.direction.y);
            out["dir.dir_z"]      = std::to_string(dl.direction.z);
            out["dir.color_r"]    = std::to_string(dl.color.x);
            out["dir.color_g"]    = std::to_string(dl.color.y);
            out["dir.color_b"]    = std::to_string(dl.color.z);
            out["dir.intensity"]  = std::to_string(dl.intensity);
            out["dir.castShadow"] = dl.castShadow ? "1" : "0";
            out["dir.bias"]       = std::to_string(dl.shadowBias);
            out["dir.normalBias"] = std::to_string(dl.shadowNormalBias);
            out["dir.strength"]   = std::to_string(dl.shadowStrength);
        });
        // 点光源
        world.ForEach<he::PointLight>([&](he::Entity e, he::PointLight& pl) {
            out["pt.enabled"]   = pl.enabled ? "1" : "0";
            if (auto* t = world.GetComponent<TransformComponent>(e)) {
                out["pt.pos_x"] = std::to_string(t->position.x);
                out["pt.pos_y"] = std::to_string(t->position.y);
                out["pt.pos_z"] = std::to_string(t->position.z);
            }
            out["pt.color_r"]    = std::to_string(pl.color.x);
            out["pt.color_g"]    = std::to_string(pl.color.y);
            out["pt.color_b"]    = std::to_string(pl.color.z);
            out["pt.intensity"]  = std::to_string(pl.intensity);
            out["pt.range"]      = std::to_string(pl.range);
            out["pt.castShadow"] = pl.castShadow ? "1" : "0";
            out["pt.bias"]       = std::to_string(pl.shadowBias);
        });
        // 聚光灯
        world.ForEach<he::SpotLight>([&](he::Entity e, he::SpotLight& sl) {
            out["spot.enabled"] = sl.enabled ? "1" : "0";
            if (auto* t = world.GetComponent<TransformComponent>(e)) {
                out["spot.pos_x"] = std::to_string(t->position.x);
                out["spot.pos_y"] = std::to_string(t->position.y);
                out["spot.pos_z"] = std::to_string(t->position.z);
            }
            out["spot.dir_x"]     = std::to_string(sl.direction.x);
            out["spot.dir_y"]     = std::to_string(sl.direction.y);
            out["spot.dir_z"]     = std::to_string(sl.direction.z);
            out["spot.color_r"]   = std::to_string(sl.color.x);
            out["spot.color_g"]   = std::to_string(sl.color.y);
            out["spot.color_b"]   = std::to_string(sl.color.z);
            out["spot.intensity"] = std::to_string(sl.intensity);
            out["spot.range"]     = std::to_string(sl.range);
            out["spot.inner"]     = std::to_string(sl.innerConeAngle);
            out["spot.outer"]     = std::to_string(sl.outerConeAngle);
            out["spot.castShadow"] = sl.castShadow ? "1" : "0";
            out["spot.bias"]      = std::to_string(sl.shadowBias);
        });
        // 矩形面光
        world.ForEach<he::RectLight>([&](he::Entity e, he::RectLight& rl) {
            out["rect.enabled"] = rl.enabled ? "1" : "0";
            if (auto* t = world.GetComponent<TransformComponent>(e)) {
                out["rect.pos_x"] = std::to_string(t->position.x);
                out["rect.pos_y"] = std::to_string(t->position.y);
                out["rect.pos_z"] = std::to_string(t->position.z);
            }
            out["rect.normal_x"]    = std::to_string(rl.normal.x);
            out["rect.normal_y"]    = std::to_string(rl.normal.y);
            out["rect.normal_z"]    = std::to_string(rl.normal.z);
            out["rect.color_r"]     = std::to_string(rl.color.x);
            out["rect.color_g"]     = std::to_string(rl.color.y);
            out["rect.color_b"]     = std::to_string(rl.color.z);
            out["rect.intensity"]   = std::to_string(rl.intensity);
            out["rect.width"]       = std::to_string(rl.width);
            out["rect.height"]      = std::to_string(rl.height);
            out["rect.range"]       = std::to_string(rl.range);
            out["rect.softness"]    = std::to_string(rl.softness);
            out["rect.castShadow"]  = rl.castShadow ? "1" : "0";
        });
        // GI 强度
        if (auto* gi = forwardPipeline.GetGI())
            out["gi.intensity"] = std::to_string(gi->GetSettings().intensity);
        SaveConfigFile(g_ConfigPath, out);
        HE_CORE_INFO("面板参数已保存: {}", g_ConfigPath);
    }

    // 清理
    imgui.Shutdown();
    device->WaitIdle();

    forwardPipeline.Shutdown();
    deferredPipeline.Shutdown();
    hybridPipeline.Shutdown();
    pathTracingPipeline.Shutdown();

    HE_CORE_INFO("Exiting after {} frames", frameIndex);
    return 0;
}
