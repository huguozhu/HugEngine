#include "Asset/glTFLoader.h"
#include "Scene/World.h"
#include "Scene/SceneGraph.h"
#include "Scene/MeshComponent.h"
#include "Scene/Transform.h"
#include "Core/Log.h"
#include "Math/Geometry.h"

#include <unordered_map>
#include <vector>
#include <string>

// GLM 矩阵分解（用于 node.matrix）
#include <glm/gtx/matrix_decompose.hpp>

// ============================================================
// cgltf 实现 — 单头文件库，仅在此翻译单元中展开
// ============================================================
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace he::asset {

// ============================================================
// 匿名命名空间 — 内部辅助函数
// ============================================================
namespace {

/// cgltf 错误码 → 可读字符串
String CgltfResultToString(cgltf_result result) {
    switch (result) {
        case cgltf_result_success:           return "成功";
        case cgltf_result_data_too_short:    return "数据过短";
        case cgltf_result_unknown_format:    return "未知格式";
        case cgltf_result_invalid_json:      return "无效 JSON";
        case cgltf_result_invalid_gltf:      return "无效 glTF";
        case cgltf_result_invalid_options:   return "无效选项";
        case cgltf_result_file_not_found:    return "文件未找到";
        case cgltf_result_io_error:          return "IO 错误";
        case cgltf_result_out_of_memory:     return "内存不足";
        case cgltf_result_legacy_gltf:       return "旧版 glTF 1.0（不支持）";
        default:                             return "未知错误";
    }
}

/// 将 glTF 节点的 TRS 变换写入 TransformComponent
///
/// 优先使用 node.matrix（通过 GLM 分解为 TRS），
/// 否则使用独立的 translation/rotation/scale 字段。
///
/// 注意：cgltf 四元数存储顺序为 {x, y, z, w}（glTF 规范），
///       GLM quat 构造函数为 quat(w, x, y, z)，需转换。
void ApplyNodeTransform(const cgltf_node* node, TransformComponent* tf) {
    if (!tf) return;

    if (node->has_matrix) {
        // node.matrix 是列主序 float[16]，与 glm::mat4 内存布局一致
        float4x4 mat = glm::make_mat4(node->matrix);
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(mat, tf->scale, tf->rotation, tf->position, skew, perspective);
    } else {
        if (node->has_translation) {
            tf->position = float3(node->translation[0],
                                  node->translation[1],
                                  node->translation[2]);
        }
        if (node->has_rotation) {
            // cgltf: {x, y, z, w} → GLM: quat(w, x, y, z)
            tf->rotation = quat(node->rotation[3],
                                node->rotation[0],
                                node->rotation[1],
                                node->rotation[2]);
        }
        if (node->has_scale) {
            tf->scale = float3(node->scale[0],
                               node->scale[1],
                               node->scale[2]);
        }
    }
}

/// 将 cgltf 材质参数复制到 MeshComponent 的 PBR 字段
///
/// 提取 metallic-roughness PBR 参数、alpha 模式、
/// 双面/无光照标志，以及纹理路径（URI 字符串）。
void ApplyMaterial(const cgltf_material* material, MeshComponent* meshComp) {
    if (!material || !meshComp) return;

    // --- PBR Metallic-Roughness ---
    if (material->has_pbr_metallic_roughness) {
        auto& pbr = material->pbr_metallic_roughness;
        meshComp->baseColorFactor = float4(
            pbr.base_color_factor[0],
            pbr.base_color_factor[1],
            pbr.base_color_factor[2],
            pbr.base_color_factor[3]);
        meshComp->metallicFactor  = pbr.metallic_factor;
        meshComp->roughnessFactor = pbr.roughness_factor;

        // 基础色纹理
        if (pbr.base_color_texture.texture &&
            pbr.base_color_texture.texture->image) {
            auto* image = pbr.base_color_texture.texture->image;
            if (image->uri && *image->uri) {
                meshComp->baseColorTexture = String(reinterpret_cast<const char*>(image->uri));
            }
        }

        // 金属度-粗糙度纹理
        if (pbr.metallic_roughness_texture.texture &&
            pbr.metallic_roughness_texture.texture->image) {
            auto* image = pbr.metallic_roughness_texture.texture->image;
            if (image->uri && *image->uri) {
                meshComp->metallicRoughnessTexture = String(reinterpret_cast<const char*>(image->uri));
            }
        }
    }

    // --- 法线贴图 ---
    if (material->normal_texture.texture &&
        material->normal_texture.texture->image) {
        auto* image = material->normal_texture.texture->image;
        if (image->uri && *image->uri) {
            meshComp->normalTexture = String(reinterpret_cast<const char*>(image->uri));
        }
    }

    // --- 遮挡贴图 ---
    if (material->occlusion_texture.texture &&
        material->occlusion_texture.texture->image) {
        auto* image = material->occlusion_texture.texture->image;
        if (image->uri && *image->uri) {
            meshComp->occlusionTexture = String(reinterpret_cast<const char*>(image->uri));
        }
    }

    // --- 自发光 ---
    meshComp->emissiveFactor = float3(
        material->emissive_factor[0],
        material->emissive_factor[1],
        material->emissive_factor[2]);

    if (material->emissive_texture.texture &&
        material->emissive_texture.texture->image) {
        auto* image = material->emissive_texture.texture->image;
        if (image->uri && *image->uri) {
            meshComp->emissiveTexture = String(reinterpret_cast<const char*>(image->uri));
        }
    }

    // --- Alpha 模式 ---
    switch (material->alpha_mode) {
        case cgltf_alpha_mode_opaque: meshComp->alphaMode = 0; break;
        case cgltf_alpha_mode_mask:   meshComp->alphaMode = 1; break;
        case cgltf_alpha_mode_blend:  meshComp->alphaMode = 2; break;
        default:                      meshComp->alphaMode = 0; break;
    }
    meshComp->alphaCutoff = material->alpha_cutoff;

    // --- 双面渲染 ---
    meshComp->doubleSided = material->double_sided ? true : false;

    // --- 无光照（KHR_materials_unlit）---
    meshComp->unlit = material->unlit ? true : false;
}

/// 检查 accessor 属性是否存在且数据可用
bool HasAttribute(const cgltf_attribute* attr) {
    return attr && attr->data && attr->data->count > 0;
}

/// 加载单个 glTF primitive 的数据并创建 MeshComponent
///
/// 使用 cgltf_accessor_unpack_floats / cgltf_accessor_unpack_indices
/// 解析顶点属性和索引数据。这些函数自动处理：
///   - 不同组件类型（FLOAT / BYTE / SHORT / HALF 等）
///   - 归一化（normalized 标志）
///   - 缓冲区步长（byteStride）
///   - 稀疏访问器（sparse）
///
/// @return 加载的顶点数和索引数（用于日志输出）
void LoadPrimitive(
    const cgltf_primitive& prim,
    World&               world,
    Entity               parentEntity,
    SceneGraph&          sceneGraph,
    glTFResult&          result)
{
    // 跳过非三角形图元
    if (prim.type != cgltf_primitive_type_triangles) {
        HE_CORE_WARN("跳过非三角形图元 (type={})", static_cast<int>(prim.type));
        return;
    }

    // --- 1. 定位顶点属性 ---
    const cgltf_attribute* posAttr    = nullptr;
    const cgltf_attribute* normalAttr = nullptr;
    const cgltf_attribute* uvAttr     = nullptr;

    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const auto& attr = prim.attributes[a];
        switch (attr.type) {
            case cgltf_attribute_type_position: posAttr    = &attr; break;
            case cgltf_attribute_type_normal:   normalAttr = &attr; break;
            case cgltf_attribute_type_texcoord: uvAttr     = &attr; break;
            default: break; // TANGENT / COLOR / JOINTS / WEIGHTS 暂不处理
        }
    }

    if (!HasAttribute(posAttr)) {
        HE_CORE_WARN("Primitive 缺少 POSITION 属性，跳过");
        return;
    }

    // --- 2. 创建本 primitive 对应的 Entity ---
    cgltf_size vertexCount = posAttr->data->count;
    String primName = String("Prim_") + std::to_string(result.meshCount);
    Entity primEntity = world.CreateEntity(primName);
    world.AddComponent<TransformComponent>(primEntity); // 默认 TRS（单位变换）
    sceneGraph.SetParent(primEntity, parentEntity);
    result.entities.push_back(primEntity);

    // --- 3. 提取位置数据 ---
    cgltf_size posComps = cgltf_num_components(posAttr->data->type); // 应为 3 (VEC3)
    std::vector<float> posFloats(posComps * vertexCount);
    cgltf_accessor_unpack_floats(posAttr->data, posFloats.data(), posComps * vertexCount);

    // --- 4. 提取法线数据（可选）---
    std::vector<float> normalFloats;
    if (HasAttribute(normalAttr)) {
        cgltf_size normComps = cgltf_num_components(normalAttr->data->type);
        normalFloats.resize(normComps * vertexCount);
        cgltf_accessor_unpack_floats(normalAttr->data, normalFloats.data(),
                                     normComps * vertexCount);
    }

    // --- 5. 提取 UV 数据（可选）---
    std::vector<float> uvFloats;
    bool hasUV = false;
    cgltf_size uvComps = 0;
    if (HasAttribute(uvAttr)) {
        uvComps = cgltf_num_components(uvAttr->data->type);
        uvFloats.resize(uvComps * vertexCount);
        cgltf_size unpacked = cgltf_accessor_unpack_floats(uvAttr->data, uvFloats.data(),
                                                           uvComps * vertexCount);
        hasUV = (unpacked > 0);
    }

    // --- 6. 构建 StaticVertex 数组 ---
    TArray<StaticVertex> vertices;
    vertices.reserve(static_cast<usize>(vertexCount));
    for (cgltf_size i = 0; i < vertexCount; ++i) {
        StaticVertex v{};
        v.position = float3(posFloats[i * posComps + 0],
                            posFloats[i * posComps + 1],
                            posFloats[i * posComps + 2]);
        v.normal   = HasAttribute(normalAttr)
            ? float3(normalFloats[i * 3], normalFloats[i * 3 + 1], normalFloats[i * 3 + 2])
            : float3(0.0f, 1.0f, 0.0f); // 默认朝上
        v.uv       = hasUV
            ? float2(uvFloats[i * uvComps], uvFloats[i * uvComps + 1])
            : float2(0.0f, 0.0f); // 默认 UV 零点
        vertices.push_back(v);
    }

    // --- 7. 提取索引数据（可选）---
    TArray<u32> indices;
    if (prim.indices) {
        // 先查询索引数量
        cgltf_size indexCount = cgltf_accessor_unpack_indices(prim.indices, nullptr, sizeof(u32), 0);
        if (indexCount > 0) {
            indices.resize(static_cast<usize>(indexCount));
            cgltf_accessor_unpack_indices(prim.indices, indices.data(), sizeof(u32), indexCount);
        }
    }

    // --- 8. 创建 MeshComponent 并设置数据 ---
    auto* meshComp = world.AddComponent<MeshComponent>(primEntity);
    if (meshComp) {
        meshComp->SetMeshData(vertices, indices);

        // 提取材质参数
        if (prim.material) {
            ApplyMaterial(prim.material, meshComp);
        }
    }

    // --- 9. 记录日志 ---
    HE_CORE_INFO("  Primitive[{}]: {} vertices, {} indices, hasNormal={}, hasUV={}",
                 result.meshCount, vertexCount, indices.size(),
                 HasAttribute(normalAttr), hasUV);

    ++result.meshCount;
}

/// 递归处理 glTF 场景中的节点
///
/// 遍历节点树，为每个节点创建 Entity + TransformComponent，
/// 如有 mesh 则为每个 primitive 创建子 Entity + MeshComponent，
/// 如有 children 则递归处理。
void ProcessNode(
    const cgltf_node*                                          node,
    Entity                                                     parentEntity,
    World&                                                     world,
    SceneGraph&                                                sceneGraph,
    std::unordered_map<const cgltf_node*, Entity>&             nodeEntityMap,
    glTFResult&                                                result)
{
    if (!node) return;

    // --- 1. 创建当前节点对应的 Entity ---
    String entityName = node->name
        ? String(reinterpret_cast<const char*>(node->name))
        : String("glTF_Node");
    Entity nodeEntity = world.CreateEntity(entityName);

    auto* tf = world.AddComponent<TransformComponent>(nodeEntity);
    ApplyNodeTransform(node, tf);

    // 建立父子关系
    if (parentEntity.IsValid()) {
        sceneGraph.SetParent(nodeEntity, parentEntity);
    }

    result.entities.push_back(nodeEntity);
    nodeEntityMap[node] = nodeEntity; // 注册映射，供未来蒙皮/动画等扩展使用

    // --- 2. 如有 mesh，为每个 primitive 创建子实体 ---
    if (node->mesh) {
        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            LoadPrimitive(node->mesh->primitives[p],
                          world, nodeEntity, sceneGraph, result);
        }
    }

    // --- 3. 递归处理子节点 ---
    for (cgltf_size c = 0; c < node->children_count; ++c) {
        ProcessNode(node->children[c], nodeEntity,
                    world, sceneGraph, nodeEntityMap, result);
    }
}

} // namespace

