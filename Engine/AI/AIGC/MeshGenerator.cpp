#include "AI/AIGC/MeshGenerator.h"

#include "Core/Log.h"
#include "Math/Math.h"

#include "nlohmann/json.hpp"

#include <cmath>

using nlohmann::json;

namespace he::ai::aigc {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 立方体：6 面 × 4 顶点（与 CubeComponent 同构）
void GenCube(float size, MeshGenResult& out) {
    const float h = size * 0.5f;
    struct Face { float3 n, t, b; };
    const Face faces[6] = {
        {{ 0, 0, 1}, {1,0,0}, {0,1,0}}, {{ 0, 0,-1}, {-1,0,0},{0,1,0}},
        {{ 1, 0, 0}, {0,0,-1},{0,1,0}}, {{-1, 0, 0}, {0,0,1}, {0,1,0}},
        {{ 0, 1, 0}, {1,0,0}, {0,0,-1}},{{ 0,-1, 0}, {1,0,0}, {0,0,1}},
    };
    for (auto& f : faces) {
        u32 base = (u32)out.vertices.size();
        float3 center = f.n * h;
        float3 corners[4] = {
            center + (-f.t - f.b) * h, center + (f.t - f.b) * h,
            center + (f.t + f.b) * h, center + (-f.t + f.b) * h,
        };
        for (u32 i = 0; i < 4; ++i)
            out.vertices.push_back({ corners[i], f.n,
                float2(i % 2 == 1 ? 1.0f : 0.0f, i / 2 == 1 ? 1.0f : 0.0f) });
        u32 idx[6] = { base, base+1, base+2, base, base+2, base+3 };
        for (u32 i = 0; i < 6; ++i) out.indices.push_back(idx[i]);
    }
    out.name = "cube";
}

// UV 球体（与 SphereComponent 同构）
void GenSphere(float radius, u32 segs, u32 rings, MeshGenResult& out) {
    if (segs < 4) segs = 4;
    if (rings < 2) rings = 2;
    for (u32 ring = 0; ring <= rings; ++ring) {
        float phi = kPi * (float)ring / (float)rings;
        float y = cosf(phi) * radius;
        float r = sinf(phi) * radius;
        for (u32 seg = 0; seg <= segs; ++seg) {
            float theta = 2.0f * kPi * (float)seg / (float)segs;
            float3 pos(r * cosf(theta), y, r * sinf(theta));
            out.vertices.push_back({ pos, glm::normalize(pos),
                float2((float)seg / segs, (float)ring / rings) });
        }
    }
    for (u32 ring = 0; ring < rings; ++ring) {
        for (u32 seg = 0; seg < segs; ++seg) {
            u32 a = ring * (segs + 1) + seg, b = a + segs + 1;
            out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(a + 1);
            out.indices.push_back(a + 1); out.indices.push_back(b); out.indices.push_back(b + 1);
        }
    }
    out.name = "sphere";
}

// 三角锥（金字塔）：底面 4 顶点 + 顶点 1
void GenPyramid(float size, MeshGenResult& out) {
    const float h = size * 0.5f;
    // 顶点：底 0..3，尖 4
    float3 v[5] = {
        float3(-h, -h, -h), float3(h, -h, -h), float3(h, -h, h), float3(-h, -h, h),
        float3(0.0f, h, 0.0f),
    };
    // 法线：每面单独计算（底朝下；底面 2 三角 + 侧面 4 三角 = 6 面）
    const int faces[6][3] = {
        {1, 2, 3}, {0, 3, 2},   // 底面两个三角
        {4, 1, 0}, {4, 2, 1}, {4, 3, 2}, {4, 0, 3},  // 侧面（逆时针朝外）
    };
    // 为每个面生成独立顶点（法线正确）
    for (int f = 0; f < 6; ++f) {
        float3 a = v[faces[f][0]], b = v[faces[f][1]], c = v[faces[f][2]];
        float3 n = glm::normalize(glm::cross(b - a, c - a));
        u32 base = (u32)out.vertices.size();
        out.vertices.push_back({ a, n, float2(0, 0) });
        out.vertices.push_back({ b, n, float2(1, 0) });
        out.vertices.push_back({ c, n, float2(0.5f, 1) });
        out.indices.push_back(base); out.indices.push_back(base + 1); out.indices.push_back(base + 2);
    }
    out.name = "pyramid";
}

// 圆环（torus）：主圆 × 管圆
void GenTorus(float majorR, float minorR, u32 majorSegs, u32 minorSegs, MeshGenResult& out) {
    if (majorSegs < 8) majorSegs = 8;
    if (minorSegs < 4) minorSegs = 4;
    for (u32 i = 0; i <= majorSegs; ++i) {
        float u = 2.0f * kPi * (float)i / (float)majorSegs;
        float3 center(cosf(u) * majorR, 0.0f, sinf(u) * majorR);
        float3 dir(cosf(u), 0.0f, sinf(u));   // 主圆径向
        for (u32 j = 0; j <= minorSegs; ++j) {
            float v = 2.0f * kPi * (float)j / (float)minorSegs;
            float3 offset = dir * (cosf(v) * minorR) + float3(0, sinf(v) * minorR, 0);
            float3 pos = center + offset;
            float3 normal = glm::normalize(offset);
            out.vertices.push_back({ pos, normal,
                float2((float)i / majorSegs, (float)j / minorSegs) });
        }
    }
    for (u32 i = 0; i < majorSegs; ++i) {
        for (u32 j = 0; j < minorSegs; ++j) {
            u32 a = i * (minorSegs + 1) + j, b = a + minorSegs + 1;
            out.indices.push_back(a); out.indices.push_back(b); out.indices.push_back(a + 1);
            out.indices.push_back(a + 1); out.indices.push_back(b); out.indices.push_back(b + 1);
        }
    }
    out.name = "torus";
}

} // namespace

bool MeshGenerator::Generate(const String& shape, float size, u32 segments, MeshGenResult& out) {
    if (shape == "cube") {
        GenCube(size, out);
    } else if (shape == "sphere") {
        GenSphere(size, segments, std::max(segments / 2, 8u), out);
    } else if (shape == "pyramid") {
        GenPyramid(size, out);
    } else if (shape == "torus") {
        GenTorus(size, size * 0.4f, segments, 12, out);
    } else {
        HE_CORE_WARN("[MeshGenerator] 未知形状 '{}'，回退 cube", shape);
        GenCube(size, out);
    }
    out.success = !out.vertices.empty() && !out.indices.empty();
    return out.success;
}

bool MeshGenerator::ParseSpec(const String& jsonText, String& shape, float& size, u32& segments) {
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const std::exception&) {
        return false;
    }
    if (j.contains("shape") && j["shape"].is_string())
        shape = j["shape"].get<String>();
    if (j.contains("size") && j["size"].is_number())
        size = j["size"].get<float>();
    if (j.contains("segments") && j["segments"].is_number_integer())
        segments = j["segments"].get<u32>();
    return true;
}

String MeshGenerator::BuildSpecPrompt() {
    return R"(
生成一个程序化网格规格 JSON。格式：
{"shape":"cube|sphere|pyramid|torus","size":1.0,"segments":16}
规则：
- shape: cube=立方体, sphere=球体(半径=size), pyramid=三角锥, torus=圆环(大半径=size)
- size: 正数（尺寸，米）
- segments: 细分参数（sphere 经线数 / torus 主段数，8~64）
- 只输出 JSON，不要输出解释文字。
)";
}

} // namespace he::ai::aigc
