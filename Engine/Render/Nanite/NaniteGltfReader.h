#pragma once

// ============================================================
//  Engine/Render/Nanite/NaniteGltfReader.h
//
//  T3（《01.Nanite离线预处理与全Mesh管线方案》）：**不依赖 ECS** 的最小 glTF 几何读取器。
//
//  【为什么不用 Engine/Asset 的 glTFLoader】
//  它依赖 Scene/ECS（构造 MeshComponent、材质系统），而离线工具 `Tools/MeshPreprocess`
//  要的是"扁平数组 + 材质描述"这种最朴素的东西，链接 ECS 既重又没必要。
//  本读取器只依赖 cgltf（单头文件库，实现体在 `Engine/External/cgltf/cgltf_impl.cpp`），
//  并且与单测共用 —— 原先两个测试各自复制了一份读取代码，现在统一到这里。
//
//  【输出坐标空间】顶点原样保留 glTF 的**网格局部空间**，不做任何世界变换、不做节点层级
//  展开：资产是"按 primitive"的，实例变换留给运行时（方案 §2 决定 5 的坐标空间契约）。
//
//  【不做的三件事】① 不解节点树（工具按 primitive 出资产，不看场景摆放）；
//  ② 不加载纹理像素（材质只留描述与 image 索引，方案 §3.3）；③ 不做 LOD（切簇是
//  `BuildNaniteClusters` 的职责）。
// ============================================================

#include "Core/Types.h"
#include "Nanite/NaniteUpload.h"   // 合并几何用到的 `NaniteSourceMeshRange` 在这里

#include <string>
#include <vector>

namespace he::render {

/// 一个 glTF primitive 的几何与材质引用（已展开成扁平数组，索引从 0 起）
///
/// 【索引口径】`indices` 是**本 primitive 内**的顶点下标（0..顶点数-1）。glTF 的
/// `POSITION` 访问器可能带 byteOffset/stride，这里统一解成紧密数组，后续构建器不必
/// 关心访问器细节。
struct NaniteGltfPrimitive {
    std::string name;                 ///< "mesh名/primitive序号"（诊断与文件名用）
    u32         meshIndex      = 0;   ///< glTF meshes[] 下标
    u32         primitiveIndex = 0;   ///< 该 mesh 内的 primitive 序号
    u32         materialIndex  = 0xFFFFFFFFu;  ///< glTF materials[] 下标（无材质 = 0xFFFFFFFF）

    std::vector<float> positions;     ///< 3 float / 顶点（必需；缺失则整条 primitive 被跳过）
    std::vector<float> normals;       ///< 3 float / 顶点（缺失时全 0，`hasNormals = false`）
    std::vector<float> uvs;           ///< 2 float / 顶点（缺失时全 0，`hasUvs = false`）
    std::vector<u32>   indices;       ///< 三角形列表（长度 = 三角形数 × 3）

    bool hasNormals = false;
    bool hasUvs     = false;

    [[nodiscard]] u32 VertexCount()   const { return (u32)(positions.size() / 3u); }
    [[nodiscard]] u32 TriangleCount() const { return (u32)(indices.size() / 3u); }
};

/// 材质（v2 扩展段 `MaterialDesc` 的产前形态：多一个名字，便于日志）
struct NaniteGltfMaterial {
    NaniteMaterialDesc desc{};        ///< 可直接写进 v2 的 `MaterialDesc` 段
    std::string        name;
};

/// 整个 glTF 文件的读取结果
struct NaniteGltfScene {
    std::vector<NaniteGltfPrimitive> primitives;
    std::vector<NaniteGltfMaterial>  materials;
    std::string                      sourcePath;   ///< 读入的文件路径（诊断用）
    u64                              sourceHash = 0;  ///< 源几何 + 材质描述的 64 位哈希

    [[nodiscard]] u64 TotalTriangleCount() const {
        u64 total = 0;
        for (const NaniteGltfPrimitive& p : primitives) total += p.TriangleCount();
        return total;
    }
};

/// 合并几何（运行时 `MeshBatcher` 的同构形态）：连续顶点/索引 + 逐网格三角形区间。
///
/// 【用途】离线工具与单测需要"把一份 glTF 当成一个整体网格"来喂 `BuildNaniteAssetFromGeometry`；
/// 运行时则由 `MeshBatcher` 产出同样形状的数据。两边口径一致才能做逐字节对比（T4/T7）。
struct NaniteGltfMergedGeometry {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<u32>   indices;
    std::vector<NaniteSourceMeshRange> meshes;   ///< 运行时结构：`firstTriangle/triangleCount/materialIndex`
};

/// 把读取结果拼成合并几何（索引已加 `baseVertex`；缺法线补 0、缺 UV 补 0）
void MergeNaniteGltfScene(const NaniteGltfScene& scene, NaniteGltfMergedGeometry& out);
/// 从磁盘读取 glTF（支持 .gltf + 外部 .bin，cgltf 自己处理相对路径）。
///
/// @param path    glTF 文件路径（UTF-8）
/// @param out     读取结果（失败时不改写）
/// @param error   失败原因（可空）
/// @return 成功返回 true；文件打不开 / 解析失败 / 一个可用 primitive 都没有 ⇒ false
[[nodiscard]] bool LoadNaniteGltfFile(const char* path, NaniteGltfScene& out,
                                      std::string* error = nullptr);

/// 计算源哈希：几何（位置/法线/UV/索引）与材质描述的**确定性** 64 位哈希。
///
/// 【用途】写进 v2 头部 `sourceHash`，供构建期增量（T6）与运行时校验（T5）判断"源变了没"。
/// 【不含纹理像素】纹理不进 `.nanite`，改贴图不应让几何缓存失效（方案 §3.4）。
[[nodiscard]] u64 ComputeNaniteGltfSourceHash(const NaniteGltfScene& scene);

} // namespace he::render
