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

// 层级合成 + 蒙皮矩阵的**共用**实现：采样器由调用方给（单剪辑 / 多层混合 / 重定向各一份）
//
// 【为什么要抽出来】各条路径的层级合成、孤儿关节兜底、蒙皮公式必须逐字一致，
// 否则"混合/重定向出来的姿势"与"单剪辑的姿势"会在层级/公式上分叉，权重=1 的混合也将不等于单剪辑。
template <typename SampleFn>
void ComposeSkinMatrices(const asset::SkeletonAsset& skel, SampleFn&& sample,
                         std::vector<float4x4>& outSkinMatrices,
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
        sample((i32)i, t, r, s);
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

// 多层混合的**共用**实现：单层采样器由调用方给（普通单剪辑 / 重定向各一份）
//
// 规则（任务 21，两条路径必须一致）：
//   · 权重 ≤0、clipIndex<0（绑定姿势层）、clipIndex 越界的层**不参与**
//   · 平移/缩放线性加权后按 Σw 归一化；旋转对齐半球后加权求和再归一化（加权 nlerp）
//   · 全部层都不参与 ⇒ 保持传进来的基准 TRS（目标关节静态 TRS = 绑定姿势）
template <typename SampleOneFn>
void BlendLayerSamples(const float3& baseT, const quat& baseR, const float3& baseS,
                       SampleOneFn&& sampleOne,
                       const asset::AnimationBlendLayer* layers, u32 layerCount,
                       usize clipCount, float3& outT, quat& outR, float3& outS) {
    outT = baseT;
    outR = baseR;
    outS = baseS;
    if (!layers || layerCount == 0) return;

    float  sumW = 0.0f;
    float3 tSum(0.0f), sSum(0.0f);
    quat   rSum(0.0f, 0.0f, 0.0f, 0.0f);   // 零四元数 = 尚未累加
    bool   hasRotation = false;
    for (u32 i = 0; i < layerCount; ++i) {
        const asset::AnimationBlendLayer& L = layers[i];
        if (L.weight <= 0.0f) continue;                              // 权重非正：不参与
        if (L.clipIndex < 0 || (usize)L.clipIndex >= clipCount)
            continue;                                                // 绑定姿势层/越界：不参与

        float3 t;
        quat   r;
        float3 s;
        sampleOne(L, t, r, s);

        tSum += L.weight * t;
        sSum += L.weight * s;
        // 四元数双覆盖：q 与 -q 表示同一旋转，加权求和前必须对齐到同一个半球，
        // 否则两个"其实相同"的旋转会互相抵消（典型现象是混合到中途姿态突然抽搐/塌陷）。
        if (hasRotation && glm::dot(rSum, r) < 0.0f) r = -r;
        if (!hasRotation) hasRotation = true;
        rSum += L.weight * r;
        sumW += L.weight;
    }
    if (sumW <= 0.0f || !hasRotation) return;   // 全不参与 ⇒ 保持基准 TRS

    outT = tSum / sumW;
    outS = sSum / sumW;
    outR = glm::normalize(rSum / sumW);          // 加权平均后归一化（加权 nlerp）
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

void SkeletalMeshSystem::SampleJointTRSBlended(const asset::SkeletonAsset& skel,
                                               const asset::AnimationBlendLayer* layers,
                                               u32 layerCount, i32 jointIndex,
                                               float3& outT, quat& outR, float3& outS) {
    // 基准 = 关节静态 TRS：全部层不参与时就是绑定姿势（与 clipIndex=-1 的行为一致）
    if (jointIndex < 0 || jointIndex >= (i32)skel.joints.size()) {
        outT = float3(0.0f);
        outR = glm::identity<quat>();
        outS = float3(1.0f);
        return;
    }
    const asset::SkeletonJoint& joint = skel.joints[jointIndex];
    BlendLayerSamples(joint.translation, joint.rotation, joint.scale,
        [&](const asset::AnimationBlendLayer& L, float3& t, quat& r, float3& s) {
            SampleJointTRS(skel, L.clipIndex, L.time, jointIndex, t, r, s);
        },
        layers, layerCount, skel.clips.size(), outT, outR, outS);
}

void SkeletalMeshSystem::ComputeSkinMatrices(const asset::SkeletonAsset& skel, i32 clipIndex,
                                             float time, std::vector<float4x4>& outSkinMatrices,
                                             std::vector<float4x4>* outWorldMatrices) {
    ComposeSkinMatrices(skel,
        [&](i32 jointIndex, float3& t, quat& r, float3& s) {
            SampleJointTRS(skel, clipIndex, time, jointIndex, t, r, s);
        },
        outSkinMatrices, outWorldMatrices);
}

void SkeletalMeshSystem::ComputeSkinMatricesBlended(const asset::SkeletonAsset& skel,
                                                    const asset::AnimationBlendLayer* layers,
                                                    u32 layerCount,
                                                    std::vector<float4x4>& outSkinMatrices,
                                                    std::vector<float4x4>* outWorldMatrices) {
    ComposeSkinMatrices(skel,
        [&](i32 jointIndex, float3& t, quat& r, float3& s) {
            SampleJointTRSBlended(skel, layers, layerCount, jointIndex, t, r, s);
        },
        outSkinMatrices, outWorldMatrices);
}

// ── 动画重定向（任务 22）──────────────────────────────────────────────────

asset::RetargetProfile SkeletalMeshSystem::BuildRetargetProfile(
        const asset::SkeletonAsset& target, const asset::SkeletonAsset& source) {
    asset::RetargetProfile profile;
    profile.name = target.name + " <- " + source.name;
    profile.targetToSource.assign(target.joints.size(), -1);

    // 按关节**名字**匹配：glTF/UE 的骨架关节都有稳定的名字（"Head"/"LeftUpLeg"…），
    // 索引在不同骨架之间没有任何对应关系，靠索引去接动画必然错位。
    for (usize i = 0; i < target.joints.size(); ++i) {
        const String& n = target.joints[i].name;
        if (n.empty()) continue;                       // 无名关节不参与匹配（保持绑定姿势）
        for (usize j = 0; j < source.joints.size(); ++j) {
            if (source.joints[j].name == n) {          // 重名取第一个 → 结果确定
                profile.targetToSource[i] = (i32)j;
                break;
            }
        }
    }
    return profile;
}

void SkeletalMeshSystem::SampleJointTRSRetargeted(const asset::SkeletonAsset& target,
                                                 const asset::SkeletonAsset& source,
                                                 const asset::RetargetProfile& profile,
                                                 i32 clipIndex, float time, i32 targetJointIndex,
                                                 float3& outT, quat& outR, float3& outS) {
    // 未映射 / 越界：目标关节保持自己的绑定姿势（不崩溃、不借用源姿态）
    const bool targetOk = targetJointIndex >= 0 && targetJointIndex < (i32)target.joints.size();
    if (!targetOk) {
        outT = float3(0.0f);
        outR = glm::identity<quat>();
        outS = float3(1.0f);
        return;
    }
    const asset::SkeletonJoint& tj = target.joints[targetJointIndex];
    outT = tj.translation;
    outR = tj.rotation;
    outS = tj.scale;

    const i32 srcIndex = (targetJointIndex < (i32)profile.targetToSource.size())
                       ? profile.targetToSource[targetJointIndex] : -1;
    if (srcIndex < 0 || srcIndex >= (i32)source.joints.size()) return;

    const asset::SkeletonJoint& sj = source.joints[srcIndex];
    // 源动画采样（源绑定姿势 = sj 的静态 TRS，缺失通道时 SampleJointTRS 会退回静态值）
    float3 sT;
    quat   sR;
    float3 sS;
    SampleJointTRS(source, clipIndex, time, srcIndex, sT, sR, sS);

    // ① 旋转：借"源相对自己绑定姿势的偏移"，叠加到目标自己的绑定姿势上
    //    源无旋转通道时 sR == sj.rotation ⇒ 偏移为单位四元数 ⇒ 结果就是目标绑定姿势
    const quat delta = glm::normalize(glm::conjugate(sj.rotation) * sR);
    outR = glm::normalize(tj.rotation * delta);

    // ② 平移：默认不重定向（平移编码的是骨骼长度/体型）。打开时按各关节绑定长度比缩放，
    //    让"长大了的骨架"迈出成比例的步子；源绑定平移退化（≈0）时比例取 1。
    if (profile.retargetTranslation) {
        float k = profile.translationScale;
        if (profile.autoProportion) {
            const float srcLen = glm::length(sj.translation);
            const float dstLen = glm::length(tj.translation);
            if (srcLen > 1e-6f) k *= dstLen / srcLen;
        }
        outT = tj.translation + (sT - sj.translation) * k;
    }

    // ③ 缩放：同样按"源相对自己绑定姿势的倍率"作用到目标绑定缩放上（逐分量，防除零）
    if (profile.retargetScale) {
        const float3 baseScale = sj.scale;
        const float3 ratio = glm::all(glm::greaterThan(glm::abs(baseScale), float3(1e-6f)))
                           ? (sS / baseScale) : float3(1.0f);
        outS = tj.scale * ratio;
    }
}

void SkeletalMeshSystem::ComputeSkinMatricesRetargeted(const asset::SkeletonAsset& target,
                                                       const asset::SkeletonAsset& source,
                                                       const asset::RetargetProfile& profile,
                                                       i32 clipIndex, float time,
                                                       std::vector<float4x4>& outSkinMatrices,
                                                       std::vector<float4x4>* outWorldMatrices) {
    // 注意：层级合成与 inverseBind 用**目标骨架**的（蒙皮矩阵必须落在目标骨架上）
    ComposeSkinMatrices(target,
        [&](i32 jointIndex, float3& t, quat& r, float3& s) {
            SampleJointTRSRetargeted(target, source, profile, clipIndex, time, jointIndex, t, r, s);
        },
        outSkinMatrices, outWorldMatrices);
}

void SkeletalMeshSystem::SampleJointTRSBlendedRetargeted(const asset::SkeletonAsset& target,
                                                        const asset::SkeletonAsset& source,
                                                        const asset::RetargetProfile& profile,
                                                        const asset::AnimationBlendLayer* layers,
                                                        u32 layerCount, i32 targetJointIndex,
                                                        float3& outT, quat& outR, float3& outS) {
    if (targetJointIndex < 0 || targetJointIndex >= (i32)target.joints.size()) {
        outT = float3(0.0f);
        outR = glm::identity<quat>();
        outS = float3(1.0f);
        return;
    }
    // 基准同普通混合路径 = 目标关节静态 TRS；每层先重定向再混合（层语义完全一致）
    const asset::SkeletonJoint& tj = target.joints[targetJointIndex];
    BlendLayerSamples(tj.translation, tj.rotation, tj.scale,
        [&](const asset::AnimationBlendLayer& L, float3& t, quat& r, float3& s) {
            SampleJointTRSRetargeted(target, source, profile, L.clipIndex, L.time,
                                     targetJointIndex, t, r, s);
        },
        layers, layerCount, source.clips.size(), outT, outR, outS);
}

void SkeletalMeshSystem::ComputeSkinMatricesBlendedRetargeted(
        const asset::SkeletonAsset& target, const asset::SkeletonAsset& source,
        const asset::RetargetProfile& profile, const asset::AnimationBlendLayer* layers,
        u32 layerCount, std::vector<float4x4>& outSkinMatrices,
        std::vector<float4x4>* outWorldMatrices) {
    ComposeSkinMatrices(target,
        [&](i32 jointIndex, float3& t, quat& r, float3& s) {
            SampleJointTRSBlendedRetargeted(target, source, profile, layers, layerCount,
                                            jointIndex, t, r, s);
        },
        outSkinMatrices, outWorldMatrices);
}

void SkeletalMeshSystem::RebuildInverseBindMatrices(asset::SkeletonAsset& skel) {
    std::vector<float4x4> skin, world;
    // 用绑定姿势（静态 TRS）做一次层级合成，取世界矩阵的逆即逆绑定矩阵
    ComposeSkinMatrices(skel,
        [&](i32 jointIndex, float3& t, quat& r, float3& s) {
            const asset::SkeletonJoint& j = skel.joints[(usize)jointIndex];
            t = j.translation;
            r = j.rotation;
            s = j.scale;
        },
        skin, &world);
    for (usize i = 0; i < skel.joints.size(); ++i)
        skel.joints[i].inverseBind = glm::inverse(world[i]);
}

void SkeletalMeshSystem::Update(World& world, f32 dt) {
    if (dt <= 0.0f) return;

    world.ForEach<SkeletalMeshComponent>([&](Entity, SkeletalMeshComponent& sm) {
        if (!sm.skeleton) return;

        // 任务 22：动画数据可能来自**另一副骨架**（重定向）；剪辑表/时长一律按动画来源取，
        // 而关节层级/逆绑定矩阵/蒙皮顶点始终用本组件自己的骨架。
        const asset::SkeletonAsset* animSkel = sm.AnimationSource();
        if (!animSkel) return;
        // 重定向：只有两边都齐（源骨架 + 关节映射）才走重定向采样器；空指针不解引用
        const bool retarget = (sm.sourceSkeleton && sm.retargetProfile);

        if (sm.blendLayerCount > 0) {
            // ── 混合路径（任务 21；任务 22 起可与重定向组合）──
            // ① 交叉淡入推进：出层 1→0、入层 0→1（淡完收敛成单层，避免长期付两层的采样）
            if (sm.bCrossFading) {
                if (sm.blendLayerCount < 2) {
                    sm.bCrossFading = false;                 // 中途被清层/被手工设层 → 取消
                } else {
                    sm.crossFadeTime += dt;
                    const float k = (sm.crossFadeDuration > 0.0f)
                                  ? std::min(sm.crossFadeTime / sm.crossFadeDuration, 1.0f)
                                  : 1.0f;
                    sm.blendLayers[0].weight = 1.0f - k;
                    sm.blendLayers[1].weight = k;
                    if (k >= 1.0f) {
                        // 淡入完成：只留入层（权重 1），回到单层混合状态
                        const asset::AnimationBlendLayer in = sm.blendLayers[1];
                        sm.ClearBlendLayers();
                        sm.blendLayers[0] = in;
                        sm.blendLayers[0].weight = 1.0f;
                        sm.blendLayerCount = 1;
                        sm.currentClip = in.clipIndex;
                        sm.clipTime    = in.time;
                    }
                }
            }

            // ② 逐层推进时间（每层有自己的速度/循环；权重为 0 的层**仍然推进** —— 否则淡入
            //    的那一层会永远停在起点）
            if (sm.playing) {
                for (u32 i = 0; i < sm.blendLayerCount; ++i) {
                    asset::AnimationBlendLayer& L = sm.blendLayers[i];
                    if (L.clipIndex < 0 || L.clipIndex >= (i32)animSkel->clips.size()) continue;
                    const asset::AnimationClip& clip = animSkel->clips[L.clipIndex];
                    L.time += dt * L.speed;
                    if (clip.duration > 0.0f) {
                        L.time = L.looping ? std::fmod(L.time, clip.duration)
                                           : std::min(L.time, clip.duration);
                    }
                }
                if (sm.blendLayerCount > 0) sm.clipTime = sm.blendLayers[0].time;   // 显示用
            }

            if (retarget) {
                ComputeSkinMatricesBlendedRetargeted(*sm.skeleton, *sm.sourceSkeleton,
                                                     *sm.retargetProfile,
                                                     sm.blendLayers, sm.blendLayerCount,
                                                     sm.boneMatrices, &sm.jointWorldMatrices);
            } else {
                ComputeSkinMatricesBlended(*sm.skeleton, sm.blendLayers, sm.blendLayerCount,
                                           sm.boneMatrices, &sm.jointWorldMatrices);
            }
            sm.bBonesDirty = true;
            return;
        }

        // ── 单剪辑路径（原有行为；带来源时按重定向采样）──
        // 1. 推进剪辑时间（循环回绕 / 播完停止）
        if (sm.playing && sm.currentClip >= 0 &&
            sm.currentClip < (i32)animSkel->clips.size()) {
            const asset::AnimationClip& clip = animSkel->clips[sm.currentClip];
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
        if (retarget) {
            ComputeSkinMatricesRetargeted(*sm.skeleton, *sm.sourceSkeleton, *sm.retargetProfile,
                                          sm.currentClip, sm.clipTime,
                                          sm.boneMatrices, &sm.jointWorldMatrices);
        } else {
            ComputeSkinMatrices(*sm.skeleton, sm.currentClip, sm.clipTime,
                                sm.boneMatrices, &sm.jointWorldMatrices);
        }
        sm.bBonesDirty = true;   // 渲染管线下一帧上传骨骼 SSBO
    });
}

} // namespace he
