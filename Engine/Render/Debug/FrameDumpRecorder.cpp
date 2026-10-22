// 帧中间结果转储器的实现（机制）。策略（目标清单）由使用方给出，见头文件说明。

#include "Debug/FrameDumpRecorder.h"

#include "Core/Log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace he::render {

FrameDumpRecorder FrameDumpRecorder::FromEnv() {
    return FromEnv("gi_", "HE_DUMP_GI", "HE_DUMP_GI_FRAME");
}

FrameDumpRecorder FrameDumpRecorder::FromEnv(const char* prefix, const char* tagEnvName,
                                             const char* frameEnvName) {
    FrameDumpRecorder r;
    r.m_Prefix = (prefix && *prefix) ? prefix : "gi_";
    const char* tagEnv   = tagEnvName ? std::getenv(tagEnvName) : nullptr;
    const char* frameEnv = frameEnvName ? std::getenv(frameEnvName) : nullptr;
    r.m_Tag          = tagEnv ? tagEnv : "";
    r.m_Active       = !r.m_Tag.empty();
    r.m_FrameIndex   = frameEnv ? (u64)std::max(1, std::atoi(frameEnv)) : 60ull;
    r.m_IncludeBuffers = (std::getenv("HE_DUMP_LUMEN_PROBES") != nullptr);
    if (r.m_Active) {
        HE_CORE_INFO("[采样落盘] 已启用：前缀={} 标签={} 目标帧={} 输出 build/verify/{}{}_*.f16",
                     r.m_Prefix, r.m_Tag, r.m_FrameIndex, r.m_Prefix, r.m_Tag);
    }
    return r;
}

u32 FrameDumpRecorder::BytesPerPixelOf(rhi::Format f) {
    // 按纹理**实际格式**算 bytes/px：此前写死 8 B（RGBA16F），而 mesh 级距离场是 R16F/R32F —
    // 写死会让读回缓冲的其余部分保持未初始化，两趟对比就会把"未初始化内存"当成差异。
    switch (f) {
    case rhi::Format::R16_FLOAT:    return 2;
    case rhi::Format::RG16_FLOAT:   return 4;
    case rhi::Format::R32_FLOAT:    return 4;
    case rhi::Format::RGBA16_FLOAT: return 8;
    default:                        return 8;
    }
}

void FrameDumpRecorder::AddTexture(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd,
                                   const char* name, rhi::IRHITexture* tex) {
    if (!m_Active || !device || !cmd || !tex) return;
    AddTexture(device, cmd, name, tex, BytesPerPixelOf(tex->GetFormat()), nullptr);
}

void FrameDumpRecorder::AddTexture(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd,
                                   const char* name, rhi::IRHITexture* tex,
                                   u32 bytesPerPixel, const char* formatTag) {
    if (!m_Active || !device || !cmd || !tex) return;

    Target t;
    t.name = name;
    t.tex  = tex;
    t.w    = tex->GetWidth();
    t.h    = tex->GetHeight();
    t.bytesPerPixel = bytesPerPixel ? bytesPerPixel : BytesPerPixelOf(tex->GetFormat());
    t.formatTag = formatTag ? formatTag : "";

    rhi::BufferDesc dd;                             // 宽 × 高 × bpp 字节
    dd.size      = (usize)t.w * t.h * t.bytesPerPixel;
    dd.usage     = rhi::BufferUsage::Storage;       // 该路径恒定带 TRANSFER_DST，可作拷贝目标
    dd.cpuAccess = true;                            // 需要 Map 读回
    t.buf = device->CreateBuffer(dd);
    if (!t.buf) {
        HE_CORE_ERROR("[GI采样] 读回缓冲创建失败: {}（{}x{}）", name, t.w, t.h);
        return;
    }
    // x=y=0 且取满宽高 ⇒ bufferRowLength=0 的紧密排布正好等于线性落盘布局
    cmd->CopyTextureToBuffer(tex, t.buf.get(), 0, 0, t.w, t.h, 0);
    m_Targets.push_back(std::move(t));
}

void FrameDumpRecorder::AddBuffer(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd,
                                  const char* name, rhi::IRHIBuffer* src, usize bytes) {
    if (!m_Active || !device || !cmd || !src || bytes == 0) return;

    Target t;
    t.name     = name;
    t.isBuffer = true;
    t.bytes    = bytes;

    rhi::BufferDesc dd;
    dd.size      = bytes;
    dd.usage     = rhi::BufferUsage::Storage;   // 该路径恒定带 TRANSFER_DST
    dd.cpuAccess = true;
    t.buf = device->CreateBuffer(dd);
    if (!t.buf) {
        HE_CORE_ERROR("[GI采样] 缓冲读回创建失败: {}（{} B）", name, (u64)bytes);
        return;
    }
    cmd->CopyBuffer(src, t.buf.get(), bytes, 0, 0);
    m_Targets.push_back(std::move(t));
}

