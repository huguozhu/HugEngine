#include "Asset/glTFLoader.h"
#include "Scene/World.h"
#include "Scene/MeshComponent.h"
#include "Scene/Transform.h"
#include "Core/Log.h"
#include "Math/Geometry.h"

#include <fstream>
#include <cstring>

namespace he::asset {

// ============================================================
// GLB 格式常量
// ============================================================
static constexpr u32 GLB_MAGIC   = 0x46546C67;  // "glTF"
static constexpr u32 JSON_CHUNK  = 0x4E4F534A;  // "JSON"
static constexpr u32 BIN_CHUNK   = 0x004E4942;  // "BIN\0"

/// 从字符串中提取整数值
static i32 ExtractInt(const char* json, const char* key) {
    const char* pos = strstr(json, key);
    if (!pos) return -1;
    pos += strlen(key);
    while (*pos == ' ' || *pos == ':' || *pos == '"') pos++;
    return strtol(pos, nullptr, 10);
}

/// 从字符串中提取浮点数
static f32 ExtractFloat(const char* json, const char* key) {
    const char* pos = strstr(json, key);
    if (!pos) return 0.0f;
    pos += strlen(key);
    while (*pos == ' ' || *pos == ':' || *pos == '"') pos++;
    return strtof(pos, nullptr);
}

/// 从字符串中提取浮点数组（最多 count 个）
static u32 ExtractFloatArray(const char* json, const char* key, f32* out, u32 maxCount) {
    const char* pos = strstr(json, key);
    if (!pos) return 0;
    pos += strlen(key);
    while (*pos == ' ' || *pos == ':' || *pos == '"' || *pos == '[') pos++;
    u32 count = 0;
    while (count < maxCount && *pos && *pos != ']') {
        out[count++] = strtof(pos, const_cast<char**>(&pos));
        while (*pos == ',' || *pos == ' ') pos++;
    }
    return count;
}

glTFResult LoadGLB(World& world, const String& filePath) {
    glTFResult result;

    // 1. 读取文件
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        result.error = "Failed to open file: " + filePath;
        HE_CORE_ERROR("{}", result.error);
        return result;
    }

    usize fileSize = static_cast<usize>(file.tellg());
    file.seekg(0);

    std::vector<u8> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    HE_CORE_INFO("Loading GLB: {} ({} bytes)", filePath, fileSize);

    // 2. 解析 GLB 头部
    if (fileSize < 12) {
        result.error = "File too small for GLB header";
        return result;
    }

    u32 magic   = *reinterpret_cast<u32*>(&data[0]);
    u32 version = *reinterpret_cast<u32*>(&data[4]);
    // u32 totalLen = *reinterpret_cast<u32*>(&data[8]);

    if (magic != GLB_MAGIC) {
        result.error = "Not a GLB file";
        return result;
    }
    if (version != 2) {
        result.error = "Only glTF 2.0 supported";
        return result;
    }

    // 3. 解析 Chunks
    usize offset = 12;
    const char* jsonData = nullptr;
    usize jsonLen = 0;
    const u8* binData = nullptr;
    usize binLen = 0;

    while (offset + 8 <= fileSize) {
        u32 chunkLen  = *reinterpret_cast<u32*>(&data[offset]);
        u32 chunkType = *reinterpret_cast<u32*>(&data[offset + 4]);
        offset += 8;

        if (chunkType == JSON_CHUNK && offset + chunkLen <= fileSize) {
            jsonData = reinterpret_cast<const char*>(&data[offset]);
            jsonLen  = chunkLen;
        } else if (chunkType == BIN_CHUNK && offset + chunkLen <= fileSize) {
            binData = &data[offset];
            binLen  = chunkLen;
        }
        offset += chunkLen;
    }

    if (!jsonData || !binData) {
        result.error = "Missing JSON or BIN chunk";
        return result;
    }