// ============================================================
// 公开入口 — LoadGLTF
// ============================================================
glTFResult LoadGLTF(World& world, SceneGraph& sceneGraph, const String& filePath) {
    glTFResult result;

    // 1. 解析文件（自动识别 .glb / .gltf）
    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result parseResult = cgltf_parse_file(&options, filePath.c_str(), &data);
    if (parseResult != cgltf_result_success) {
        result.error = String("cgltf 解析失败: ") + CgltfResultToString(parseResult)
                       + " (" + filePath + ")";
        HE_CORE_ERROR("{}", result.error);
        return result;
    }

    // 2. 加载外部缓冲区数据（.bin 文件、base64 内嵌等）
    cgltf_result loadResult = cgltf_load_buffers(&options, data, filePath.c_str());
    if (loadResult != cgltf_result_success) {
        result.error = String("cgltf 加载缓冲区失败: ") + CgltfResultToString(loadResult);
        HE_CORE_ERROR("{}", result.error);
        cgltf_free(data);
        return result;
    }

    // 3. 选择要加载的场景（默认场景或第一个场景）
    cgltf_scene* scene = data->scene;
    if (!scene) {
        if (data->scenes_count > 0) {
            scene = &data->scenes[0];
        } else {
            result.error = "glTF 文件中没有场景";
            HE_CORE_ERROR("{}", result.error);
            cgltf_free(data);
            return result;
        }
    }

    HE_CORE_INFO("加载 glTF: {} (场景: {}, 节点: {}, 网格: {})",
                 filePath,
                 scene->name ? reinterpret_cast<const char*>(scene->name) : "默认",
                 scene->nodes_count,
                 data->meshes_count);

    // 4. 递归遍历场景中的所有根节点
    std::unordered_map<const cgltf_node*, Entity> nodeEntityMap;
    for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
        ProcessNode(scene->nodes[i], Entity{kInvalidEntity},
                    world, sceneGraph, nodeEntityMap, result);
    }

    // 5. 清理
    cgltf_free(data);

    result.success = true;
    HE_CORE_INFO("glTF 加载完成: {} 实体, {} 网格图元", result.entities.size(), result.meshCount);
    return result;
}

} // namespace he::asset
