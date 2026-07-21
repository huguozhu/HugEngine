// VulkanDevice_Descriptors.cpp — 描述符集布局、分配与更新
// 从 VulkanDevice.cpp 拆分，包含：
//   - CreateDescriptorSetLayout / AllocateDescriptorSet
//   - UpdateDescriptorSet ×5（Buffer / Texture+Sampler / 数组 / AS / ImageView）
//   - Per-Mip ImageView 创建/销毁
//   - EnsureDescriptorPool / ToVkDescType

#include "RHI/RHI.h"
#include "RHI/Buffer.h"
#include "Core/Log.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "VulkanDevice.h"
#include "Core/Assert.h"

#include <algorithm>
#include <vector>

namespace he::rhi {

// ============================================================
// Descriptor Set Implementation
// ============================================================

VkDescriptorType VulkanDevice::ToVkDescType(DescriptorType type) const {
    switch (type) {
        case DescriptorType::UniformBuffer:         return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::StorageBuffer:         return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::CombinedImageSampler:  return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case DescriptorType::StorageImage:          return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case DescriptorType::SampledImage:          return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::Sampler:               return VK_DESCRIPTOR_TYPE_SAMPLER;
        case DescriptorType::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        default:                                    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
}

// ============================================================
// 描述符池容量常量 — Vulkan 后端资源预算
// 需根据引擎实际负载（bindless 纹理数、材质数、RT 集数）调整
// ============================================================
static constexpr u32 kDescPoolSize_UniformBuffer         = 64;    // 逐帧 UBO 数量
static constexpr u32 kDescPoolSize_StorageBuffer         = 1024;  // SSBO（Object/Light/Meshlet 等）
static constexpr u32 kDescPoolSize_CombinedImageSampler  = 8192;  // bindless 纹理数组
static constexpr u32 kDescPoolSize_SampledImage          = 4096;  // 采样图像（非组合）
static constexpr u32 kDescPoolSize_StorageImage          = 256;   // StorageImage（RT BackBuffer 等）
static constexpr u32 kDescPoolSize_Sampler               = 4096;  // bindless 采样器数组
static constexpr u32 kDescPoolSize_AccelStruct           = 64;    // RT TLAS 绑定
static constexpr u32 kDescPoolMaxSets                    = 1024;  // 最大描述符集总数

void VulkanDevice::EnsureDescriptorPool() {
    if (m_DescPool != VK_NULL_HANDLE) return;

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           kDescPoolSize_UniformBuffer },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           kDescPoolSize_StorageBuffer },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   kDescPoolSize_CombinedImageSampler }, // bindless 纹理数组
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,            kDescPoolSize_SampledImage },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            kDescPoolSize_StorageImage },          // StorageImage（RT BackBuffer 等）
        { VK_DESCRIPTOR_TYPE_SAMPLER,                  kDescPoolSize_Sampler },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kDescPoolSize_AccelStruct },         // RT TLAS 绑定
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = kDescPoolMaxSets;  // 需容纳 bindless + per-frame 描述符集
    poolInfo.poolSizeCount = static_cast<u32>(std::size(poolSizes));
    poolInfo.pPoolSizes    = poolSizes;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
                            | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

    vkCreateDescriptorPool(m_Device, &poolInfo, nullptr, &m_DescPool);
}

