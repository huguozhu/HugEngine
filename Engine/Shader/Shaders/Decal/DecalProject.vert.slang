// ============================================================
// DecalProject.vert — GBuffer 投影贴花：绘制贴花**体积盒**
//
// 与旧 MVP（半透明投射片）的区别：贴花不再作为一张卡片参与光照，而是画一个
// 包裹贴花影响范围的盒子；片段着色器按 GBuffer 的世界坐标把盒子裁剪到投影体积内，
// 只对"盒内的真实表面"生效。深度/世界坐标的读取在 frag 里（见 DecalProject.frag.slang）。
//
// 顶点输入 = 单位立方体 [-0.5, 0.5]^3（DecalPass 程序化生成，无 UV：UV 由世界坐标反算）
// ============================================================

#include "common.slang"

struct VSInput {
    [[vk::location(0)]] float3 position;
};

struct VSOutput {
    float4 position : SV_Position;
};

// 每个贴花一次 DrawIndexed，逐贴花参数走 push constant（240 字节，见 DecalPass.h 的同名结构）
[[vk::push_constant]] cbuffer DecalConstants {
    float4x4 viewProj;
    float4   rotRow0;       // 贴花局部 → 世界 的旋转矩阵行（正交，已归一化）
    float4   rotRow1;
    float4   rotRow2;
    float4   invRotRow0;    // 世界 → 贴花局部 的旋转矩阵行（上面矩阵的转置 = 逆）
    float4   invRotRow1;
    float4   invRotRow2;
    float4   decalOrigin;   // 贴花世界原点（xyz）
    float4   halfExtents;   // 半尺寸：xy = 半宽/半高，z = 半厚度（投影深度的一半）
    float4   colorOpacity;  // rgb = 贴花颜色，a = 不透明度
    float4   normalMetal;   // xyz = 世界空间投影法线（单位向量），w = 金属度
    float4   params;        // x = 粗糙度，y = bindless 纹理句柄，z/w = 1/屏幕宽、1/屏幕高
    uint     flags;         // bit0 = 使用贴花纹理
    uint     _pad0;
    uint     _pad1;
    uint     _pad2;
};

VSOutput vertexMain(VSInput input) {
    // 单位盒顶点 → 贴花体积（乘半尺寸）→ 世界空间 → 裁剪空间
    float3 lp = input.position * halfExtents.xyz;
    float3 wp = decalOrigin.xyz +
        float3(dot(rotRow0.xyz, lp), dot(rotRow1.xyz, lp), dot(rotRow2.xyz, lp));

    VSOutput output;
    output.position = mul(viewProj, float4(wp, 1.0));
    return output;
}
