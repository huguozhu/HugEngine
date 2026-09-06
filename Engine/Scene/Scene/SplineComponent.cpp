// ============================================================
// SplineComponent.cpp — Hermite 分段样条 + 弧长缓存
// ============================================================

#include "Scene/SplineComponent.h"

#include <algorithm>
#include <cmath>

namespace he {

namespace {
constexpr int kSamplesPerSegment = 16;   // 弧长采样步数（每段）

// Hermite 基函数
inline float H00(float t) { return 2.0f * t * t * t - 3.0f * t * t + 1.0f; }
inline float H10(float t) { return t * t * t - 2.0f * t * t + t; }
inline float H01(float t) { return -2.0f * t * t * t + 3.0f * t * t; }
inline float H11(float t) { return t * t * t - t * t; }

inline float D00(float t) { return 6.0f * t * t - 6.0f * t; }
inline float D10(float t) { return 3.0f * t * t - 4.0f * t + 1.0f; }
inline float D01(float t) { return -6.0f * t * t + 6.0f * t; }
inline float D11(float t) { return 3.0f * t * t - 2.0f * t; }
} // namespace

void SplineComponent::AddPoint(const float3& position, const float3& tangent) {
    points.push_back({ position, tangent });
    m_Dirty = true;
}

void SplineComponent::Clear() {
    points.clear();
    m_CumLen.clear();
    m_TotalLength = 0.0f;
    m_Dirty = true;
}

int SplineComponent::GetSegmentCount() const {
    if (points.size() < 2) return 0;
    return bClosedLoop ? (int)points.size() : (int)points.size() - 1;
}

float SplineComponent::GetTotalLength() {
    if (m_Dirty) Rebuild();
    return m_TotalLength;
}

void SplineComponent::Rebuild() {
    m_Dirty = false;
    m_TotalLength = 0.0f;
    m_CumLen.clear();
    const int n = (int)points.size();
    const int segCount = GetSegmentCount();
    if (segCount <= 0) return;

    // 1. 自动切线（零向量 → Catmull-Rom：(P_{i+1} − P_{i−1}) / 2；端点单侧差）
    for (int i = 0; i < n; ++i) {
        if (glm::dot(points[i].tangent, points[i].tangent) > 1e-12f) continue;   // 用户已给
        int prev = (i == 0) ? (bClosedLoop ? n - 1 : 0) : i - 1;
        int next = (i == n - 1) ? (bClosedLoop ? 0 : n - 1) : i + 1;
        points[i].tangent = (points[next].position - points[prev].position) * 0.5f;
    }

    // 2. 逐段弧长采样（弦长累加近似）
    m_CumLen.assign(segCount + 1, 0.0f);
    for (int seg = 0; seg < segCount; ++seg) {
        const SplinePoint& p0 = points[seg];
        const SplinePoint& p1 = points[(seg + 1) % n];
        float len = 0.0f;
        float3 prev = p0.position;
        for (int k = 1; k <= kSamplesPerSegment; ++k) {
            float t = (float)k / kSamplesPerSegment;
            float3 cur = H00(t) * p0.position + H10(t) * p0.tangent
                       + H01(t) * p1.position + H11(t) * p1.tangent;
            len += glm::length(cur - prev);
            prev = cur;
        }
        m_TotalLength += len;
        m_CumLen[seg + 1] = m_TotalLength;
    }
}

float3 SplineComponent::EvaluateAtDistance(float distance) {
    if (m_Dirty) Rebuild();
    const int n = (int)points.size();
    if (n < 2 || m_TotalLength <= 0.0f) return points.empty() ? float3(0.0f) : points[0].position;

    // 距离回绕/钳制
    float d = distance;
    if (bClosedLoop) {
        d = std::fmod(d, m_TotalLength);
        if (d < 0.0f) d += m_TotalLength;
    } else {
        d = std::clamp(d, 0.0f, m_TotalLength);
    }
    // 线性查找所在段
    int seg = 0;
    for (; seg < GetSegmentCount() - 1; ++seg)
        if (d <= m_CumLen[seg + 1]) break;
    float segLen = m_CumLen[seg + 1] - m_CumLen[seg];
    float t = segLen > 1e-9f ? (d - m_CumLen[seg]) / segLen : 0.0f;

    const SplinePoint& p0 = points[seg];
    const SplinePoint& p1 = points[(seg + 1) % n];
    return H00(t) * p0.position + H10(t) * p0.tangent
         + H01(t) * p1.position + H11(t) * p1.tangent;
}

float3 SplineComponent::EvaluateAtParam(float t) {
    if (m_Dirty) Rebuild();
    const int n = (int)points.size();
    const int segCount = GetSegmentCount();
    if (n < 2 || segCount <= 0) return points.empty() ? float3(0.0f) : points[0].position;

    // 参数 t 回绕（闭环）/钳制（开环）到 [0, 段数]
    float tc = t;
    if (bClosedLoop) {
        tc = std::fmod(tc, (float)segCount);
        if (tc < 0.0f) tc += (float)segCount;
    } else {
        tc = std::clamp(tc, 0.0f, (float)segCount);
    }
    int seg = std::min((int)tc, segCount - 1);
    float local = tc - (float)seg;
    const SplinePoint& p0 = points[seg];
    const SplinePoint& p1 = points[(seg + 1) % n];
    return H00(local) * p0.position + H10(local) * p0.tangent
         + H01(local) * p1.position + H11(local) * p1.tangent;
}

float3 SplineComponent::GetTangent(float distance) {
    if (m_Dirty) Rebuild();
    const int n = (int)points.size();
    const int segCount = GetSegmentCount();
    if (n < 2 || segCount <= 0 || m_TotalLength <= 0.0f) return float3(1.0f, 0.0f, 0.0f);

    float d = distance;
    if (bClosedLoop) {
        d = std::fmod(d, m_TotalLength);
        if (d < 0.0f) d += m_TotalLength;
    } else {
        d = std::clamp(d, 0.0f, m_TotalLength);
    }
    int seg = 0;
    for (; seg < segCount - 1; ++seg)
        if (d <= m_CumLen[seg + 1]) break;
    float segLen = m_CumLen[seg + 1] - m_CumLen[seg];
    float t = segLen > 1e-9f ? (d - m_CumLen[seg]) / segLen : 0.0f;

    const SplinePoint& p0 = points[seg];
    const SplinePoint& p1 = points[(seg + 1) % n];
    float3 deriv = D00(t) * p0.position + D10(t) * p0.tangent
                 + D01(t) * p1.position + D11(t) * p1.tangent;
    float len = glm::length(deriv);
    return len > 1e-9f ? deriv / len : float3(1.0f, 0.0f, 0.0f);
}

} // namespace he