DescriptorSetLayoutHandle VulkanDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) {
    DescLayoutInfo info;
    std::vector<VkDescriptorSetLayoutBinding> vkBindings;
    // 找到最后一个 bindless binding 的索引（Vulkan 要求 VARIABLE_DESCRIPTOR_COUNT_BIT
    // 只能设置在最后一个 binding 上）
    i32 lastBindlessIdx = -1;
    for (i32 i = 0; i < (i32)desc.bindings.size(); ++i) {
        if (desc.bindings[i].bindless)
            lastBindlessIdx = i;
    }

    for (i32 i = 0; i < (i32)desc.bindings.size(); ++i) {
        auto& b = desc.bindings[i];
        VkDescriptorSetLayoutBinding vb{};
        vb.binding            = b.binding;
        vb.descriptorType     = ToVkDescType(b.type);
        vb.descriptorCount    = b.count;
        vb.stageFlags = b.stageMask;
        vkBindings.push_back(vb);

        info.bindings.push_back(b);

        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                                       | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        if (b.bindless && i == lastBindlessIdx) {
            // 只有最后一个 bindless binding 允许设置 VARIABLE_COUNT
            flags |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        }
        info.bindingFlags.push_back(flags);
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount  = static_cast<u32>(info.bindingFlags.size());
    flagsInfo.pBindingFlags = info.bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.pNext        = &flagsInfo;
    layoutInfo.bindingCount = static_cast<u32>(vkBindings.size());
    layoutInfo.pBindings    = vkBindings.data();

    VkDescriptorSetLayout layout;
    vkCreateDescriptorSetLayout(m_Device, &layoutInfo, nullptr, &layout);
    info.layout = layout;

    DescriptorSetLayoutHandle handle = static_cast<DescriptorSetLayoutHandle>(m_DescLayoutInfos.size() + 1);
    m_DescLayoutInfos.push_back(info);
    return handle;
}

DescriptorSetHandle VulkanDevice::AllocateDescriptorSet(DescriptorSetLayoutHandle layoutHandle) {
    EnsureDescriptorPool();
    if (layoutHandle == 0 || layoutHandle > m_DescLayoutInfos.size()) return kInvalidSet;

    auto& info = m_DescLayoutInfos[static_cast<usize>(layoutHandle - 1)];
    VkDescriptorSetLayout layout = info.layout;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_DescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &layout;

    // 处理 bindless 可变描述符数量
    VkDescriptorSetVariableDescriptorCountAllocateInfo varCountInfo{};
    u32 maxVarCount = 0;
    bool hasBindless = false;
    for (usize i = 0; i < info.bindings.size(); ++i) {
        if (info.bindings[i].bindless) {
            maxVarCount = std::max(maxVarCount, info.bindings[i].count);
            hasBindless = true;
        }
    }
    if (hasBindless) {
        varCountInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        varCountInfo.descriptorSetCount = 1;
        varCountInfo.pDescriptorCounts = &maxVarCount;
        allocInfo.pNext = &varCountInfo;
    }

    VkDescriptorSet ds;
    VkResult result = vkAllocateDescriptorSets(m_Device, &allocInfo, &ds);
    if (result != VK_SUCCESS) return kInvalidSet;

    DescriptorSetHandle handle = static_cast<DescriptorSetHandle>(m_DescSets.size() + 1);
    m_DescSets.push_back(ds);
    m_DescSetLayoutParents.push_back(layoutHandle);
    return handle;
}

// ============================================================
// UpdateDescriptorSet — Buffer 绑定
// ============================================================
void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle setHandle, u32 binding,
                                        DescriptorType type, IRHIBuffer* buffer) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];

    auto* vkBuf = static_cast<VulkanBuffer*>(buffer);
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = vkBuf->GetHandle();
    bufInfo.offset = 0;
    bufInfo.range  = vkBuf->GetSize();

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = ToVkDescType(type);
    write.pBufferInfo     = &bufInfo;

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

// ============================================================
// UpdateDescriptorSet — 纹理数组绑定
// ============================================================
void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle setHandle, u32 binding,
                                        DescriptorType type,
                                        IRHITexture** textures, IRHISampler** samplers,
                                        u32 count) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    if (count == 0) return;
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];
    if (ds == VK_NULL_HANDLE) return;

    VkDescriptorType vkType = ToVkDescType(type);
    std::vector<VkDescriptorImageInfo> imageInfos(count);
    for (u32 i = 0; i < count; ++i) {
        // SampledImage: 仅使用 imageView，sampler 字段忽略
        if (vkType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            if (!textures || !textures[i]) {
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfos[i].imageView   = VK_NULL_HANDLE;
                imageInfos[i].sampler     = VK_NULL_HANDLE;
                continue;
            }
            auto* vkTex = static_cast<VulkanTexture*>(textures[i]);
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[i].imageView   = vkTex->GetImageView();
            imageInfos[i].sampler     = VK_NULL_HANDLE;
            continue;
        }
        // Sampler: 仅使用 sampler，imageView 字段忽略
        if (vkType == VK_DESCRIPTOR_TYPE_SAMPLER) {
            if (!samplers || !samplers[i]) {
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                imageInfos[i].imageView   = VK_NULL_HANDLE;
                imageInfos[i].sampler     = VK_NULL_HANDLE;
                continue;
            }
            auto* vkSampler = static_cast<VulkanSampler*>(samplers[i]);
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[i].imageView   = VK_NULL_HANDLE;
            imageInfos[i].sampler     = vkSampler->GetHandle();
            continue;
        }
        // CombinedImageSampler: 同时使用 imageView + sampler
        if (!textures || !textures[i] || !samplers || !samplers[i]) {
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos[i].imageView   = VK_NULL_HANDLE;
            imageInfos[i].sampler     = VK_NULL_HANDLE;
            continue;
        }
        auto* vkTex     = static_cast<VulkanTexture*>(textures[i]);
        auto* vkSampler = static_cast<VulkanSampler*>(samplers[i]);
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[i].imageView   = vkTex->GetImageView();
        imageInfos[i].sampler     = vkSampler->GetHandle();
    }

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = count;
    write.descriptorType  = vkType;
    write.pImageInfo      = imageInfos.data();

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

