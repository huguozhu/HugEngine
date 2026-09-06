// ============================================================
// TextRenderSystem.cpp — 文字栅格化（stb_truetype + 兜底字体）+ 纹理管理
// ============================================================

#include "Scene/TextRenderSystem.h"

#include "Scene/TextRenderComponent.h"
#include "Scene/World.h"
#include "Core/Log.h"

#include <fstream>
#include <algorithm>
#include <cstring>

// stb_truetype：矢量字体栅格化（支持 TTF/TTC）
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
// stb_easy_font：内置 ASCII 位图字体（所有字体都找不到时的最终兜底）
#define STB_EASY_FONT_IMPLEMENTATION
#include "stb_easy_font.h"

namespace he {

namespace {

// 系统字体兜底链（Windows 常见字体，覆盖 CJK）
const char* kFallbackFonts[] = {
    "C:/Windows/Fonts/msyh.ttc",    // 微软雅黑（默认含中文）
    "C:/Windows/Fonts/simhei.ttf",  // 黑体
    "C:/Windows/Fonts/simsun.ttc",  // 宋体
    "C:/Windows/Fonts/arial.ttf",   // Arial
};

// 读取文件全部字节；失败返回空
std::vector<u8> ReadFileBytes(const String& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<u8> data((usize)size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

// UTF-8 解码一个码点；返回码点（0 = 解码失败，按单字节吞掉）
u32 DecodeUtf8(const String& s, usize& i) {
    const u8* p = reinterpret_cast<const u8*>(s.data());
    u8 c = p[i];
    if (c < 0x80) { i += 1; return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        u32 cp = ((c & 0x1F) << 6) | (p[i+1] & 0x3F); i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        u32 cp = ((c & 0x0F) << 12) | ((p[i+1] & 0x3F) << 6) | (p[i+2] & 0x3F); i += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        u32 cp = ((c & 0x07) << 18) | ((p[i+1] & 0x3F) << 12) | ((p[i+2] & 0x3F) << 6) | (p[i+3] & 0x3F); i += 4; return cp;
    }
    i += 1;   // 非法序列：吞掉一字节继续
    return 0;
}

// stb_truetype 栅格化（bottom-up 行序）；失败返回 false
bool RasterizeWithTTF(const std::vector<u8>& fontData, const String& text, float fontSize,
                      u32& outW, u32& outH, std::vector<u8>& outPixels) {
    stbtt_fontinfo info;
    int offset = stbtt_GetFontOffsetForIndex(fontData.data(), 0);  // .ttc 取第 0 个面；.ttf 返回 0
    if (offset < 0 || !stbtt_InitFont(&info, fontData.data(), offset)) return false;

    float scale = stbtt_ScaleForPixelHeight(&info, fontSize);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);

    // 位图高度 = 升部+降部（+2 边距），基线 = 升部+1
    u32 H = (u32)((ascent - descent) * scale) + 2;
    float baseline = ascent * scale + 1.0f;

    // 第一遍：累计宽度
    float penX = 0.0f;
    usize i = 0;
    while (i < text.size()) {
        u32 cp = DecodeUtf8(text, i);
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, cp, &advance, &lsb);
        penX += advance * scale;
    }
    u32 W = std::max(1u, (u32)(penX + 1.0f));

    // 位图（先按 top-down 生成，最后翻转成 bottom-up）
    std::vector<u8> topDown((usize)W * H * 4, 0);
    auto BlendPixel = [&](u32 px, u32 py, u8 coverage) {
        if (px >= W || py >= H) return;
        usize idx = ((usize)py * W + px) * 4;
        u8& a = topDown[idx + 3];
        a = (u8)std::max((int)a, (int)coverage);
    };

    // 第二遍：逐码点栅格化字形（灰度 → alpha，白色）
    penX = 0.0f;
    i = 0;
    while (i < text.size()) {
        u32 cp = DecodeUtf8(text, i);
        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, cp, &advance, &lsb);
        int glyph = stbtt_FindGlyphIndex(&info, cp);
        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        unsigned char* gb = glyph > 0
            ? stbtt_GetGlyphBitmap(&info, scale, scale, glyph, &gw, &gh, &xoff, &yoff)
            : nullptr;
        if (gb) {
            // 字形位图左上角位于 (penX + xoff, baseline + yoff)（top-down 行序；
            // stb_truetype 的 yoff 为负 = 字形顶部在基线上方）
            int gx0 = (int)(penX + xoff);
            int gy0 = (int)(baseline + yoff);
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x)
                    BlendPixel((u32)(gx0 + x), (u32)(gy0 + y), gb[y * gw + x]);
            stbtt_FreeBitmap(gb, nullptr);
        }
        penX += advance * scale;
    }

