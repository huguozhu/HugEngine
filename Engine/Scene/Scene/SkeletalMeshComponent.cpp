// ============================================================
// SkeletalMeshComponent.cpp — 骨骼网格（蒙皮顶点上传 + 剪辑播放）
// ============================================================

#include "Scene/SkeletalMeshComponent.h"
#include "RHI/RHI.h"
#include "Core/Log.h"

#include <algorithm>

namespace he {

void SkeletalMeshComponent::SetSkeleton(std::shared_ptr<asset::SkeletonAsset> skel) {
    skeleton = std::move(skel);
    jointWorldMatrices.assign(skeleton ? skeleton->joints.size() : 0, float4x4(1.0f));
    currentClip = -1;
    clipTime    = 0.0f;

    // 蒙皮顶点布局与 StaticVertex 不同（附加 JOINTS/WEIGHTS），
    // 直接以原始字节上传 GPU 顶点缓冲（布局见 SkinnedVertex 与 PBR.vert 的 location 3/4）
    if (!skeleton || skeleton->vertices.empty()) {
        m_VertexCount = 0;
        m_IndexCount  = 0;
        return;
    }
    m_VertexCount = static_cast<u32>(skeleton->vertices.size());
    m_IndexCount  = static_cast<u32>(skeleton->indices.size());

    // 包围盒（绑定姿势，纯 CPU）
    m_Bounds = AABB();
    for (auto& v : skeleton->vertices) m_Bounds.Expand(v.position);

    if (auto* device = rhi::GetDevice()) {
        rhi::BufferDesc vbDesc;
        vbDesc.size        = skeleton->vertices.size() * sizeof(asset::SkinnedVertex);
        vbDesc.usage       = rhi::BufferUsage::Vertex | rhi::BufferUsage::AccelerationStruct;
        vbDesc.initialData = skeleton->vertices.data();
        vbDesc.stride      = sizeof(asset::SkinnedVertex);
        m_VertexBuffer = device->CreateBuffer(vbDesc);

        if (!skeleton->indices.empty()) {
            rhi::BufferDesc ibDesc;
            ibDesc.size        = skeleton->indices.size() * sizeof(u32);
            ibDesc.usage       = rhi::BufferUsage::Index | rhi::BufferUsage::AccelerationStruct;
            ibDesc.initialData = skeleton->indices.data();
            ibDesc.stride      = sizeof(u32);
            m_IndexBuffer = device->CreateBuffer(ibDesc);
        }
        HE_CORE_INFO("[SkeletalMesh] 上传蒙皮网格: {} 顶点, {} 索引（{} 关节）",
            m_VertexCount, m_IndexCount, skeleton->joints.size());
    }
}

void SkeletalMeshComponent::PlayClip(i32 clipIndex, bool loop) {
    if (!skeleton || clipIndex < 0 || clipIndex >= static_cast<i32>(skeleton->clips.size())) {
        currentClip = -1;   // 越界/无剪辑 → 绑定姿势
        return;
    }
    currentClip = clipIndex;
    clipTime    = 0.0f;
    looping     = loop;
    playing     = true;
    ClearBlendLayers();     // 单剪辑路径与混合路径互斥：显式播放剪辑即退出混合
}

// ── 剪辑混合（任务 21）──────────────────────────────────────────────────────

void SkeletalMeshComponent::SetBlendLayers(const asset::AnimationBlendLayer* layers, u32 count) {
    const u32 n = std::min(count, kMaxBlendLayers);
    blendLayerCount = (layers ? n : 0u);
    for (u32 i = 0; i < kMaxBlendLayers; ++i)
        blendLayers[i] = (layers && i < blendLayerCount) ? layers[i] : asset::AnimationBlendLayer{};
    bCrossFading      = false;
    crossFadeTime     = 0.0f;
    crossFadeDuration = 0.0f;
    if (blendLayerCount > 0) {
        // 旧字段只作显示（真值在层里；面板与序列化仍读 currentClip/clipTime）
        currentClip = blendLayers[0].clipIndex;
        clipTime    = blendLayers[0].time;
        playing     = true;
    }
}

bool SkeletalMeshComponent::SetBlendLayer(u32 index, i32 clipIndex, float weight, float time,
                                          float speed, bool loop) {
    if (index >= kMaxBlendLayers) return false;                       // 超上限：拒绝
    if (!skeleton || clipIndex < 0 || clipIndex >= static_cast<i32>(skeleton->clips.size()))
        return false;                                                 // 剪辑越界：拒绝且不改状态
    blendLayers[index].clipIndex = clipIndex;
    blendLayers[index].weight    = weight;
    blendLayers[index].time      = time;
    blendLayers[index].speed     = speed;
    blendLayers[index].looping   = loop;
    if (index + 1 > blendLayerCount) blendLayerCount = index + 1;
    bCrossFading = false;                                             // 手工设层即取消淡入
    currentClip  = blendLayers[0].clipIndex;
    clipTime     = blendLayers[0].time;
    playing      = true;
    return true;
}

void SkeletalMeshComponent::ClearBlendLayers() {
    for (auto& L : blendLayers) L = asset::AnimationBlendLayer{};
    blendLayerCount   = 0;
    bCrossFading      = false;
    crossFadeTime     = 0.0f;
    crossFadeDuration = 0.0f;
}

void SkeletalMeshComponent::CrossFadeTo(i32 toClip, float duration, bool loop) {
    if (!skeleton || toClip < 0 || toClip >= static_cast<i32>(skeleton->clips.size())) return;

    // 出层 = 当前播放状态：已有混合层时取第 0 层，否则用单剪辑状态合成一层
    asset::AnimationBlendLayer out{};
    if (blendLayerCount > 0) {
        out = blendLayers[0];
    } else {
        out.clipIndex = currentClip;          // 可能是 -1（绑定姿势）→ 出层权重会降到 0
        out.time      = clipTime;
        out.speed     = playSpeed;
        out.looping   = looping;
    }
    out.weight = 1.0f;

    asset::AnimationBlendLayer in{};
    in.clipIndex = toClip;
    in.weight    = 0.0f;                      // 由淡入推进抬到 1
    in.time      = 0.0f;                      // 入层从剪辑起点开始
    in.speed     = 1.0f;
    in.looping   = loop;

    blendLayers[0]    = out;
    blendLayers[1]    = in;
    for (u32 i = 2; i < kMaxBlendLayers; ++i) blendLayers[i] = asset::AnimationBlendLayer{};
    blendLayerCount   = 2;
    bCrossFading      = true;
    crossFadeTime     = 0.0f;
    crossFadeDuration = std::max(duration, 0.0f);
    if (crossFadeDuration <= 0.0f) {          // 立即切换：权重就位，Update 里一步收尾
        blendLayers[0].weight = 0.0f;
        blendLayers[1].weight = 1.0f;
    }
    playing     = true;
    currentClip = toClip;                     // 显示用
    clipTime    = 0.0f;
}

void SkeletalMeshComponent::GetBlendWeights(float* out, u32 capacity) const {
    if (!out || capacity == 0) return;
    float sum = 0.0f;
    for (u32 i = 0; i < blendLayerCount; ++i) {
        const auto& L = blendLayers[i];
        if (L.clipIndex >= 0 && L.weight > 0.0f) sum += L.weight;
    }
    for (u32 i = 0; i < capacity; ++i) {
        const auto& L = blendLayers[i];
        out[i] = (i < blendLayerCount && L.clipIndex >= 0 && L.weight > 0.0f && sum > 0.0f)
               ? (L.weight / sum) : 0.0f;
    }
}

} // namespace he
