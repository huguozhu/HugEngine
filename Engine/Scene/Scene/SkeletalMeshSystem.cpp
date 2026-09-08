// ============================================================
// SkeletalMeshSystem.cpp — 关节动画采样 + 层级合成 + 蒙皮矩阵
// ============================================================

#include "Scene/SkeletalMeshSystem.h"

#include "Scene/SkeletalMeshComponent.h"
#include "Scene/SkeletonAsset.h"
#include "Scene/World.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace he {

namespace {

// 向量分量通道线性插值采样（times 升序；缺通道返回默认值 def）
float3 SampleVec3Channel(const std::vector<float>& times, const std::vector<float3>& values,
                         float time, const float3& def) {
    if (values.empty() || times.empty()) return def;
    if (time <= times.front()) return values.front();
    if (time >= times.back()) return values.back();
    for (usize i = 0; i + 1 < times.size(); ++i) {
        if (time <= times[i + 1]) {
            float k = (time - times[i]) / std::max(times[i + 1] - times[i], 1e-9f);
            return glm::mix(values[i], values[i + 1], k);
        }
    }
    return values.back();
}

// 四元数通道采样（slerp 插值）
quat SampleQuatChannel(const std::vector<float>& times, const std::vector<quat>& values,
                       float time, const quat& def) {
    if (values.empty() || times.empty()) return def;
    if (time <= times.front()) return values.front();
    if (time >= times.back()) return values.back();
    for (usize i = 0; i + 1 < times.size(); ++i) {
        if (time <= times[i + 1]) {
            float k = (time - times[i]) / std::max(times[i + 1] - times[i], 1e-9f);
            quat q = glm::slerp(values[i], values[i + 1], k);
            return glm::normalize(q);
        }
    }
    return values.back();
}

// 本地 TRS → 矩阵
float4x4 TRSToMatrix(const float3& t, const quat& r, const float3& s) {
    float4x4 m = glm::translate(float4x4(1.0f), t);
    m = m * glm::mat4_cast(r);
    m = m * glm::scale(float4x4(1.0f), s);
    return m;
}

} // namespace

void SkeletalMeshSystem::SampleJointTRS(const asset::SkeletonAsset& skel, i32 clipIndex,
                                        float time, i32 jointIndex,
                                        float3& outT, quat& outR, float3& outS) {
    if (jointIndex < 0 || jointIndex >= (i32)skel.joints.size()) {
        outT = float3(0.0f);
        outR = glm::identity<quat>();
        outS = float3(1.0f);
        return;
    }
    const asset::SkeletonJoint& joint = skel.joints[jointIndex];
    outT = joint.translation;
    outR = joint.rotation;
    outS = joint.scale;

    // 剪辑通道覆盖静态 TRS（缺某分量的通道保持静态值）
    if (clipIndex < 0 || clipIndex >= (i32)skel.clips.size()) return;
    const asset::AnimationClip& clip = skel.clips[clipIndex];
    for (const auto& ch : clip.channels) {
        if (ch.jointIndex != jointIndex) continue;
        if (!ch.translations.empty())
            outT = SampleVec3Channel(ch.times, ch.translations, time, outT);
        if (!ch.rotations.empty())
            outR = SampleQuatChannel(ch.times, ch.rotations, time, outR);
        if (!ch.scales.empty())
            outS = SampleVec3Channel(ch.times, ch.scales, time, outS);
    }
}

void SkeletalMeshSystem::ComputeSkinMatrices(const asset::SkeletonAsset& skel, i32 clipIndex,
                                             float time, std::vector<float4x4>& outSkinMatrices,
                                             std::vector<float4x4>* outWorldMatrices) {
    const usize n = skel.joints.size();
    outSkinMatrices.assign(n, float4x4(1.0f));
    if (n == 0) return;

    // 1. 采样全部关节本地 TRS → 本地矩阵
    std::vector<float4x4> local(n);
    for (usize i = 0; i < n; ++i) {
        float3 t;
        quat r;
        float3 s;
        SampleJointTRS(skel, clipIndex, time, (i32)i, t, r, s);
        local[i] = TRSToMatrix(t, r, s);
    }

    // 2. 层级合成世界矩阵（父在前；循环解析直至全部完成，兼容任意关节顺序）
    std::vector<float4x4> world(n, float4x4(0.0f));   // 全零 = 未解析
    usize resolved = 0;
    while (resolved < n) {
        usize before = resolved;
        for (usize i = 0; i < n; ++i) {
            if (world[i][3].w != 0.0f) continue;   // 已解析（w=1 标记）
            i32 p = skel.joints[i].parent;
            if (p < 0) {
                world[i] = local[i];
                ++resolved;
            } else if (p < (i32)n && world[p][3].w != 0.0f) {
                world[i] = world[p] * local[i];
                ++resolved;
            }
        }
        if (before == resolved) break;   // 坏数据（孤儿关节）：跳出防死循环
    }
    // 未解析的孤儿关节回退为本地矩阵
    for (usize i = 0; i < n; ++i)
        if (world[i][3].w == 0.0f) world[i] = local[i];

    // 3. 蒙皮矩阵 = world × inverseBind（可选同时输出世界矩阵）
    for (usize i = 0; i < n; ++i) {
        outSkinMatrices[i] = world[i] * skel.joints[i].inverseBind;
    }
    if (outWorldMatrices) {
        outWorldMatrices->assign(n, float4x4(1.0f));
        for (usize i = 0; i < n; ++i) (*outWorldMatrices)[i] = world[i];
    }
}

void SkeletalMeshSystem::Update(World& world, f32 dt) {
    if (dt <= 0.0f) return;

    world.ForEach<SkeletalMeshComponent>([&](Entity, SkeletalMeshComponent& sm) {
        if (!sm.skeleton) return;

        // 1. 推进剪辑时间（循环回绕 / 播完停止）
        if (sm.playing && sm.currentClip >= 0 &&
            sm.currentClip < (i32)sm.skeleton->clips.size()) {
            const asset::AnimationClip& clip = sm.skeleton->clips[sm.currentClip];
            sm.clipTime += dt * sm.playSpeed;
            if (clip.duration > 0.0f) {
                if (sm.looping) {
                    sm.clipTime = std::fmod(sm.clipTime, clip.duration);
                } else if (sm.clipTime >= clip.duration) {
                    sm.clipTime = clip.duration;
                    sm.playing = false;   // 播完停止
                }
            }
        }

        // 2. 计算蒙皮矩阵（world × inverseBind）+ 世界矩阵（调试）
        ComputeSkinMatrices(*sm.skeleton, sm.currentClip, sm.clipTime,
                            sm.boneMatrices, &sm.jointWorldMatrices);
        sm.bBonesDirty = true;   // 渲染管线下一帧上传骨骼 SSBO
    });
}

} // namespace he