    // 翻转为 bottom-up（UV 原点在左下，Bit 位图第一行 = 屏幕底）
    outPixels.assign((usize)W * H * 4, 0);
    for (u32 y = 0; y < H; ++y) {
        u32 srcRow = H - 1 - y;
        for (u32 x = 0; x < W; ++x) {
            usize src = ((usize)srcRow * W + x) * 4;
            usize dst = ((usize)y * W + x) * 4;
            outPixels[dst + 0] = 255;                  // 白字（颜色由材质 baseColor 乘出）
            outPixels[dst + 1] = 255;
            outPixels[dst + 2] = 255;
            outPixels[dst + 3] = topDown[src + 3];     // alpha = 字形覆盖
        }
    }
    outW = W; outH = H;
    return true;
}

// stb_easy_font 兜底（内置 ASCII 位图字体，仅支持 ASCII 32~126）
bool RasterizeWithEasyFont(const String& text, float fontSize,
                           u32& outW, u32& outH, std::vector<u8>& outPixels) {
    // 只保留 ASCII（非 ASCII 字符用 '?' 替代，保证任何文字都有可见输出）
    String ascii = text;
    for (auto& ch : ascii)
        if ((u8)ch < 32 || (u8)ch > 126) ch = '?';
    if (ascii.empty()) ascii = "?";

    // 每字符 16 个顶点（4 quad × 4 分量），预分配充足空间
    const usize kMaxQuads = ascii.size() * 2;
    std::vector<float> buf(kMaxQuads * 64, 0.0f);
    int numQuads = stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(ascii.c_str()),
                                       nullptr, buf.data(), (int)buf.size() * (int)sizeof(float));
    if (numQuads <= 0) return false;

    // easy_font 字形约 8 像素高，按 fontSize 缩放
    float s = fontSize / 10.0f;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int q = 0; q < numQuads; ++q) {
        const float* v = buf.data() + q * 16;
        for (int k = 0; k < 4; ++k) {
            minX = std::min(minX, v[k*4+0] * s); maxX = std::max(maxX, v[k*4+0] * s);
            minY = std::min(minY, v[k*4+1] * s); maxY = std::max(maxY, v[k*4+1] * s);
        }
    }
    u32 W = (u32)(maxX - minX) + 4;
    u32 H = (u32)(maxY - minY) + 4;
    std::vector<u8> topDown((usize)W * H * 4, 0);
    // 每个 quad 用包围盒填充（字体小，视觉可读；MVP 精度足够）
    for (int q = 0; q < numQuads; ++q) {
        const float* v = buf.data() + q * 16;
        int qx0 = 1e9, qy0 = 1e9, qx1 = -1e9, qy1 = -1e9;
        for (int k = 0; k < 4; ++k) {
            qx0 = std::min(qx0, (int)(v[k*4+0] * s)); qx1 = std::max(qx1, (int)(v[k*4+0] * s));
            qy0 = std::min(qy0, (int)(v[k*4+1] * s)); qy1 = std::max(qy1, (int)(v[k*4+1] * s));
        }
        for (int y = qy0; y <= qy1; ++y)
            for (int x = qx0; x <= qx1; ++x) {
                u32 px = (u32)(x - (int)minX + 1), py = (u32)(y - (int)minY + 1);
                if (px < W && py < H) topDown[((usize)py * W + px) * 4 + 3] = 255;
            }
    }

    // 翻转为 bottom-up
    outPixels.assign((usize)W * H * 4, 0);
    for (u32 y = 0; y < H; ++y) {
        u32 srcRow = H - 1 - y;
        for (u32 x = 0; x < W; ++x) {
            usize src = ((usize)srcRow * W + x) * 4;
            usize dst = ((usize)y * W + x) * 4;
            outPixels[dst + 0] = 255;
            outPixels[dst + 1] = 255;
            outPixels[dst + 2] = 255;
            outPixels[dst + 3] = topDown[src + 3];
        }
    }
    outW = W; outH = H;
    return true;
}

} // namespace