    // 4. 解析 JSON — 找到第一个 mesh 的 primitive
    // 查找 "meshes" 数组中的第一个 mesh
    const char* meshPos = strstr(jsonData, "\"meshes\"");
    if (!meshPos) {
        result.error = "No meshes found";
        return result;
    }

    const char* primPos = strstr(meshPos, "\"primitives\"");
    if (!primPos) {
        result.error = "No primitives found";
        return result;
    }

    // 获取第一个 primitive 的 POSITION accessor
    i32 posAccessor = ExtractInt(primPos, "\"POSITION\"");
    if (posAccessor < 0) {
        result.error = "No POSITION accessor";
        return result;
    }

    // 获取 NORMAL accessor (可选)
    i32 normalAccessor = ExtractInt(primPos, "\"NORMAL\"");

    // 获取 TEXCOORD_0 accessor (可选)
    i32 uvAccessor = ExtractInt(primPos, "\"TEXCOORD_0\"");

    // 获取 INDEX accessor
    i32 idxAccessor = ExtractInt(primPos, "\"indices\"");

    // 获取 baseColorFactor (可选)
    f32 baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const char* matPos = strstr(jsonData, "\"materials\"");

    // 5. 解析 accessors 获取 bufferView
    HE_CORE_INFO("  POSITION accessor: {}, INDEX: {}", posAccessor, idxAccessor);

    // 查找 accessor 指向的 bufferView
    const char* accessorsPos = strstr(jsonData, "\"accessors\"");
    if (!accessorsPos) {
        result.error = "No accessors found";
        return result;
    }

    // 辅助函数：在 accessors JSON 数组中定位第 N 个 accessor（按 {} 计数）
    auto findAccessor = [&](i32 accIdx, i32& outBV, i32& outCount, i32& outOffset, i32& outCompType) -> bool {
        if (accIdx < 0) return false;
        // 在 accessors 数组中定位第 accIdx 个 JSON 对象
        const char* cur = accessorsPos;
        i32 depth = 0, found = -1;
        bool inString = false;
        while (*cur) {
            if (*cur == '"' && (cur == accessorsPos || *(cur-1) != '\\')) inString = !inString;
            if (!inString) {
                if (*cur == '{') {
                    if (depth == 0) found++;
                    depth++;
                } else if (*cur == '}') {
                    depth--;
                }
            }
            if (found == accIdx && depth == 1) break; // 找到了目标 accessor 的开头
            cur++;
        }
        if (found != accIdx) return false;

        outBV       = ExtractInt(cur, "\"bufferView\"");
        outCount    = ExtractInt(cur, "\"count\"");
        outOffset   = ExtractInt(cur, "\"byteOffset\"");
        outCompType = ExtractInt(cur, "\"componentType\"");
        return outBV >= 0;
    };

    // 辅助函数：根据 bufferView index 获取 buffer 中的 byteOffset + byteLength
    auto findBufferView = [&](i32 bvIdx, i32& outByteOffset, i32& outByteLen) -> bool {
        char key[64];
        snprintf(key, sizeof(key), "\"bufferView\":%d", bvIdx);
        const char* bv = strstr(jsonData, key);
        if (!bv) return false;

        outByteOffset = ExtractInt(bv, "\"byteOffset\"");
        outByteLen    = ExtractInt(bv, "\"byteLength\"");
        return true;
    };

    // 6. 提取顶点位置
    i32 posBV, posCount, posOffset, posCompType;
    if (!findAccessor(posAccessor, posBV, posCount, posOffset, posCompType)) {
        result.error = "Invalid POSITION accessor";
        return result;
    }

    i32 posByteOffset, posByteLen;
    if (!findBufferView(posBV, posByteOffset, posByteLen)) {
        result.error = "Invalid POSITION bufferView";
        return result;
    }

    usize posStart = posByteOffset + posOffset;
    if (posStart + posCount * 12 > binLen) {  // 12 = 3 floats * 4 bytes
        result.error = "POSITION data out of range";
        return result;
    }

