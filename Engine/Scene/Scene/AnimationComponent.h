#pragma once

#include "Scene/Component.h"
#include "Math/Math.h"
#include <vector>

namespace he { class TransformComponent; }  // 前向声明

// ============================================================
// AnimationComponent — Transform 动画组件
//
// 存储位置/旋转/缩放关键帧，每帧 Update() 驱动目标
// TransformComponent 的 TRS 值。支持循环播放和速度控制。
//
// 用法：
//   auto* anim = world.AddComponent<AnimationComponent>(entity);
//   anim->AddTranslationKey(0.0f, float3(0,0,0));
//   anim->AddTranslationKey(1.0f, float3(5,0,0));
//   anim->playing = true;
//   每帧: anim->Update(deltaTime, transformComponent);
// ============================================================

namespace he {

// ============================================================
// 单属性关键帧
// ============================================================
template<typename T>
struct AnimKey {
    float time;   // 时间戳（秒）
    T     value;  // 属性值
};

using TranslationKey = AnimKey<float3>;
using RotationKey    = AnimKey<quat>;
using ScaleKey       = AnimKey<float3>;

// ============================================================
// 动画片段（一组关键帧轨道）
// ============================================================
struct TransformClip {
    String name;
    float  duration = 0.0f;  // 总时长（秒）
    bool   looping  = true;  // 循环播放

    std::vector<TranslationKey> translations;  // 按时间升序
    std::vector<RotationKey>    rotations;
    std::vector<ScaleKey>       scales;

    /// 在指定时间求值，返回插值后的 TRS
    void Evaluate(float time, float3& outTranslation, quat& outRotation, float3& outScale) const;
};

// ============================================================
// Transform 动画组件
// ============================================================
class AnimationComponent : public Component {
    HE_COMPONENT()
public:
    // --- 动画数据 ---
    std::vector<TransformClip> clips;
    i32   currentClip = -1;      // 当前播放的动画索引
    float time        = 0.0f;    // 当前播放时间（秒）
    float speed       = 1.0f;    // 播放速度倍率
    bool  playing     = false;   // 是否正在播放

    // --- 方法 ---

    /// 推进动画时间，并将结果写入目标 TransformComponent
    void Update(float deltaTime, TransformComponent* target);

    /// 添加平移动关键帧（自动排序）
    void AddTranslationKey(float t, const float3& v);
    /// 添加旋转关键帧
    void AddRotationKey(float t, const quat& v);
    /// 添加缩放关键帧
    void AddScaleKey(float t, const float3& v);
    /// 完成关键帧添加后调用，更新 duration
    void FinalizeClip();
};

} // namespace he