bool TextRenderSystem::RasterizeText(const String& text, const String& fontPath, float fontSize,
                                     u32& outW, u32& outH, std::vector<u8>& outPixels) {
    if (text.empty()) return false;
    fontSize = std::clamp(fontSize, 8.0f, 512.0f);

    // 1. 指定字体
    if (!fontPath.empty()) {
        auto data = ReadFileBytes(fontPath);
        if (!data.empty() && RasterizeWithTTF(data, text, fontSize, outW, outH, outPixels))
            return true;
        HE_CORE_WARN("[TextRender] 字体加载失败，尝试系统字体兜底: {}", fontPath);
    }
    // 2. 系统字体兜底链（CJK）
    for (const char* f : kFallbackFonts) {
        auto data = ReadFileBytes(f);
        if (!data.empty() && RasterizeWithTTF(data, text, fontSize, outW, outH, outPixels))
            return true;
    }
    // 3. 内置 ASCII 位图字体（最终兜底）
    if (RasterizeWithEasyFont(text, fontSize, outW, outH, outPixels)) {
        HE_CORE_WARN("[TextRender] 所有系统字体不可用，使用内置 ASCII 位图字体（CJK 显示为 ?）");
        return true;
    }
    return false;
}

void TextRenderSystem::Update(World& world, rhi::IRHIDevice* device) {
    world.ForEach<TextRenderComponent>([&](Entity, TextRenderComponent& t) {
        // 文字颜色 → 材质色（每帧同步，无需重栅格化）
        t.baseColorFactor = t.textColor;
        if (!t.IsDirty()) return;

        // 重栅格化 → 更新广告牌尺寸（每 100 像素 = 1 米）
        u32 w = 0, h = 0;
        std::vector<u8> px;
        if (!RasterizeText(t.text, t.fontPath, t.fontSize, w, h, px)) {
            t.ClearDirty();
            return;
        }
        t.size = float2((float)w, (float)h) / 100.0f;

        // 有设备时：重建纹理 + 注册 bindless（materialID 指向 baseColor 槽）
        if (device) {
            rhi::TextureDesc td;
            td.format      = rhi::Format::RGBA8_UNORM;
            td.width       = w;
            td.height      = h;
            td.mipLevels   = 1;
            td.usage       = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::TransferDst;
            td.initialData = px.data();
            auto tex = device->CreateTexture(td);

            rhi::SamplerDesc sd;
            sd.minFilter = sd.magFilter = rhi::FilterMode::Linear;
            sd.addressU = sd.addressV = rhi::AddressMode::ClampToEdge;
            auto samp = device->CreateSampler(sd);

            u32 handle = device->GetBindlessHeap()->RegisterTexture(tex.get(), samp.get());
            t.materialID = handle;
            // 标记 baseColor 槽有纹理（textureMask bit0），shader 只采样该槽
            t.baseColorTexture = "__text_runtime__";
            // 追加持有（bindless 堆 append-only，旧纹理不销毁避免悬垂指针）
            t.AddRuntimeTexture(std::move(tex), std::move(samp));
        }
        t.ClearDirty();
    });
}

} // namespace he
