// ============================================================
//  SkyboxLoader.cpp —— 02/03/04 共用的天空盒加载实现
//
//  这里也是整个 libentry.so 里**唯一**定义 stb_image 实现的 TU：
//  02/03/04 的 App 只 include <stb_image.h> 使用 stbi_load / stbi_loadf。
// ============================================================

#include "SkyboxLoader.h"

#include "Core/Core.h"
#include "Core/Log.h"
#include "Math/Math.h"
#include "Platform/IVfs.h"
#include "RHI/RHI.h"
#include "Scene/SceneGraph.h"
#include "Scene/SkyboxComponent.h"
#include "Scene/Transform.h"
#include "Scene/World.h"

// 整个 .so 只在这里产出 stbi_* 的定义（其它 TU 只用声明）
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <vector>

namespace he::samples {
namespace {

/// 把 6 面 RGBA8 数据 + 采样器挂成一个 SkyboxComponent 实体（两种布局的公共尾巴）
void AttachSkybox(World& world, SceneGraph& sceneGraph, rhi::IRHIDevice* device,
                  u32 faceSize, std::vector<u8>& faces) {
    rhi::TextureDesc td;
    td.format      = rhi::Format::RGBA8_UNORM;
    td.width       = faceSize;
    td.height      = faceSize;
    td.mipLevels   = 1;
    td.arrayLayers = 6;
    td.usage       = rhi::TextureUsage::ShaderResource
                   | rhi::TextureUsage::Cubemap
                   | rhi::TextureUsage::TransferDst;
    td.initialData = faces.data();
    auto cubemap = device->CreateTexture(td);

    rhi::SamplerDesc sd;
    sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
    sd.addressU = sd.addressV = sd.addressW = rhi::AddressMode::ClampToEdge;
    auto sampler = device->CreateSampler(sd);

    Entity e = world.CreateEntity("Skybox");
    world.AddComponent<TransformComponent>(e);
    auto* sc = world.AddComponent<SkyboxComponent>(e);
    sc->SetCubemap(std::move(cubemap), std::move(sampler));
    sceneGraph.SetParent(e, Entity{ kInvalidEntity });
}

} // namespace

// ============================================================
//  02.Cube 的布局：6 张 PNG 面
// ============================================================
bool CreateSkyboxFromPngFaces(World& world, SceneGraph& sceneGraph, rhi::IRHIDevice* device) {
    const String skyDir = vfs::Resolve("Textures/skybox/");
    const char* faces[6] = {
        "daylight0.png", "daylight1.png", "daylight2.png",
        "daylight3.png", "daylight4.png", "daylight5.png",
    };

    std::vector<u8> all;
    u32 faceW = 0, faceH = 0;
    for (u32 f = 0; f < 6; ++f) {
        const String path = skyDir + faces[f];
        int w = 0, h = 0, ch = 0;
        u8* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!pixels) {
            HE_CORE_WARN("[Skybox] 第 {} 面加载失败: {}", f, path);
            return false;
        }
        if (f == 0) {
            faceW = static_cast<u32>(w);
            faceH = static_cast<u32>(h);
        }
        all.insert(all.end(), pixels, pixels + static_cast<usize>(faceW) * faceH * 4);
        stbi_image_free(pixels);
    }
    if (all.empty() || faceW == 0 || faceH == 0) {
        HE_CORE_WARN("[Skybox] 6 个面都为空，跳过天空盒");
        return false;
    }
    // 6 个面尺寸必须一致，否则 arrayLayers=6 的纹理会被驱动按最小面裁掉
    if (faceW != faceH) {
        HE_CORE_WARN("[Skybox] 面不是正方形（{}x{}），按最大值裁切", faceW, faceH);
    }

    AttachSkybox(world, sceneGraph, device, faceW > faceH ? faceW : faceH, all);
    return true;
}

// ============================================================
//  03/04 的布局：等距柱状 HDR → 6 面
// ============================================================
bool CreateSkyboxFromHdr(World& world, SceneGraph& sceneGraph, rhi::IRHIDevice* device) {
    const String hdrPath = vfs::Resolve("Textures/skybox.hdr");
    int hdrW = 0, hdrH = 0, hdrCh = 0;
    float* hdrData = stbi_loadf(hdrPath.c_str(), &hdrW, &hdrH, &hdrCh, 4);
    if (!hdrData) {
        HE_CORE_WARN("[Skybox] skybox.hdr 加载失败: {}", hdrPath);
        return false;
    }
    HE_CORE_INFO("[Skybox] skybox.hdr {}x{}（{} 通道）", hdrW, hdrH, hdrCh);

    const u32 faceSize  = 512;
    const u32 faceBytes = faceSize * faceSize * 4;
    std::vector<u8> allFaces(faceBytes * 6);

    // 6 个面的朝向：+X/-X/+Y/-Y/+Z/-Z，与桌面 03 的算法逐行一致
    struct Face { float3 dir, up, right; };
    const Face faces[6] = {
        {{ 1, 0, 0}, {0,-1, 0}, {0, 0,-1}},
        {{-1, 0, 0}, {0,-1, 0}, {0, 0, 1}},
        {{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{ 0,-1, 0}, {0, 0,-1}, {1, 0, 0}},
        {{ 0, 0, 1}, {0,-1, 0}, {1, 0, 0}},
        {{ 0, 0,-1}, {0,-1, 0}, {-1,0, 0}},
    };

    for (u32 f = 0; f < 6; ++f) {
        u8* faceData = allFaces.data() + f * faceBytes;
        for (u32 y = 0; y < faceSize; ++y) {
            for (u32 x = 0; x < faceSize; ++x) {
                const float u = (2.0f * x / faceSize) - 1.0f;
                const float v = (2.0f * y / faceSize) - 1.0f;
                const float3 dir = glm::normalize(faces[f].dir + faces[f].right * u + faces[f].up * v);
                const float eqU = (std::atan2(dir.z, dir.x) / (2.0f * 3.14159265f)) + 0.5f;
                const float eqV = (std::asin(glm::clamp(dir.y, -1.0f, 1.0f)) / 3.14159265f) + 0.5f;
                int px = static_cast<int>(eqU * hdrW) % hdrW;
                int py = static_cast<int>(eqV * hdrH) % hdrH;
                if (px < 0) px += hdrW;
                if (py < 0) py += hdrH;
                const float* src = hdrData + (py * hdrW + px) * 4;
                auto tonemap = [](float c) {
                    c = c / (1.0f + c);   // Reinhard
                    return static_cast<u8>(glm::clamp(c, 0.0f, 1.0f) * 255.0f);
                };
                const usize idx = (y * faceSize + x) * 4;
                faceData[idx + 0] = tonemap(src[0]);
                faceData[idx + 1] = tonemap(src[1]);
                faceData[idx + 2] = tonemap(src[2]);
                faceData[idx + 3] = 255;
            }
        }
    }
    stbi_image_free(hdrData);

    AttachSkybox(world, sceneGraph, device, faceSize, allFaces);
    return true;
}

} // namespace he::samples