void FrameDumpRecorder::FinishRecording(rhi::IRHIDevice* device, rhi::IRHICommandList* cmd) {
    if (!m_Active || !device || !cmd) return;

    if (m_Targets.empty()) {
        m_Active = false;
        HE_CORE_WARN("[采样落盘] 没有可用目标，本次跳过");
        return;
    }
    m_Done = true;   // 已录制；实际读取放在 Submit 之后
    // 【A-5】给本帧挂 timeline 信号：读取侧只等这一次提交（见 TryWriteBack）
    if (!m_Fence) m_Fence = std::make_unique<CrossThreadFence>();
    if (m_Fence->fence.load() == rhi::kInvalidFence) {
        m_Fence->fence.store(device->CreateFence());   // 拥有线程（渲染线程）上创建
    }
    const u64 fv = m_Fence->value.load() + 1ull;
    m_Fence->value.store(fv);
    cmd->SetTimelineSignal(m_Fence->fence.load(), fv);
}

bool FrameDumpRecorder::TryWriteBack(rhi::IRHIDevice* device, const CameraSnapshot& camera) {
    if (!m_Active || !m_Done || m_Written || !device) return false;

    // 【A-5】只等本帧那次提交：先让 RHI 线程把已排队的提交落地（模式 2），再等 timeline 到本帧的值；
    // 不再做设备级 vkDeviceWaitIdle（那会连带等掉别的在飞工作，且掩盖"提交未落地"的真问题）。
    if (auto* router = device->GetSubmitRouter(); router != nullptr) router->FlushAndWait();
    const auto fenceH = m_Fence ? m_Fence->fence.load() : rhi::kInvalidFence;
    if (fenceH != rhi::kInvalidFence) {
        device->WaitForFence(fenceH, m_Fence->value.load(), UINT64_MAX);
    } else {
        device->WaitIdle();   // 兜底（理论上不会走到：录制时就建了）
    }

    const String dir  = "build/verify/";
    const String base = dir + m_Prefix + m_Tag;
    std::filesystem::create_directories(dir);
    std::ofstream meta(base + "_meta.txt");
    for (auto& t : m_Targets) {                        // 逐目标写：像素直落，无文件头
        const usize bytes = t.isBuffer ? t.bytes : ((usize)t.w * t.h * t.bytesPerPixel);
        const void* p = t.buf ? t.buf->Map() : nullptr;
        if (!p) {
            HE_CORE_ERROR("[GI采样] 映射失败: {}_{}", base, t.name);
            continue;
        }
        // 缓冲目标写 .bin（原始字节）；纹理目标写 .f16（RGBA16F 等原始像素）
        std::ofstream f(base + "_" + t.name + (t.isBuffer ? ".bin" : ".f16"), std::ios::binary);
        f.write(static_cast<const char*>(p), (std::streamsize)bytes);
        t.buf->Unmap();
        String fmt;
        if (t.isBuffer)                  fmt = "RAWBUFFER";
        else if (!t.formatTag.empty())   fmt = t.formatTag;                 // 调用方给的字样（05 的脚本依赖它）
        else                             fmt = std::to_string(t.bytesPerPixel * 8u) + "bit";
        meta << t.name << " " << t.w << " " << t.h << " " << fmt << "\n";
        HE_CORE_INFO("[采样落盘] {}_{}.f16  {}x{}  {} B", base, t.name, t.w, t.h, bytes);
    }
    HE_CORE_INFO("[采样落盘] 共落盘 {} 个目标", m_Targets.size());

    // 相机参数一并落盘：解析对照需要"渲染这一帧时"的相机参数，硬编码的会随 cfg 改动失效。
    {
        std::ofstream cm(base + "_camera.txt");
        cm << "pos "     << camera.pos[0]     << " " << camera.pos[1]     << " " << camera.pos[2]     << "\n";
        cm << "forward " << camera.forward[0] << " " << camera.forward[1] << " " << camera.forward[2] << "\n";
        cm << "up "      << camera.up[0]      << " " << camera.up[1]      << " " << camera.up[2]      << "\n";
        cm << "fov "     << camera.fov        << "\n";
        cm << "near "    << camera.nearPlane  << "\n";
        cm << "far "     << camera.farPlane   << "\n";
        cm << "aspect "  << camera.aspect     << "\n";
    }
    m_Written = true;
    return true;
}

} // namespace he::render
