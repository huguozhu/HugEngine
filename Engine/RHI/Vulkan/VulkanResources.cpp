// ============================================================
// VulkanResources.cpp — Vulkan 资源实现
// 负责 Buffer、Texture、Sampler 的创建/销毁/映射
// ============================================================

// VMA 实现：在且仅在此编译单元中编译 VMA 单头文件的函数体
#define VMA_IMPLEMENTATION
#include "RHI/RHI.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstring>

// Vulkan 类型的完整定义（供 inline 方法使用）
#include "VulkanResources.h"
#include "VulkanConverters.h"

namespace he::rhi {

// BufferUsage 位掩码 → VkBufferUsageFlags 映射
static VkBufferUsageFlags ToVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (u32(usage) & u32(BufferUsage::Vertex))   flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Index))    flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Uniform))  flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Storage))  flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Indirect)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::AccelerationStruct))
        flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    return flags;
}

// ============================================================
// VulkanBuffer 实现
// ============================================================

VulkanBuffer::VulkanBuffer(VmaAllocator allocator, const BufferDesc& desc)
    : m_Allocator(allocator), m_Size(desc.size)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = desc.size;
    bufferInfo.usage = ToVkBufferUsage(desc.usage);

    // VMA 一键创建 Buffer + 分配内存（持久映射，HOST_VISIBLE）
    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                          | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocCreateInfo,
                                       &m_Buffer, &m_Allocation, nullptr);
    HE_ASSERT(result == VK_SUCCESS, "VMA: Failed to create buffer");

    // 获取持久映射指针（VMA_ALLOCATION_CREATE_MAPPED_BIT 保证 pMappedData 非空）
    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(allocator, m_Allocation, &allocInfo);
    m_MappedPtr = allocInfo.pMappedData;
    m_IsMapped = (m_MappedPtr != nullptr);

    // 获取 VkDevice（从 VMA 分配器）
    VmaAllocatorInfo allocatorInfo;
    vmaGetAllocatorInfo(allocator, &allocatorInfo);
    m_Device = allocatorInfo.device;

    // 检查内存是否 host-coherent（通过 VMA allocation 属性查询）
    {
        VkMemoryPropertyFlags memFlags = 0;
        vmaGetAllocationMemoryProperties(allocator, m_Allocation, &memFlags);
        m_IsCoherent = (memFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }

    // Device address
    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_Buffer;
    m_DeviceAddress = vkGetBufferDeviceAddress(m_Device, &addrInfo);

    // 上传初始数据（直接写入持久映射指针，非 coherent 需 flush）
    if (desc.initialData && m_MappedPtr) {
        std::memcpy(m_MappedPtr, desc.initialData, desc.size);
        if (!m_IsCoherent) {
            VkMappedMemoryRange range{};
            range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = allocInfo.deviceMemory;
            range.offset = allocInfo.offset;
            range.size   = desc.size;
            vkFlushMappedMemoryRanges(m_Device, 1, &range);
        }
    }
}

VulkanBuffer::~VulkanBuffer() {
    if (m_Buffer) {
        vmaDestroyBuffer(m_Allocator, m_Buffer, m_Allocation);
    }
}

void* VulkanBuffer::Map() {
    // GPU→CPU 回读：始终 invalidate（coherent 内存上为 no-op）
    if (m_IsMapped) {
        VmaAllocationInfo allocInfo;
        vmaGetAllocationInfo(m_Allocator, m_Allocation, &allocInfo);
        VkMappedMemoryRange range{};
        range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = allocInfo.deviceMemory;
        range.offset = allocInfo.offset;
        range.size   = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(m_Device, 1, &range);
    }
    return m_MappedPtr;
}

void VulkanBuffer::Unmap() {
    // CPU→GPU 写入：始终 flush（coherent 内存上为 no-op）
    if (m_IsMapped) {
        VmaAllocationInfo allocInfo;
        vmaGetAllocationInfo(m_Allocator, m_Allocation, &allocInfo);
        VkMappedMemoryRange range{};
        range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = allocInfo.deviceMemory;
        range.offset = allocInfo.offset;
        range.size   = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(m_Device, 1, &range);
    }
}