    const f32* posData = reinterpret_cast<const f32*>(&binData[posStart]);

    // 7. 提取法线（可选）
    const f32* normalData = nullptr;
    i32 normalCount = 0;
    if (normalAccessor >= 0) {
        i32 nBV, nCount, nOffset, nCompType;
        i32 nByteOffset, nByteLen;
        if (findAccessor(normalAccessor, nBV, nCount, nOffset, nCompType) &&
            findBufferView(nBV, nByteOffset, nByteLen)) {
            usize nStart = nByteOffset + nOffset;
            if (nStart + nCount * 12 <= binLen) {
                normalData = reinterpret_cast<const f32*>(&binData[nStart]);
                normalCount = nCount;
            }
        }
    }

    // 8. 提取 UV（可选）
    const f32* uvData = nullptr;
    i32 uvCount = 0;
    if (uvAccessor >= 0) {
        i32 uBV, uCount, uOffset, uCompType;
        i32 uByteOffset, uByteLen;
        if (findAccessor(uvAccessor, uBV, uCount, uOffset, uCompType) &&
            findBufferView(uBV, uByteOffset, uByteLen)) {
            usize uStart = uByteOffset + uOffset;
            if (uStart + uCount * 8 <= binLen) {  // 8 = 2 floats * 4 bytes
                uvData = reinterpret_cast<const f32*>(&binData[uStart]);
                uvCount = uCount;
            }
        }
    }

    // 9. 提取索引
    const u8* indexRaw = nullptr;
    i32 idxCount = 0;
    bool idx32 = false;
    if (idxAccessor >= 0) {
        i32 iBV, iCount, iOffset, iCompType;
        i32 iByteOffset, iByteLen;
        if (findAccessor(idxAccessor, iBV, iCount, iOffset, iCompType) &&
            findBufferView(iBV, iByteOffset, iByteLen)) {
            usize iStart = iByteOffset + iOffset;
            if (iStart + iCount * 2 <= binLen) {
                indexRaw = &binData[iStart];
                idxCount = iCount;
                idx32 = (iCompType == 5125);  // 5125 = UNSIGNED_INT
            }
        }
    }

    HE_CORE_INFO("  Vertices: {}, Normals: {}, UVs: {}, Indices: {}",
                 posCount, normalCount, uvCount, idxCount);

    // 10. 构建网格数据
    TArray<StaticVertex> vertices;
    vertices.reserve(posCount);
    for (i32 i = 0; i < posCount; ++i) {
        StaticVertex v;
        v.position = float3(posData[i * 3], posData[i * 3 + 1], posData[i * 3 + 2]);
        v.normal   = (normalData && i < normalCount)
            ? float3(normalData[i * 3], normalData[i * 3 + 1], normalData[i * 3 + 2])
            : float3(0, 1, 0);
        v.uv       = (uvData && i < uvCount)
            ? float2(uvData[i * 2], uvData[i * 2 + 1])
            : float2(0, 0);
        vertices.push_back(v);
    }

    TArray<u32> indices;
    if (indexRaw && idxCount > 0) {
        indices.reserve(idxCount);
        for (i32 i = 0; i < idxCount; ++i) {
            if (idx32) {
                indices.push_back(reinterpret_cast<const u32*>(indexRaw)[i]);
            } else {
                indices.push_back(reinterpret_cast<const u16*>(indexRaw)[i]);
            }
        }
    }

    // 11. 创建实体
    Entity entity = world.CreateEntity("glTF_Mesh");
    world.AddComponent<TransformComponent>(entity);

    auto* mesh = world.AddComponent<MeshComponent>(entity);
    if (mesh) {
        mesh->SetMeshData(vertices, indices);
    }

    result.entities.push_back(entity);
    result.meshCount = 1;
    result.success   = true;

    HE_CORE_INFO("glTF loaded: {} vertices, {} indices", posCount, idxCount);
    return result;
}

} // namespace he::asset