// ============================================================
// UpdateDescriptorSet — 单纹理 + 采样器绑定
// ============================================================
void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle setHandle, u32 binding,
                                        DescriptorType type, IRHITexture* texture,
                                        IRHISampler* sampler) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    if (!texture || !sampler) return;
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];
    if (ds == VK_NULL_HANDLE) return;

    auto* vkTex     = static_cast<VulkanTexture*>(texture);
    auto* vkSampler = static_cast<VulkanSampler*>(sampler);

    VkImageView imgView = vkTex->GetImageView();
    VkSampler   vkSamp  = vkSampler->GetHandle();

    if (imgView == VK_NULL_HANDLE || imgView == (VkImageView)0xdddddddddddddddd ||
        vkSamp == VK_NULL_HANDLE || vkSamp == (VkSampler)0xdddddddddddddddd) {
        HE_CORE_ERROR("UpdateDescriptorSet: 非法句柄! binding={} imgView={:#018x} sampler={:#018x} texPtr={} sampPtr={}",
            binding, (u64)imgView, (u64)vkSamp, (void*)texture, (void*)sampler);
        return;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView   = imgView;
    imageInfo.sampler     = vkSamp;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = ToVkDescType(type);
    write.pImageInfo      = &imageInfo;

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

// ============================================================
// UpdateDescriptorSetWithImageView — StorageImage 绑定
// ============================================================
void VulkanDevice::UpdateDescriptorSetWithImageView(DescriptorSetHandle setHandle, u32 binding,
                                                      DescriptorType type, void* imageView) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];
    if (ds == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // StorageImage 使用 GENERAL 布局
    imageInfo.imageView   = static_cast<VkImageView>(imageView);
    imageInfo.sampler     = VK_NULL_HANDLE;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo      = &imageInfo;

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

// ============================================================
// UpdateDescriptorSet — AccelerationStructure 绑定
// ============================================================
void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle setHandle, u32 binding,
                                        DescriptorType type,
                                        IRHIAccelerationStructure* as) {
    if (setHandle == 0 || setHandle > m_DescSets.size()) return;
    VkDescriptorSet ds = m_DescSets[static_cast<usize>(setHandle - 1)];
    if (ds == VK_NULL_HANDLE) return;

    auto* vkAS = static_cast<VulkanAccelerationStructure*>(as);
    VkAccelerationStructureKHR vkHandle = vkAS->GetHandle();

    VkWriteDescriptorSetAccelerationStructureKHR asInfo{};
    asInfo.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures    = &vkHandle;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext           = &asInfo;
    write.dstSet          = ds;
    write.dstBinding      = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

// ============================================================
// Per-Mip ImageView 支持
// ============================================================

// 创建 mip ImageView 的通用辅助函数
static void* CreateMipViewInternal(VkDevice device, VulkanTexture* vkTex,
                                    u32 mipLevel, u32 arrayLayer, const char* label) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = vkTex->GetImage();
    viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format           = vkTex->GetVkFormat();
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = mipLevel;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = arrayLayer;
    viewInfo.subresourceRange.layerCount     = 1;
    VkImageView view = VK_NULL_HANDLE;
    VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &view);
    HE_ASSERT_MSG(result == VK_SUCCESS, label);
    return reinterpret_cast<void*>(view);
}

void* VulkanDevice::CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel) {
    return CreateMipViewInternal(m_Device, static_cast<VulkanTexture*>(texture),
                                  mipLevel, 0, "Failed to create mip storage image view");
}
void* VulkanDevice::CreateTextureMipStorageView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) {
    return CreateMipViewInternal(m_Device, static_cast<VulkanTexture*>(texture),
                                  mipLevel, arrayLayer, "Failed to create mip storage image view");
}

void* VulkanDevice::CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel) {
    return CreateMipViewInternal(m_Device, static_cast<VulkanTexture*>(texture),
                                  mipLevel, 0, "Failed to create mip sampled image view");
}
void* VulkanDevice::CreateTextureMipSampledView(IRHITexture* texture, u32 mipLevel, u32 arrayLayer) {
    return CreateMipViewInternal(m_Device, static_cast<VulkanTexture*>(texture),
                                  mipLevel, arrayLayer, "Failed to create mip sampled image view");
}

void VulkanDevice::DestroyTextureMipView(void* view) {
    if (view) {
        vkDestroyImageView(m_Device, reinterpret_cast<VkImageView>(view), nullptr);
    }
}

void VulkanDevice::DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) {
    if (handle == 0 || handle > m_DescLayoutInfos.size()) return;
    VkDescriptorSetLayout layout = m_DescLayoutInfos[static_cast<usize>(handle - 1)].layout;
    vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
}

} // namespace he::rhi