// ============================================================
// 格式转换辅助函数（部分被 VulkanPipeline.cpp 共享）
// ============================================================

// 根据格式获取每像素字节数（压缩格式返回块大小）
static u32 GetFormatByteSize(Format fmt) {
    switch (fmt) {
        case Format::R8_UNORM:       case Format::R8_SRGB:        return 1;
        case Format::RG8_UNORM:      case Format::RG8_SRGB:       return 2;
        case Format::RGBA8_UNORM:    case Format::RGBA8_SRGB:
        case Format::BGRA8_UNORM:    case Format::BGRA8_SRGB:      return 4;
        case Format::R16_FLOAT:                                    return 2;
        case Format::RG16_FLOAT:                                   return 4;
        case Format::RGBA16_FLOAT:                                 return 8;
        case Format::R32_FLOAT:                                    return 4;
        case Format::RG32_FLOAT:                                   return 8;
        case Format::RGBA32_FLOAT:                                 return 16;
        case Format::R11G11B10_FLOAT:                              return 4;
        case Format::D16_UNORM:                                    return 2;
        case Format::D32_FLOAT:                                    return 4;
        case Format::D24_UNORM_S8_UINT:                            return 4;
        case Format::D32_FLOAT_S8_UINT:                            return 8;
        // BC 压缩 — 返回块字节数
        case Format::BC1_UNORM: case Format::BC4_UNORM:            return 8;
        case Format::BC3_UNORM: case Format::BC5_UNORM:
        case Format::BC7_UNORM:                                    return 16;
        default:                                                   return 4;
    }
}

// 检查格式是否为压缩格式
static bool IsCompressedFormat(Format fmt) {
    switch (fmt) {
        case Format::BC1_UNORM: case Format::BC3_UNORM:
        case Format::BC4_UNORM: case Format::BC5_UNORM:
        case Format::BC7_UNORM:
            return true;
        default:
            return false;
    }
}

