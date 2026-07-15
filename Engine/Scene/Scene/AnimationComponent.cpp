#include "Scene/AnimationComponent.h"
#include "Scene/Transform.h"
#include <algorithm>
#include <cmath>

// ============================================================
// Transform 动画系统实现
// ============================================================

namespace he {

// ============================================================
// 辅助：二分查找包围关键帧对 + 插值
// ============================================================

namespace {

template<typename T>
T InterpolateKeys(const std::vector<AnimKey<T>>& keys, float time, const T& defaultValue) {
    if (keys.empty()) return defaultValue;
    if (keys.size() == 1) return keys[0].value;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time)  return keys.back().value;

    // 二分查找
    usize lo = 0, hi = keys.size() - 1;
    while (hi - lo > 1) {
        usize mid = (lo + hi) / 2;
        if (keys[mid].time <= time) lo = mid;
        else hi = mid;
    }

    float range = keys[hi].time - keys[lo].time;
    float t = (range > 0.0001f) ? (time - keys[lo].time) / range : 0.0f;
    return glm::mix(keys[lo].value, keys[hi].value, t);
}

// quat 特化：slerp
template<>
quat InterpolateKeys<quat>(const std::vector<AnimKey<quat>>& keys, float time, const quat& defaultValue) {
    if (keys.empty()) return defaultValue;
    if (keys.size() == 1) return keys[0].value;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time)  return keys.back().value;

    usize lo = 0, hi = keys.size() - 1;
    while (hi - lo > 1) {
        usize mid = (lo + hi) / 2;
        if (keys[mid].time <= time) lo = mid;
        else hi = mid;
    }

    float range = keys[hi].time - keys[lo].time;
    float t = (range > 0.0001f) ? (time - keys[lo].time) / range : 0.0f;
    return glm::slerp(keys[lo].value, keys[hi].value, t);
}

} // namespace

// ============================================================
// TransformClip::Evaluate
// ============================================================

void TransformClip::Evaluate(float t, float3& outTrans, quat& outRot, float3& outScale) const {
    outTrans = InterpolateKeys(translations, t, float3(0.0f));
    outRot   = InterpolateKeys(rotations,    t, glm::identity<quat>());
    outScale = InterpolateKeys(scales,       t, float3(1.0f));
}

// ============================================================
// AnimationComponent
// ============================================================

void AnimationComponent::Update(float deltaTime, TransformComponent* target) {
    if (!playing || !target || currentClip < 0 || currentClip >= static_cast<i32>(clips.size()))
        return;

    auto& clip = clips[currentClip];
    if (clip.duration <= 0.0f) return;

    time += deltaTime * speed;

    // 循环
    if (clip.looping) {
        while (time > clip.duration) time -= clip.duration;
        while (time < 0.0f) time += clip.duration;
    } else {
        time = std::clamp(time, 0.0f, clip.duration);
    }

    // 求值并写入 Transform
    clip.Evaluate(time, target->position, target->rotation, target->scale);
}

void AnimationComponent::AddTranslationKey(float t, const float3& v) {
    if (currentClip < 0 || currentClip >= static_cast<i32>(clips.size())) return;
    clips[currentClip].translations.push_back({t, v});
}

void AnimationComponent::AddRotationKey(float t, const quat& v) {
    if (currentClip < 0 || currentClip >= static_cast<i32>(clips.size())) return;
    clips[currentClip].rotations.push_back({t, v});
}

void AnimationComponent::AddScaleKey(float t, const float3& v) {
    if (currentClip < 0 || currentClip >= static_cast<i32>(clips.size())) return;
    clips[currentClip].scales.push_back({t, v});
}

void AnimationComponent::FinalizeClip() {
    if (currentClip < 0 || currentClip >= static_cast<i32>(clips.size())) return;
    auto& clip = clips[currentClip];
    // 排序
    auto sortKeys = [](auto& keys) {
        std::sort(keys.begin(), keys.end(),
            [](const auto& a, const auto& b) { return a.time < b.time; });
    };
    sortKeys(clip.translations);
    sortKeys(clip.rotations);
    sortKeys(clip.scales);
    // 计算 duration
    clip.duration = 0.0f;
    if (!clip.translations.empty())
        clip.duration = std::max(clip.duration, clip.translations.back().time);
    if (!clip.rotations.empty())
        clip.duration = std::max(clip.duration, clip.rotations.back().time);
    if (!clip.scales.empty())
        clip.duration = std::max(clip.duration, clip.scales.back().time);
}

} // namespace he
