#pragma once

#include "RHI/Types.h"   // DescriptorSetHandle / u32
#include "RHI/Buffer.h"  // IRHIBuffer / IRHITexture / IRHISampler

// ============================================================
// Bindless.h — RHI 层统一 Bindless 描述符堆抽象
//
// 把「资源绑定」从 CPU per-draw 描述符集变成 GPU 按 uint 索引取：
// 调用方只 Register* → handle，不碰 descriptor set / binding / Vulkan flag。
// 一个堆统一管理纹理 + 采样器 + StorageBuffer（SSBO）三类资源。
//
// 任务 23：堆支持**槽位回收**（BindlessSlotRing）——高频更新（文字/实例变换）不再
// 需要"旧资源无界保活 + 槽位只增不减"，见 Release*/BeginFrame。
// ============================================================

namespace he::rhi {

// Bindless 句柄（最小版 = 该类型数组内的索引，shader 直接当索引用）
using BindlessHandle = u32;

class IRHIBindlessHeap {
public:
    virtual ~IRHIBindlessHeap() = default;

    // --- 描述符集登记 ---
    // 管线初始化时调用，登记「堆要写哪些帧描述符集 + 各类型 binding 号」。
    // bufferBinding = 0 表示该集不含 SSBO 数组（如 RT set=0 只登记纹理/采样器）。
    virtual void RegisterDescriptorSet(DescriptorSetHandle set,
                                       u32 textureBinding,
                                       u32 samplerBinding,
                                       u32 bufferBinding = 0) = 0;

    // --- 资源注册（返回该类型数组内索引，自动标记待 flush） ---
    virtual BindlessHandle RegisterTexture(IRHITexture* texture, IRHISampler* sampler) = 0;
    virtual BindlessHandle RegisterSampler(IRHISampler* sampler) = 0;
    virtual BindlessHandle RegisterBuffer(IRHIBuffer* ssbo) = 0;   // StorageBuffer

    // --- 槽位回收（任务 23：环形化，替代"append-only + 旧资源无界保活"）---
    // 语义：Release* 只登记"该句柄不再使用"，槽位内容保持原样，经过 graceFrames 帧
    // （= 飞行帧数）后才可被 Register* 复用 —— 保护期内仍在执行的帧仍按原内容取值。
    // 调用方义务：资源本身也要延迟同样帧数再销毁（用 FrameRetireQueue），否则保护期内
    // 描述符会指向已销毁的资源。
    virtual void ReleaseTexture(BindlessHandle handle) { (void)handle; }
    virtual void ReleaseSampler(BindlessHandle handle) { (void)handle; }
    virtual void ReleaseBuffer(BindlessHandle handle)  { (void)handle; }

    /// 帧边界推进（每帧一次）：把保护期已过的槽位放回空闲表
    virtual void BeginFrame(u64 frameIndex) { (void)frameIndex; }

    // --- 槽位统计（调试 / 判据：验证"不再无限增长"）---
    virtual u32 GetTextureSlotCount()     const { return GetTextureCount(); }   // 已分配槽位总数
    virtual u32 GetFreeTextureSlotCount() const { return 0; }
    virtual u32 GetBufferSlotCount()      const { return GetBufferCount(); }
    virtual u32 GetFreeBufferSlotCount()  const { return 0; }

    // --- 同步：把 pending 变更写入所有已登记描述符集 ---
    virtual void Flush() = 0;

    // --- 查询（调试 / shader 常量对齐） ---
    virtual u32 GetTextureCount() const = 0;
    virtual u32 GetBufferCount() const = 0;

    // --- 占位资源（null 纹理/采样器回退用，须在 RegisterTexture 前调用） ---
    virtual void SetDefaultTexture(IRHITexture* texture, IRHISampler* sampler) = 0;
};

} // namespace he::rhi