// TextureUsage → VkImageUsageFlags
static VkImageUsageFlags ToVkImageUsage(TextureUsage usage) {
    VkImageUsageFlags flags = 0;
    if (u32(usage) & u32(TextureUsage::ShaderResource))  flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (u32(usage) & u32(TextureUsage::RenderTarget))    flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (u32(usage) & u32(TextureUsage::DepthStencil))    flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (u32(usage) & u32(TextureUsage::UnorderedAccess)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (u32(usage) & u32(TextureUsage::TransferSrc))     flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (u32(usage) & u32(TextureUsage::TransferDst))     flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}

// 格式 → 图像宽高比掩码
static VkImageAspectFlags ToVkAspectMask(Format fmt) {
    switch (fmt) {
        case Format::D16_UNORM:
        case Format::D32_FLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case Format::D24_UNORM_S8_UINT:
        case Format::D32_FLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

// TextureDesc → VkImageViewType
static VkImageViewType ToVkImageViewType(const TextureDesc& desc) {
    if (desc.depth > 1)
        return VK_IMAGE_VIEW_TYPE_3D;
    bool isCubemap = u32(desc.usage) & u32(TextureUsage::Cubemap);
    if (isCubemap)
        return VK_IMAGE_VIEW_TYPE_CUBE;
    if (desc.arrayLayers > 1)
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    return VK_IMAGE_VIEW_TYPE_2D;
}

// FilterMode → VkFilter
static VkFilter ToVkFilter(FilterMode mode) {
    return mode == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

// AddressMode → VkSamplerAddressMode
static VkSamplerAddressMode ToVkAddressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:                          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// ============================================================
// VulkanTexture 实现
// ============================================================

VulkanTexture::VulkanTexture(VmaAllocator allocator, VkCommandPool cmdPool, VkQueue queue,
                             const TextureDesc& desc)
    : m_Allocator(allocator)
    , m_Width(desc.width), m_Height(desc.height), m_Depth(desc.depth)
    , m_MipLevels(desc.mipLevels)
    , m_ArrayLayers(u32(desc.usage) & u32(TextureUsage::Cubemap) ? 6 : desc.arrayLayers)
    , m_SampleCount(desc.sampleCount)
    , m_Format(desc.format)
    , m_VkFormat(ToVkFormat(desc.format))
{
    // 从 VMA 分配器获取 VkDevice（供 vkCreateImage / vkDestroyImageView 使用）
    VmaAllocatorInfo allocInfo;
    vmaGetAllocatorInfo(allocator, &allocInfo);
    m_Device = allocInfo.device;

    // 1. VMA 一步创建 VkImage + 分配 + 绑定（VMA_MEMORY_USAGE_AUTO 需资源创建上下文）
    bool isCubemap = u32(desc.usage) & u32(TextureUsage::Cubemap);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = (desc.depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format        = m_VkFormat;
    imageInfo.extent        = {m_Width, m_Height, m_Depth};
    imageInfo.mipLevels     = m_MipLevels;
    imageInfo.arrayLayers   = isCubemap ? 6 : m_ArrayLayers;
    // 将 u32 采样数转换为 VkSampleCountFlagBits
    VkSampleCountFlagBits vkSamples = VK_SAMPLE_COUNT_1_BIT;
    switch (desc.sampleCount) {
        case 2:  vkSamples = VK_SAMPLE_COUNT_2_BIT;  break;
        case 4:  vkSamples = VK_SAMPLE_COUNT_4_BIT;  break;
        case 8:  vkSamples = VK_SAMPLE_COUNT_8_BIT;  break;
        case 16: vkSamples = VK_SAMPLE_COUNT_16_BIT; break;
        case 32: vkSamples = VK_SAMPLE_COUNT_32_BIT; break;
        case 64: vkSamples = VK_SAMPLE_COUNT_64_BIT; break;
        default: vkSamples = VK_SAMPLE_COUNT_1_BIT;  break;
    }
    imageInfo.samples       = vkSamples;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = ToVkImageUsage(desc.usage);
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (isCubemap)
        imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    // 需要上传初始数据时添加 TransferDst 标志
    if (desc.initialData)
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkResult result = vmaCreateImage(allocator, &imageInfo, &allocCreateInfo,
                                      &m_Image, &m_Allocation, nullptr);
    HE_ASSERT(result == VK_SUCCESS, "VMA: Failed to create texture");

    // 3. 上传初始数据（如果有）
    if (desc.initialData)
        UploadInitialData(cmdPool, queue, desc);

    // 4. 创建 ImageView
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image            = m_Image;
    viewInfo.viewType         = ToVkImageViewType(desc);
    viewInfo.format           = m_VkFormat;
    viewInfo.subresourceRange.aspectMask     = ToVkAspectMask(desc.format);
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = m_MipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = m_ArrayLayers;

    result = vkCreateImageView(m_Device, &viewInfo, nullptr, &m_ImageView);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create Vulkan image view");

    // 5. 为 Cubemap 创建 6 个逐面 2D ImageView（用于离屏渲染到每个面）
    if (isCubemap) {
        m_FaceViews.resize(kCubemapFaceCount);
        VkImageViewCreateInfo faceViewInfo{};
        faceViewInfo.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        faceViewInfo.image      = m_Image;
        faceViewInfo.viewType   = VK_IMAGE_VIEW_TYPE_2D;
        faceViewInfo.format     = m_VkFormat;
        faceViewInfo.subresourceRange.aspectMask     = ToVkAspectMask(desc.format);
        faceViewInfo.subresourceRange.baseMipLevel   = 0;
        faceViewInfo.subresourceRange.levelCount     = m_MipLevels;
        faceViewInfo.subresourceRange.baseArrayLayer = 0;
        faceViewInfo.subresourceRange.layerCount     = 1;

        for (u32 face = 0; face < kCubemapFaceCount; ++face) {
            faceViewInfo.subresourceRange.baseArrayLayer = face;
            result = vkCreateImageView(m_Device, &faceViewInfo, nullptr, &m_FaceViews[face]);
            HE_ASSERT(result == VK_SUCCESS, "Failed to create cubemap face image view");
        }
    }

    HE_CORE_INFO("Vulkan texture created: {}x{} [{}]{}", m_Width, m_Height,
                 m_Format == Format::RGBA8_UNORM ? "RGBA8" : "other",
                 isCubemap ? " cubemap" : "");
}

VulkanTexture::~VulkanTexture() {
    for (auto& fv : m_FaceViews)
        if (fv) vkDestroyImageView(m_Device, fv, nullptr);
    m_FaceViews.clear();
    if (m_ImageView) vkDestroyImageView(m_Device, m_ImageView, nullptr);
    if (m_Image) {
        // vmaDestroyImage 自动处理 vkDestroyImage + vkFreeMemory
        vmaDestroyImage(m_Allocator, m_Image, m_Allocation);
    }
}

void VulkanTexture::UploadInitialData(VkCommandPool cmdPool, VkQueue queue,
                                      const TextureDesc& desc) {
    // 计算数据总大小
    usize dataSize;
    u32   blockW = desc.width;
    u32   blockH = desc.height;
    if (IsCompressedFormat(desc.format)) {
        u32 blockSize = GetFormatByteSize(desc.format);
        blockW = (desc.width  + 3) / 4;
        blockH = (desc.height + 3) / 4;
        dataSize = static_cast<usize>(blockW) * blockH * blockSize
                 * desc.depth * desc.arrayLayers;
    } else {
        dataSize = static_cast<usize>(desc.width) * desc.height
                 * desc.depth * desc.arrayLayers * GetFormatByteSize(desc.format);
    }

    // 创建暂存缓冲区（VMA 管理，CPU 可见）
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size        = dataSize;
    stagingInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                           | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    VkResult result = vmaCreateBuffer(m_Allocator, &stagingInfo, &stagingAllocInfo,
                                       &stagingBuffer, &stagingAlloc, nullptr);
    HE_ASSERT(result == VK_SUCCESS, "VMA: Failed to create staging buffer for texture upload");

    // 拷贝数据到暂存缓冲区（VMA 持久映射，无需手动 Map/Unmap）
    VmaAllocationInfo stagingAllocInfo2;
    vmaGetAllocationInfo(m_Allocator, stagingAlloc, &stagingAllocInfo2);
    std::memcpy(stagingAllocInfo2.pMappedData, desc.initialData, dataSize);

    // 分配一次性命令缓冲
    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = cmdPool;
    cmdAlloc.level       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_Device, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // 图像布局转换：UNDEFINED → TRANSFER_DST（所有 level）
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = m_Image;
    barrier.subresourceRange.aspectMask     = ToVkAspectMask(desc.format);
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = m_MipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = m_ArrayLayers;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // 拷贝暂存缓冲 → 图像（level 0）
    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset                    = 0;
    copyRegion.bufferRowLength                 = 0;
    copyRegion.bufferImageHeight               = 0;
    copyRegion.imageSubresource.aspectMask     = ToVkAspectMask(desc.format);
    copyRegion.imageSubresource.mipLevel       = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount     = m_ArrayLayers;
    copyRegion.imageOffset                     = {0, 0, 0};
    copyRegion.imageExtent                     = {m_Width, m_Height, m_Depth};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_Image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // --- Mipmap 生成（Copy 后逐级 blit，标准逐级 barrier）---
    if (m_MipLevels > 1) {
        VkImageMemoryBarrier mipBarrier{};
        mipBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mipBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mipBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mipBarrier.image               = m_Image;
        mipBarrier.subresourceRange.aspectMask     = ToVkAspectMask(desc.format);
        mipBarrier.subresourceRange.baseArrayLayer = 0;
        mipBarrier.subresourceRange.layerCount     = m_ArrayLayers;
        mipBarrier.subresourceRange.levelCount     = 1;

        i32 mipW = static_cast<i32>(m_Width);
        i32 mipH = static_cast<i32>(m_Height);

        for (u32 i = 1; i < m_MipLevels; ++i) {
            i32 nextW = mipW > 1 ? mipW / 2 : 1;
            i32 nextH = mipH > 1 ? mipH / 2 : 1;

            // level i-1: TRANSFER_DST → TRANSFER_SRC（作为 blit 源）
            mipBarrier.subresourceRange.baseMipLevel = i - 1;
            mipBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            mipBarrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            mipBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mipBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &mipBarrier);

            // Blit level i-1 → level i
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask     = ToVkAspectMask(desc.format);
            blit.srcSubresource.mipLevel       = i - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount     = m_ArrayLayers;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mipW, mipH, 1};
            blit.dstSubresource.aspectMask     = ToVkAspectMask(desc.format);
            blit.dstSubresource.mipLevel       = i;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount     = m_ArrayLayers;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {nextW, nextH, 1};

            vkCmdBlitImage(cmd,
                m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_LINEAR);

            // level i-1: TRANSFER_SRC → SHADER_READ_ONLY（完成使命）
            mipBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            mipBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mipBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            mipBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &mipBarrier);

            mipW = nextW;
            mipH = nextH;
        }

        // 最后一层 level: TRANSFER_DST → SHADER_READ_ONLY
        mipBarrier.subresourceRange.baseMipLevel = m_MipLevels - 1;
        mipBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mipBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        mipBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mipBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &mipBarrier);
    } else {
        // 单 mip level：TRANSFER_DST → SHADER_READ_ONLY
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.subresourceRange.levelCount = 1;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(cmd);

    // 提交并等待完成
    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // 清理暂存资源（VMA 一步完成 Buffer + Allocation 销毁）
    vkFreeCommandBuffers(m_Device, cmdPool, 1, &cmd);
    vmaDestroyBuffer(m_Allocator, stagingBuffer, stagingAlloc);
}

// ============================================================
// VulkanSampler 实现
// ============================================================

VulkanSampler::VulkanSampler(VkDevice device, const SamplerDesc& desc)
    : m_Device(device)
{
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = ToVkFilter(desc.magFilter);
    info.minFilter    = ToVkFilter(desc.minFilter);
    info.mipmapMode   = (desc.mipFilter == FilterMode::Nearest)
                        ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                        : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = ToVkAddressMode(desc.addressU);
    info.addressModeV = ToVkAddressMode(desc.addressV);
    info.addressModeW = ToVkAddressMode(desc.addressW);
    info.mipLodBias   = desc.mipLodBias;
    info.minLod       = desc.minLod;
    info.maxLod       = desc.maxLod;
    info.maxAnisotropy = static_cast<float>(desc.maxAnisotropy);
    if (desc.maxAnisotropy > 1)
        info.anisotropyEnable = VK_TRUE;

    // 深度比较采样器（用于阴影贴图）
    if (desc.enableCompare) {
        info.compareEnable = VK_TRUE;
        info.compareOp     = ToVkCompareOp(desc.compareFunc);
    }

    VkResult result = vkCreateSampler(device, &info, nullptr, &m_Sampler);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create Vulkan sampler");

    HE_CORE_INFO("Vulkan sampler created [{}]", desc.enableCompare ? "comparison" : "default");
}

VulkanSampler::~VulkanSampler() {
    if (m_Sampler) vkDestroySampler(m_Device, m_Sampler, nullptr);
}

// ============================================================
// 工厂函数 — 由 VulkanDevice 调用
// ============================================================

std::unique_ptr<IRHIBuffer> CreateVulkanBuffer(
    VmaAllocator allocator, const BufferDesc& desc)
{
    return std::make_unique<VulkanBuffer>(allocator, desc);
}

std::unique_ptr<IRHITexture> CreateVulkanTexture(
    VmaAllocator allocator, VkCommandPool cmdPool, VkQueue queue,
    const TextureDesc& desc)
{
    return std::make_unique<VulkanTexture>(allocator, cmdPool, queue, desc);
}

std::unique_ptr<IRHISampler> CreateVulkanSampler(
    VkDevice device, const SamplerDesc& desc)
{
    return std::make_unique<VulkanSampler>(device, desc);
}

} // namespace he::rhi
