// ============================================================
//  Engine/Render/Nanite/NaniteGltfReader.cpp
//
//  T3：最小 glTF 几何读取器的实现（cgltf；不依赖 ECS）。
//  设计说明见头文件顶部。
// ============================================================

#include "Nanite/NaniteGltfReader.h"

#include "Core/Log.h"

#include <cgltf.h>

#include <cstring>
#include <filesystem>
#include <functional>

namespace he::render {
namespace {

// FNV-1a 64 位：源哈希用它（简单、确定性、无外部依赖）
inline u64 Fnv1a64(u64 hash, const void* data, usize bytes) {
    const u8* p = static_cast<const u8*>(data);
    for (usize i = 0; i < bytes; ++i) {
        hash ^= (u64)p[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

inline u64 Fnv1a64String(u64 hash, const char* text) {
    if (text == nullptr) return hash;
    return Fnv1a64(hash, text, std::strlen(text));
}

/// 解一个 float 访问器到紧密数组（支持 byteOffset/stride；components 由调用方给定）
bool ReadFloatAccessor(const cgltf_accessor* accessor, u32 components, std::vector<float>& out) {
    if (accessor == nullptr || components == 0u) return false;
    const usize count = (usize)accessor->count;
    out.assign(count * components, 0.0f);
    // cgltf_accessor_read_float 会处理 stride / componentType（float / normalized 整型）
    for (usize i = 0; i < count; ++i) {
        if (!cgltf_accessor_read_float(accessor, i, out.data() + i * components, components)) {
            return false;
        }
    }
    return true;
}

/// 解索引访问器到 u32 数组（无 indices 访问器时按"顺序三角形列表"生成）
bool ReadIndices(const cgltf_primitive& primitive, u32 vertexCount, std::vector<u32>& out) {
    out.clear();
    if (primitive.indices != nullptr) {
        const usize count = (usize)primitive.indices->count;
        out.resize(count);
        for (usize i = 0; i < count; ++i) {
            out[i] = (u32)cgltf_accessor_read_index(primitive.indices, i);
        }
        return true;
    }

    // 无索引：按顶点顺序每 3 个成一个三角形
    if ((vertexCount % 3u) != 0u) return false;
    out.resize(vertexCount);
    for (u32 i = 0; i < vertexCount; ++i) out[i] = i;
    return true;
}

/// glTF 材质 → v2 的 `MaterialDesc`（纹理只留 image 索引，不加载像素）
///
/// 【取 image 下标的方式】cgltf 的 `cgltf_texture` 只给 `cgltf_image*`，没有现成的下标字段；
/// 用"指针减去 images[] 首地址"得到下标（同一 `cgltf_data` 内有效）。
NaniteMaterialDesc ConvertMaterial(const cgltf_material* material, const cgltf_image* imagesBase) {
    NaniteMaterialDesc desc;
    if (material == nullptr) return desc;

    auto imageIndexOf = [imagesBase](const cgltf_texture_view& view) -> u32 {
        if (view.texture == nullptr || view.texture->image == nullptr || imagesBase == nullptr) {
            return kNaniteInvalidTextureIndex;
        }
        return (u32)(view.texture->image - imagesBase);
    };

    desc.alphaMode = (u32)material->alpha_mode;   // 0=Opaque 1=Mask 2=Blend（与 glTF 一致）

    if (material->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
        for (u32 c = 0; c < 4u; ++c) desc.baseColorFactor[c] = pbr.base_color_factor[c];
        desc.metallicFactor  = pbr.metallic_factor;
        desc.roughnessFactor = pbr.roughness_factor;

        desc.textureImageIndex[kNaniteMaterialTextureSlotBaseColor] =
            imageIndexOf(pbr.base_color_texture);
        desc.textureImageIndex[kNaniteMaterialTextureSlotMetallicRoughness] =
            imageIndexOf(pbr.metallic_roughness_texture);
    }

    desc.textureImageIndex[kNaniteMaterialTextureSlotNormal]   = imageIndexOf(material->normal_texture);
    desc.textureImageIndex[kNaniteMaterialTextureSlotEmissive] = imageIndexOf(material->emissive_texture);

    // cgltf 的 emissive_factor 永远是数组（无 has_ 标记），直接拷三个分量
    for (u32 c = 0; c < 3u; ++c) desc.emissiveFactor[c] = material->emissive_factor[c];
    return desc;
}

} // namespace

bool LoadNaniteGltfFile(const char* path, NaniteGltfScene& out, std::string* error) {
    auto fail = [&](const char* reason) {
        if (error != nullptr) *error = reason;
        return false;
    };
    if (path == nullptr || *path == '\0') return fail("路径为空");

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success || data == nullptr) {
        return fail("cgltf_parse_file 失败（文件不存在或不是合法 glTF）");
    }
    // 外部 .bin：cgltf 按 glTF 文件所在目录解析相对路径
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        cgltf_free(data);
        return fail("cgltf_load_buffers 失败（外部 .bin 缺失或不可读）");
    }

    NaniteGltfScene scene;
    scene.sourcePath = path;

    // ── 材质（下标与 glTF materials[] 一一对应，primitive 的 materialIndex 直接可用）──
    scene.materials.reserve((usize)data->materials_count);
    for (usize m = 0; m < data->materials_count; ++m) {
        const cgltf_material& material = data->materials[m];
        NaniteGltfMaterial entry;
        entry.desc = ConvertMaterial(&material, data->images);
        entry.desc.materialIndex = (u32)m;
        entry.name = (material.name != nullptr) ? material.name : "";
        scene.materials.push_back(std::move(entry));
    }

    // ── 几何：按**场景节点**遍历（与运行时 `MeshBatcher` 看到的实体集合一致）──
    // 跳过蒙皮网格；同一 mesh 被多个节点引用时按"每次出现"各算一份，与运行时一致。
    // 没有场景/节点时退回"全部 mesh"，方便只有 mesh 没有 scene 的测试资产。
    u32 skippedNonTriangle = 0;
    u32 skippedNoPosition  = 0;

    auto appendPrimitive = [&](const cgltf_mesh& mesh, usize mi, usize pi) {
        const cgltf_primitive& primitive = mesh.primitives[pi];

        // 只接受三角形列表；strip/fan 与点/线留给 T4 的完整化（先明确跳过并计数）
        if (primitive.type != cgltf_primitive_type_triangles) { ++skippedNonTriangle; return; }

        const cgltf_accessor* positionAttr = nullptr;
        const cgltf_accessor* normalAttr   = nullptr;
        const cgltf_accessor* uvAttr       = nullptr;
        for (usize a = 0; a < primitive.attributes_count; ++a) {
            const cgltf_attribute& attribute = primitive.attributes[a];
            if (attribute.type == cgltf_attribute_type_position) positionAttr = attribute.data;
            else if (attribute.type == cgltf_attribute_type_normal) normalAttr = attribute.data;
            else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
                uvAttr = attribute.data;
        }
        if (positionAttr == nullptr) { ++skippedNoPosition; return; }

        NaniteGltfPrimitive geometry;
        geometry.meshIndex = (u32)mi;
        geometry.primitiveIndex = (u32)pi;
        geometry.materialIndex = primitive.material != nullptr
                                     ? (u32)(primitive.material - data->materials)
                                     : 0xFFFFFFFFu;
        geometry.name = (mesh.name != nullptr ? mesh.name : "mesh") + (std::string("/") +
                        std::to_string(pi));

        if (!ReadFloatAccessor(positionAttr, 3u, geometry.positions)) { ++skippedNoPosition; return; }
        const u32 vertexCount = geometry.VertexCount();

        if (normalAttr != nullptr && ReadFloatAccessor(normalAttr, 3u, geometry.normals)) {
            geometry.hasNormals = geometry.normals.size() == (usize)vertexCount * 3u;
        }
        if (!geometry.hasNormals) geometry.normals.assign((usize)vertexCount * 3u, 0.0f);

        if (uvAttr != nullptr && ReadFloatAccessor(uvAttr, 2u, geometry.uvs)) {
            geometry.hasUvs = geometry.uvs.size() == (usize)vertexCount * 2u;
        }
        if (!geometry.hasUvs) geometry.uvs.assign((usize)vertexCount * 2u, 0.0f);

        if (!ReadIndices(primitive, vertexCount, geometry.indices)) { ++skippedNonTriangle; return; }
        if (geometry.indices.empty()) return;

        scene.primitives.push_back(std::move(geometry));
    };

    const cgltf_scene* gltfScene = data->scene != nullptr
                                       ? data->scene
                                       : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (gltfScene != nullptr && gltfScene->nodes_count > 0) {
        // 递归走节点树（与旧测试的 ProcessNode 一致：只看 node->mesh，跳过蒙皮）
        std::function<void(const cgltf_node*)> walk = [&](const cgltf_node* node) {
            if (node == nullptr) return;
            if (node->mesh != nullptr && node->skin == nullptr) {
                const cgltf_mesh* mesh = node->mesh;
                for (usize pi = 0; pi < mesh->primitives_count; ++pi) {
                    const usize mi = (usize)(mesh - data->meshes);
                    appendPrimitive(*mesh, mi, pi);
                }
            }
            for (usize c = 0; c < node->children_count; ++c) walk(node->children[c]);
        };
        for (usize n = 0; n < gltfScene->nodes_count; ++n) walk(gltfScene->nodes[n]);
    } else {
        for (usize mi = 0; mi < data->meshes_count; ++mi) {
            const cgltf_mesh& mesh = data->meshes[mi];
            for (usize pi = 0; pi < mesh.primitives_count; ++pi) appendPrimitive(mesh, mi, pi);
        }
    }
    if (skippedNonTriangle > 0u || skippedNoPosition > 0u) {
        HE_CORE_WARN("NaniteGltfReader: 跳过 {} 条非三角形 primitive、{} 条缺 POSITION 的 primitive",
                     skippedNonTriangle, skippedNoPosition);
    }

    if (scene.primitives.empty()) {
        cgltf_free(data);
        return fail("glTF 里没有可用的三角形 primitive");
    }

    scene.sourceHash = ComputeNaniteGltfSourceHash(scene);
    out = std::move(scene);
    cgltf_free(data);
    return true;
}

void MergeNaniteGltfScene(const NaniteGltfScene& scene, NaniteGltfMergedGeometry& out) {
    out.positions.clear();
    out.normals.clear();
    out.uvs.clear();
    out.indices.clear();
    out.meshes.clear();

    for (const NaniteGltfPrimitive& primitive : scene.primitives) {
        const u32 baseVertex = (u32)(out.positions.size() / 3u);
        out.positions.insert(out.positions.end(), primitive.positions.begin(), primitive.positions.end());
        out.normals.insert(out.normals.end(), primitive.normals.begin(), primitive.normals.end());
        out.uvs.insert(out.uvs.end(), primitive.uvs.begin(), primitive.uvs.end());

        NaniteSourceMeshRange range;
        range.firstTriangle = (u32)(out.indices.size() / 3u);
        range.triangleCount = primitive.TriangleCount();
        range.materialIndex  = (u32)out.meshes.size();   // 与运行时同口径：一网格一条材质记录
        for (u32 index : primitive.indices) out.indices.push_back(index + baseVertex);
        if (range.triangleCount > 0u) out.meshes.push_back(range);
    }
}

u64 ComputeNaniteGltfSourceHash(const NaniteGltfScene& scene) {
    u64 hash = 0xCBF29CE484222325ull;   // FNV-1a 偏移基

    for (const NaniteGltfPrimitive& primitive : scene.primitives) {
        hash = Fnv1a64String(hash, primitive.name.c_str());
        hash = Fnv1a64(hash, primitive.positions.data(), primitive.positions.size() * sizeof(float));
        hash = Fnv1a64(hash, primitive.normals.data(), primitive.normals.size() * sizeof(float));
        hash = Fnv1a64(hash, primitive.uvs.data(), primitive.uvs.size() * sizeof(float));
        hash = Fnv1a64(hash, primitive.indices.data(), primitive.indices.size() * sizeof(u32));
    }
    for (const NaniteGltfMaterial& material : scene.materials) {
        hash = Fnv1a64String(hash, material.name.c_str());
        hash = Fnv1a64(hash, &material.desc, sizeof(NaniteMaterialDesc));
    }
    return hash;
}

} // namespace he::render
