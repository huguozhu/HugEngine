// ============================================================
// SplineMeshComponent.cpp — 沿样条生成条带网格
// ============================================================

#include "Scene/SplineMeshComponent.h"
#include "Scene/SplineComponent.h"

#include <algorithm>
#include <cmath>

namespace he {

void SplineMeshComponent::OnCreate() {
    // 条带为单面几何：默认双面渲染，便于从下方观察（道路/管线）
    doubleSided = true;
    m_BuiltVersion = 0;   // 尚未构建
}

void SplineMeshComponent::RebuildFrom(SplineComponent& spline) {
    const int   segCount = spline.GetSegmentCount();
    const float totalLen = spline.GetTotalLength();   // 可能触发样条惰性重建（版本号随之递增）

    // 样条无效：清空网格（避免残留旧几何），并记录当前版本避免每帧重试
    if (segCount <= 0 || totalLen <= 0.0f) {
        TArray<StaticVertex> emptyVerts;
        TArray<u32>          emptyIdx;
        SetMeshData(emptyVerts, emptyIdx);
        m_BuiltVersion = spline.GetVersion();
        return;
    }

    const int   N     = std::max(1, segments);
    const float halfW = width * 0.5f;

    TArray<StaticVertex> verts;
    TArray<u32>          idx;
    verts.reserve((usize)(N + 1) * 2);
    idx.reserve((usize)N * 6);

    for (int i = 0; i <= N; ++i) {
        const float d = totalLen * (float)i / (float)N;   // 沿弧长均匀采样
        const float3 pos = spline.EvaluateAtDistance(d);
        const float3 tan = spline.GetTangent(d);          // 单位切向

        // 侧向：切线 × 世界 up（切线接近竖直时改用 X 轴，避免叉积退化）
        const float3 up = (std::abs(tan.y) < 0.99f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
        const float3 right = glm::normalize(glm::cross(tan, up));
        // 顶面法线：right × tangent（与 up 同向）
        const float3 nrm = glm::normalize(glm::cross(right, tan));

        const float v = d * uvTiling;   // V 随弧长推进

        StaticVertex left { pos - right * halfW, nrm, float2(0.0f, v) };
        StaticVertex rightV{ pos + right * halfW, nrm, float2(1.0f, v) };
        verts.push_back(left);
        verts.push_back(rightV);
    }

    // 四边形条带 → 每段 2 个三角形（绕序使法线朝上，与引擎 CCW 正面一致）
    for (int i = 0; i < N; ++i) {
        const u32 i0 = (u32)(i * 2);       // 左_i
        const u32 i1 = i0 + 1;             // 右_i
        const u32 i2 = i0 + 2;             // 左_i+1
        const u32 i3 = i0 + 3;             // 右_i+1
        idx.push_back(i0); idx.push_back(i1); idx.push_back(i2);
        idx.push_back(i1); idx.push_back(i3); idx.push_back(i2);
    }

    SetMeshData(verts, idx);
    m_BuiltVersion = spline.GetVersion();
}

} // namespace he
