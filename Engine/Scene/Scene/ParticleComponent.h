#pragma once

// ============================================================
// ParticleComponent.h — GPU 粒子系统组件
//
// 参考 SeekEngine 实现，适配 HugEngine DeferredPipeline。
// 粒子数据全部在 GPU 端管理，CPU 只负责参数上传和 Dispatch。
// ============================================================

#include "Scene/Component.h"
#include "Scene/Transform.h"
#include "RHI/RHI.h"
#include "Core/Types.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace he {

// ============================================================
// 枚举类型 (与 ParticleTypes.slang 常量保持一致)
// ============================================================

enum class EmitDirectionType : u32 {
    Directional  = 0,   // 统一方向 + 扩散角度
    BiDirectional = 1,  // 双向
    Uniform_2D   = 2,   // 2D 圆盘随机
    Uniform_3D   = 3,   // 3D 球体随机
};

enum class EmitShapeType : u32 {
    Sphere = 0,
    Box    = 1,
};

enum class TexTimeSamplingType : u32 {
    Play_Once          = 0,
    Play_Loop          = 1,
    Random_Still_Frame = 2,
    Random_Loop        = 3,
};

enum class ParticleState : u32 {
    Stopped,
    Playing,
    Pause,
};

enum class ParticleMsgType : u32 {
    Start,
    Pause,
    Resume,
    Stop,
};

// ============================================================
// 渐变曲线类型
// ============================================================

using SizeOverLife  = std::vector<std::pair<float, float2>>;
using ColorOverLife = std::vector<std::pair<float, float4>>;

// ============================================================
// 粒子系统参数 (CPU 端配置)
// ============================================================

struct ParticleSystemParam {
    // 主参数
    float   duration           = -1.0f;  // -1 = 无限
    float   particlesPerSec    = 300.0f;
    float   minLifeTime        = 0.1f;
    float   maxLifeTime        = 1.0f;
    float   minInitSpeed       = 1.0f;
    float   maxInitSpeed       = 5.0f;

    // 发射器
    EmitDirectionType emitDirectionType = EmitDirectionType::Directional;
    float3            direction         = float3(0, 1, 0);
    float             directionSpread   = 50.0f;   // 百分比
    EmitShapeType     emitShape         = EmitShapeType::Sphere;
    float             sphereRadius      = 1.0f;
    float3            boxSize           = float3(1, 1, 0);

    // 纹理
    uint2             texRowsCols        = uint2(1, 1);
    float             texFramesPerSec    = 1.0f;
    TexTimeSamplingType texTimeSampling  = TexTimeSamplingType::Play_Once;
    rhi::DescriptorSetHandle particleTex = rhi::kInvalidSet;  // bindless 纹理句柄

    // 物理
    float3            gravity = float3(0, -9.8f, 0);

    // 生命周期渐变
    SizeOverLife      sizeOverLife;
    ColorOverLife     colorOverLife;
};

// ============================================================
// GPU 端数据结构 (与 ParticleTypes.slang 保持一致)
// ============================================================

// 粒子数据 (与 GPU ParticleTypes.slang 对齐)
struct alignas(16) Particle {
    float3 lifeTime;     // x: cur, y: total, z: tex time
    u32    texIndex;
    float4 velocity;     // xyz: vel, w: damping
    float4 position;     // xyz: world pos, w: size
};

// 全局计数器
struct alignas(16) ParticleCounters {
    u32 deadCount;
    u32 aliveCount[2];   // [0]=pre_sim, [1]=post_sim
    u32 emitCount;
    u32 simulateCount;
    u32 renderCount;
    uint2 pad;
};

// 排序条目 (8 bytes)
struct SortInfo {
    u32   particleIndex;
    float particleDepth;
};

// ============================================================
// GPU 参数结构体 (std140 布局, 与 ParticleTypes.slang 对应)
// ============================================================

struct alignas(16) GpuEmitParam {
    float3 position;        u32   maxParticles;
    u32    emitDirectionType; float3 direction;
    float  directionSpreadPercent;
    float  minInitSpeed;    float maxInitSpeed;
    float  minLifeTime;     float maxLifeTime;
    float3 boxSize;         i32   emitShape;
    float  sphereRadius;    uint2 texRowsCols;
    u32    texTimeSampling; float3 pad;
};

struct alignas(16) GpuSimulateParam {
    float  deltaTime;       float3 gravity;
    u32    texTimeSampling; uint2  texRowsCols;
    float  texFramesPerSec;
};

struct alignas(16) GpuCullingParam {
    float4x4 viewProj;
    float4   frustumPlanes[6];
};

struct alignas(16) GpuRenderParam {
    float4x4 viewMatrix;
    float4x4 projMatrix;
    uint2    texRowsCols;
    uint2    pad;
};

struct alignas(16) GpuSortParam {
    u32 sortLevel;
    u32 descendMask;
    u32 matrixWidth;
    u32 matrixHeight;
};

struct alignas(16) DispatchArgs {
    u32 dispatchX, dispatchY, dispatchZ;
};

struct alignas(16) ParticleDrawArgs {
    u32 vertexCount;
    u32 instanceCount;
    u32 firstVertex;
    u32 firstInstance;
};

// ============================================================
// 粒子回调
// ============================================================

using ParticleCallback = std::function<void(const std::string& name, ParticleMsgType msg)>;

// ============================================================
// ParticleComponent — GPU 粒子系统组件
// ============================================================

class ParticleComponent : public Component {
    HE_COMPONENT()

public:
    ParticleComponent() = default;
    ~ParticleComponent() override = default;

    // 生命周期
    void Play();
    void Pause();
    void Resume();
    void Stop();

    // 参数
    ParticleSystemParam& GetParam() { return m_Param; }
    const ParticleSystemParam& GetParam() const { return m_Param; }
    ParticleState GetState() const { return m_ParticleState; }
    u32 GetMaxParticles() const { return m_MaxParticles; }

    // 回调
    void SetCallback(ParticleCallback cb) { m_Callback = std::move(cb); }

    // GPU Buffers (由 ParticleRenderer 管理更新)
    void SetGPUReady(bool ready) { m_GPUReady = ready; }
    bool IsGPUReady() const { return m_GPUReady; }

    // Tick (CPU 端累积时间, GPU 端在 ParticleRenderer 中执行)
    void Tick(float deltaTime);

    // 获取发射位置 (世界空间)
    float3 GetWorldEmitPosition() const;

    // 状态查询
    float GetElapsedTime() const { return m_Elapsed; }
    u32   GetEmitCount()   const { return m_EmitCount; }
    u32   GetPreSimIndex()  const { return m_PreSimIndex; }
    u32   GetPostSimIndex() const { return m_PostSimIndex; }

private:
    ParticleSystemParam m_Param;
    ParticleState       m_ParticleState     = ParticleState::Stopped;
    ParticleCallback    m_Callback;

    // 时间
    float  m_Elapsed       = 0.0f;
    float  m_LastEmitTime  = 0.0f;
    float  m_TickDeltaTime  = 0.0f;

    // 粒子管理
    u32    m_MaxParticles  = 0;
    u32    m_EmitCount     = 0;
    u32    m_PreSimIndex   = 0;
    u32    m_PostSimIndex  = 1;
    bool   m_NeedInit      = true;
    bool   m_GPUReady      = false;
};

} // namespace he
