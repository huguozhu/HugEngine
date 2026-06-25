// ============================================================
// ToneMap.vert — 全屏三角形（无需顶点缓冲）
//
// 使用 gl_VertexIndex 生成覆盖整个屏幕的三角形，
// 省去创建顶点/索引缓冲的麻烦。
// ============================================================
#version 450

layout(location = 0) out vec2 outUV;

void main() {
    // 全屏三角形：3 个顶点覆盖整个 NDC 空间
    // gl_VertexIndex = 0 → (-1, -1) → UV(0, 0)
    // gl_VertexIndex = 1 → ( 3, -1) → UV(2, 0)
    // gl_VertexIndex = 2 → (-1,  3) → UV(0, 2)
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}
