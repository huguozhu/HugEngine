#pragma once

// ============================================================
//  SkyboxLoader —— 鸿蒙三个 App（02 / 03 / 04）共用的天空盒加载
//
//  两种资产布局：
//    · 6 张 PNG 面（02.Cube）：Textures/skybox/daylight{0..5}.png
//    · 等距柱状 HDR（03/04）：Textures/skybox.hdr → 6 面（512²，Reinhard 色调映射）
//
//  【为什么单独一个 TU】三个 App 都编进同一个 libentry.so，stb_image 的实现只能
//  有一份。原来 02 没加 STB_IMAGE_STATIC、03/04 加了，靠"恰好只有一个非 static
//  实现"躲过重复定义；实现收口到这里后，重复定义的风险从结构上消除。
// ============================================================

namespace he {

class World;
class SceneGraph;

namespace rhi {
class IRHIDevice;
}

namespace samples {

/// 6 张 PNG 面 → Cubemap，并挂到场景的 SkyboxComponent 上（02.Cube 的布局）。
/// 返回 false 表示某张面加载失败（此时不改动场景）。
bool CreateSkyboxFromPngFaces(World& world, SceneGraph& sceneGraph, rhi::IRHIDevice* device);

/// 等距柱状 HDR → 6 面 Cubemap（03/04 的布局），面大小固定 512。
/// 返回 false 表示 HDR 加载失败（此时不改动场景）。
bool CreateSkyboxFromHdr(World& world, SceneGraph& sceneGraph, rhi::IRHIDevice* device);

} // namespace samples
} // namespace he
