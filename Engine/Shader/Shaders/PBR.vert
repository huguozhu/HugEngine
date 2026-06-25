// ============================================================
// PBR.vert — PBR 渲染顶点着色器
//
// 输入:  StaticVertex (position + normal + uv)
// 输出:  worldPos, worldNormal, uv 给片元着色器
// Push:  modelMatrix, viewProjMatrix
// ============================================================
#version 450

// --- 顶点输入（匹配 StaticVertex 布局）---
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// --- Push constants（与 CPU 端 PushConstantData 对齐）---
layout(push_constant) uniform PushConstants {
    // [0..64]   模型矩阵（世界变换）
    mat4 modelMatrix;
    // [64..128] 视图投影矩阵（裁剪空间变换）
    mat4 viewProjMatrix;
    // [128..144] 基础色因子（线性空间）
    vec4 baseColorFactor;
    // [144..152] 金属度 + 粗糙度 + 环境光遮蔽 + 透明度截断
    float metallicFactor;
    float roughnessFactor;
    float aoFactor;
    float alphaCutoff;
    // [160..176] 相机世界空间位置
    vec4 cameraPosition;
    // [176..192] 光照方向 + 强度
    vec4 lightDirection;
    // [192..208] 光照颜色
    vec4 lightColor;
} u_Push;

// --- 输出给片元着色器 ---
layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUV;

void main() {
    // 世界空间位置（用于光照计算）
    vec4 worldPos = u_Push.modelMatrix * vec4(inPosition, 1.0);
    outWorldPos = worldPos.xyz;

    // 世界空间法线（假设 uniform scale，使用模型矩阵的左上 3x3）
    outWorldNormal = normalize(mat3(u_Push.modelMatrix) * inNormal);

    // UV 直传
    outUV = inUV;

    // 裁剪空间位置
    gl_Position = u_Push.viewProjMatrix * worldPos;
}
