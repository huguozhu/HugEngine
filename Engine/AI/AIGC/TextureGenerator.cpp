#include "AI/AIGC/TextureGenerator.h"

#include "Core/Log.h"

#include "nlohmann/json.hpp"

// stb_image_write 为 header-only：本编译单元提供其实现（写 PNG 用）
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstring>
#include <filesystem>

using nlohmann::json;

namespace he::ai::aigc {

namespace {

// 随机数（固定种子，保证同规格生成结果可复现）
u32 g_Seed = 12345;
u32 NextRand() {
    g_Seed = g_Seed * 1664525u + 1013904223u;
    return g_Seed >> 8;
}

// 颜色 [0,1] float3 → 0..255 字节
u8 ToByte(float v) {
    int c = (int)(v * 255.0f);
    return (u8)(c < 0 ? 0 : (c > 255 ? 255 : c));
}

} // namespace

bool TextureGenerator::Generate(const TextureSpec& spec, std::vector<u8>& outRGBA) {
    const u32 size = (spec.size == 0) ? 64 : spec.size;
    outRGBA.assign(size * size * 4, 0);

    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            u8* p = &outRGBA[(y * size + x) * 4];
            float3 c = spec.colorA;   // 默认主色

            if (spec.pattern == "solid") {
                c = spec.colorA;
            } else if (spec.pattern == "gradient") {
                // 从左到右 colorA → colorB
                float t = (size <= 1) ? 0.0f : (float)x / (float)(size - 1);
                c = spec.colorA * (1.0f - t) + spec.colorB * t;
            } else if (spec.pattern == "checker") {
                // 8×8 棋盘
                const u32 cell = size / 8;
                bool on = ((x / cell) + (y / cell)) % 2 == 0;
                c = on ? spec.colorA : spec.colorB;
            } else if (spec.pattern == "noise") {
                // 灰度随机（colorA 亮度为基底）
                float n = (float)(NextRand() & 255) / 255.0f;
                c = float3(n, n, n) * 0.7f + spec.colorA * 0.3f;
            } else {
                HE_CORE_WARN("[TextureGenerator] 未知 pattern '{}'，回退 gradient", spec.pattern);
                float t = (size <= 1) ? 0.0f : (float)x / (float)(size - 1);
                c = spec.colorA * (1.0f - t) + spec.colorB * t;
            }

            p[0] = ToByte(c.x);
            p[1] = ToByte(c.y);
            p[2] = ToByte(c.z);
            p[3] = 255;
        }
    }
    return true;
}

bool TextureGenerator::WritePNG(const String& path, u32 w, u32 h, const u8* rgba) {
    // 确保目录存在
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);

    // stbi_write_png 成功返回非 0
    int ok = stbi_write_png(path.c_str(), (int)w, (int)h, 4, rgba, (int)w * 4);
    if (!ok) {
        HE_CORE_ERROR("[TextureGenerator] PNG 写入失败: {}", path);
        return false;
    }
    HE_CORE_INFO("[TextureGenerator] 已生成纹理资产: {} ({}x{})", path, w, h);
    return true;
}

bool TextureGenerator::ParseSpec(const String& jsonText, TextureSpec& out) {
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const std::exception&) {
        return false;
    }

    // pattern（默认 gradient）
    if (j.contains("pattern") && j["pattern"].is_string())
        out.pattern = j["pattern"].get<String>();

    // size
    if (j.contains("size") && j["size"].is_number_integer()) {
        int s = j["size"].get<int>();
        out.size = (u32)(s < 8 ? 8 : (s > 512 ? 512 : s));   // 钳制 [8,512]
    }

    // 颜色（数组 [r,g,b] 0~1）
    auto readColor = [&](const char* key, float3& dst) {
        if (j.contains(key) && j[key].is_array() && j[key].size() >= 3) {
            dst = float3(j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>());
        }
    };
    readColor("colorA", out.colorA);
    readColor("colorB", out.colorB);
    return true;
}

String TextureGenerator::BuildSpecPrompt() {
    return R"(
生成一个纹理规格 JSON，用于程序化生成 2D 纹理。格式：
{"pattern":"solid|gradient|checker|noise","size":64,"colorA":[r,g,b],"colorB":[r,g,b]}
规则：
- pattern: solid=纯色, gradient=线性渐变(左colorA→右colorB), checker=8x8棋盘, noise=噪声
- size: 8~512 的整数（默认 64）
- color: [r,g,b] 各 0~1
- 只输出 JSON，不要输出解释文字。
)";
}

} // namespace he::ai::aigc
