#include "RHI/RHI.h"
#include "Core/Log.h"
#include "Core/Assert.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstring>

// Vulkan 类型的完整定义（供 inline 方法使用）
#include "VulkanInternal.h"

namespace he::rhi {

static u32 FindMemoryType(VkPhysicalDevice physical, u32 typeFilter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    for (u32 i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    HE_ASSERT(false, "No suitable memory type found");
    return 0;
}

// BufferUsage 位掩码 → VkBufferUsageFlags 映射
static VkBufferUsageFlags ToVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (u32(usage) & u32(BufferUsage::Vertex))   flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Index))    flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Uniform))  flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Storage))  flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (u32(usage) & u32(BufferUsage::Indirect)) flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    return flags;
}

VulkanBuffer::VulkanBuffer(VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc)
    : m_Device(device), m_Size(desc.size)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = desc.size;
    bufferInfo.usage = ToVkBufferUsage(desc.usage);

    VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, &m_Buffer);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create buffer");

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, m_Buffer, &memReqs);

    VkMemoryAllocateFlagsInfo allocFlags{};
    allocFlags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    allocFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext           = &allocFlags;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physical, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    result = vkAllocateMemory(device, &allocInfo, nullptr, &m_Memory);
    HE_ASSERT(result == VK_SUCCESS, "Failed to allocate buffer memory");

    vkBindBufferMemory(device, m_Buffer, m_Memory, 0);

    // Device address
    VkBufferDeviceAddressInfo addrInfo{};
    addrInfo.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addrInfo.buffer = m_Buffer;
    m_DeviceAddress = vkGetBufferDeviceAddress(device, &addrInfo);

    // 持久映射：构造时映射一次，后续 Map() 直接返回指针（Phase 1 多线程渲染）
    vkMapMemory(device, m_Memory, 0, m_Size, 0, &m_MappedPtr);
    m_IsMapped = true;

    // Upload initial data（直接写入持久映射指针）
    if (desc.initialData) {
        std::memcpy(m_MappedPtr, desc.initialData, desc.size);
    }
}

VulkanBuffer::~VulkanBuffer() {
    if (m_IsMapped) {
        vkUnmapMemory(m_Device, m_Memory);
        m_IsMapped = false;
    }
    vkDestroyBuffer(m_Device, m_Buffer, nullptr);
    vkFreeMemory(m_Device, m_Memory, nullptr);
}

void* VulkanBuffer::Map() {
    // 持久映射：直接返回已映射的指针，无需每次调用 vkMapMemory（Phase 1）
    return m_MappedPtr;
}

void VulkanBuffer::Unmap() {
    // 持久映射：Unmap 变为 no-op，缓冲区在整个生命周期保持映射状态（Phase 1）
}

// ============================================================
// VulkanPipelineState 析构（类定义在 VulkanInternal.h）
// ============================================================
VulkanPipelineState::~VulkanPipelineState() {
    vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
    vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
    vkDestroyRenderPass(m_Device, m_RenderPass, nullptr);
}

// ============================================================
// VulkanTexture / VulkanSampler 实现（类定义在 VulkanInternal.h）
// ============================================================

// ============================================================
// 格式转换辅助函数
// ============================================================

// he::rhi::Format → VkFormat 映射
static VkFormat ToVkFormat(Format fmt) {
    switch (fmt) {
        // 8-bit 颜色
        case Format::R8_UNORM:       return VK_FORMAT_R8_UNORM;
        case Format::R8_SRGB:        return VK_FORMAT_R8_SRGB;
        case Format::RG8_UNORM:      return VK_FORMAT_R8G8_UNORM;
        case Format::RG8_SRGB:       return VK_FORMAT_R8G8_SRGB;
        case Format::RGBA8_UNORM:    return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB:     return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8_UNORM:    return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8_SRGB:     return VK_FORMAT_B8G8R8A8_SRGB;
        // 16-bit 浮点
        case Format::R16_FLOAT:      return VK_FORMAT_R16_SFLOAT;
        case Format::RG16_FLOAT:     return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGBA16_FLOAT:   return VK_FORMAT_R16G16B16A16_SFLOAT;
        // 32-bit 浮点
        case Format::R32_FLOAT:      return VK_FORMAT_R32_SFLOAT;
        case Format::RG32_FLOAT:     return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGBA32_FLOAT:   return VK_FORMAT_R32G32B32A32_SFLOAT;
        // 特殊
        case Format::R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        // 深度/模板
        case Format::D16_UNORM:           return VK_FORMAT_D16_UNORM;
        case Format::D32_FLOAT:           return VK_FORMAT_D32_SFLOAT;
        case Format::D24_UNORM_S8_UINT:   return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_FLOAT_S8_UINT:   return VK_FORMAT_D32_SFLOAT_S8_UINT;
        // BC 压缩
        case Format::BC1_UNORM:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Format::BC3_UNORM:      return VK_FORMAT_BC3_UNORM_BLOCK;
        case Format::BC4_UNORM:      return VK_FORMAT_BC4_UNORM_BLOCK;
        case Format::BC5_UNORM:      return VK_FORMAT_BC5_UNORM_BLOCK;
        case Format::BC7_UNORM:      return VK_FORMAT_BC7_UNORM_BLOCK;
        default:                     return VK_FORMAT_UNDEFINED;
    }
}

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

// CompareFunc → VkCompareOp
static VkCompareOp ToVkCompareOp(CompareFunc func) {
    switch (func) {
        case CompareFunc::Never:        return VK_COMPARE_OP_NEVER;
        case CompareFunc::Less:         return VK_COMPARE_OP_LESS;
        case CompareFunc::Equal:        return VK_COMPARE_OP_EQUAL;
        case CompareFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareFunc::Greater:      return VK_COMPARE_OP_GREATER;
        case CompareFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareFunc::Always:       return VK_COMPARE_OP_ALWAYS;
        default:                        return VK_COMPARE_OP_NEVER;
    }
}

// ============================================================
// VulkanTexture 实现
// ============================================================

VulkanTexture::VulkanTexture(VkDevice device, VkPhysicalDevice physical,
                             VkCommandPool cmdPool, VkQueue queue,
                             const TextureDesc& desc)
    : m_Device(device), m_Physical(physical)
    , m_Width(desc.width), m_Height(desc.height), m_Depth(desc.depth)
    , m_MipLevels(desc.mipLevels)
    , m_ArrayLayers(u32(desc.usage) & u32(TextureUsage::Cubemap) ? 6 : desc.arrayLayers)
    , m_Format(desc.format)
    , m_VkFormat(ToVkFormat(desc.format))
{
    // 1. 创建 VkImage
    bool isCubemap = u32(desc.usage) & u32(TextureUsage::Cubemap);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType     = (desc.depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format        = m_VkFormat;
    imageInfo.extent        = {m_Width, m_Height, m_Depth};
    imageInfo.mipLevels     = m_MipLevels;
    imageInfo.arrayLayers   = isCubemap ? 6 : m_ArrayLayers;  // Cubemap 固定 6 面
    imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage         = ToVkImageUsage(desc.usage);
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (isCubemap)
        imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    // 需要上传初始数据时添加 TransferDst 标志
    if (desc.initialData)
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult result = vkCreateImage(device, &imageInfo, nullptr, &m_Image);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create Vulkan image");

    // 2. 分配并绑定内存
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, m_Image, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physical, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    result = vkAllocateMemory(device, &allocInfo, nullptr, &m_Memory);
    HE_ASSERT(result == VK_SUCCESS, "Failed to allocate texture memory");

    vkBindImageMemory(device, m_Image, m_Memory, 0);

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

    result = vkCreateImageView(device, &viewInfo, nullptr, &m_ImageView);
    HE_ASSERT(result == VK_SUCCESS, "Failed to create Vulkan image view");

    // 5. 为 Cubemap 创建 6 个逐面 2D ImageView（用于离屏渲染到每个面）
    if (isCubemap) {
        m_FaceViews.resize(6);
        VkImageViewCreateInfo faceViewInfo{};
        faceViewInfo.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        faceViewInfo.image      = m_Image;
        faceViewInfo.viewType   = VK_IMAGE_VIEW_TYPE_2D;  // 单面 2D
        faceViewInfo.format     = m_VkFormat;
        faceViewInfo.subresourceRange.aspectMask     = ToVkAspectMask(desc.format);
        faceViewInfo.subresourceRange.baseMipLevel   = 0;
        faceViewInfo.subresourceRange.levelCount     = m_MipLevels;
        faceViewInfo.subresourceRange.baseArrayLayer = 0;
        faceViewInfo.subresourceRange.layerCount     = 1;

        for (u32 face = 0; face < 6; ++face) {
            faceViewInfo.subresourceRange.baseArrayLayer = face;
            result = vkCreateImageView(device, &faceViewInfo, nullptr, &m_FaceViews[face]);
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
    if (m_Image)     vkDestroyImage(m_Device, m_Image, nullptr);
    if (m_Memory)    vkFreeMemory(m_Device, m_Memory, nullptr);
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

    // 创建暂存缓冲区（host-visible）
    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size        = dataSize;
    stagingInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stagingBuffer;
    vkCreateBuffer(m_Device, &stagingInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements stagingMemReqs;
    vkGetBufferMemoryRequirements(m_Device, stagingBuffer, &stagingMemReqs);

    VkDeviceMemory stagingMemory;
    VkMemoryAllocateInfo stagingAlloc{};
    stagingAlloc.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAlloc.allocationSize = stagingMemReqs.size;
    stagingAlloc.memoryTypeIndex = FindMemoryType(m_Physical, stagingMemReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(m_Device, &stagingAlloc, nullptr, &stagingMemory);
    vkBindBufferMemory(m_Device, stagingBuffer, stagingMemory, 0);

    // 拷贝数据到暂存缓冲区
    void* mapped;
    vkMapMemory(m_Device, stagingMemory, 0, dataSize, 0, &mapped);
    std::memcpy(mapped, desc.initialData, dataSize);
    vkUnmapMemory(m_Device, stagingMemory);

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

    // 图像布局转换：UNDEFINED → TRANSFER_DST
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

    // 拷贝暂存缓冲 → 图像
    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset                    = 0;
    copyRegion.bufferRowLength                 = 0; // 紧密排列
    copyRegion.bufferImageHeight               = 0;
    copyRegion.imageSubresource.aspectMask     = ToVkAspectMask(desc.format);
    copyRegion.imageSubresource.mipLevel       = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount     = m_ArrayLayers;
    copyRegion.imageOffset                     = {0, 0, 0};
    copyRegion.imageExtent                     = {m_Width, m_Height, m_Depth};

    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_Image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

    // 图像布局转换：TRANSFER_DST → SHADER_READ_ONLY
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    // 提交并等待完成
    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmd;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    // 清理暂存资源
    vkFreeCommandBuffers(m_Device, cmdPool, 1, &cmd);
    vkDestroyBuffer(m_Device, stagingBuffer, nullptr);
    vkFreeMemory(m_Device, stagingMemory, nullptr);
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
    VkDevice device, VkPhysicalDevice physical, const BufferDesc& desc)
{
    return std::make_unique<VulkanBuffer>(device, physical, desc);
}

std::unique_ptr<IRHITexture> CreateVulkanTexture(
    VkDevice device, VkPhysicalDevice physical,
    VkCommandPool cmdPool, VkQueue queue,
    const TextureDesc& desc)
{
    return std::make_unique<VulkanTexture>(device, physical, cmdPool, queue, desc);
}

std::unique_ptr<IRHISampler> CreateVulkanSampler(
    VkDevice device, const SamplerDesc& desc)
{
    return std::make_unique<VulkanSampler>(device, desc);
}

std::unique_ptr<IRHIPipelineState> CreateVulkanPipeline(
    VkDevice device, const PipelineStateDesc& desc,
    const std::vector<VkDescriptorSetLayout>& descLayouts)
{
    // 1. Create shader modules
    auto createShader = [&](const ShaderBytecode* bc, VkShaderStageFlagBits stage) -> VkShaderModule {
        if (!bc || bc->spirv.empty()) return VK_NULL_HANDLE;

        VkShaderModuleCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bc->spirv.size() * sizeof(u32);
        info.pCode    = bc->spirv.data();

        VkShaderModule mod;
        vkCreateShaderModule(device, &info, nullptr, &mod);
        return mod;
    };

    VkShaderModule vert = createShader(desc.vertexShader,   VK_SHADER_STAGE_VERTEX_BIT);
    VkShaderModule frag = createShader(desc.pixelShader,    VK_SHADER_STAGE_FRAGMENT_BIT);

    // 2. Render pass（颜色附件 + 可选的深度附件）
    //    支持 depth-only 模式：colorAttachmentCount=0 时仅创建深度附件
    bool hasColor = (desc.colorAttachmentCount > 0);
    bool hasDepth = (desc.depthFormat != Format::Unknown);

    VkAttachmentDescription colorAttach{};
    if (hasColor) {
        colorAttach.format        = VK_FORMAT_B8G8R8A8_UNORM;
        colorAttach.samples       = VK_SAMPLE_COUNT_1_BIT;
        colorAttach.loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttach.finalLayout   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    VkAttachmentDescription depthAttach{};
    depthAttach.format         = hasDepth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
    depthAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttach.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE; // 阴影贴图需要 STORE
    depthAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttach.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; // 阴影贴图后续要采样

    // depth-only 模式下，深度附件在 index 0；否则在 index 1
    u32 depthAttachIdx = hasColor ? 1u : 0u;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{depthAttachIdx, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = hasColor ? 1u : 0u;
    subpass.pColorAttachments       = hasColor ? &colorRef : nullptr;
    subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

    // 构建附件数组：depth-only 时深度在 [0]，否则颜色在 [0]、深度在 [1]
    VkAttachmentDescription attachments[2];
    u32 attachmentCount = 0;
    u32 colorIdx = 0, depthInArrIdx = 0;
    if (hasColor) {
        attachments[attachmentCount++] = colorAttach;
        colorIdx = 0;
        depthInArrIdx = 1;
        if (hasDepth) attachments[attachmentCount++] = depthAttach;
    } else if (hasDepth) {
        attachments[attachmentCount++] = depthAttach;
        depthInArrIdx = 0;
    }

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask  = (hasColor ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT : 0u) |
                        (hasDepth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT : 0u);
    dep.dstAccessMask = (hasColor ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u) |
                        (hasDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0u);

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = attachmentCount;
    rpInfo.pAttachments    = attachments;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = (hasColor || hasDepth) ? 1u : 0u;
    rpInfo.pDependencies   = (hasColor || hasDepth) ? &dep : nullptr;

    VkRenderPass renderPass;
    vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);

    // 3. Vertex input
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = desc.vertexLayout.stride > 0 ? desc.vertexLayout.stride : 8;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // VertexFormat -> VkFormat 映射
    auto toVkVertexFormat = [](VertexFormat fmt) -> VkFormat {
        switch (fmt) {
            case VertexFormat::Float:   return VK_FORMAT_R32_SFLOAT;
            case VertexFormat::Float2:  return VK_FORMAT_R32G32_SFLOAT;
            case VertexFormat::Float3:  return VK_FORMAT_R32G32B32_SFLOAT;
            case VertexFormat::Float4:  return VK_FORMAT_R32G32B32A32_SFLOAT;
            case VertexFormat::UByte4_Norm: return VK_FORMAT_R8G8B8A8_UNORM;
            case VertexFormat::Byte4_Norm:  return VK_FORMAT_R8G8B8A8_SNORM;
            case VertexFormat::UInt:    return VK_FORMAT_R32_UINT;
            case VertexFormat::UInt2:   return VK_FORMAT_R32G32_UINT;
            case VertexFormat::UInt4:   return VK_FORMAT_R32G32B32A32_UINT;
            default:                    return VK_FORMAT_R32G32B32_SFLOAT;
        }
    };

    // 根据 desc.vertexLayout 构建 Vulkan 属性列表
    std::vector<VkVertexInputAttributeDescription> vkAttrs;
    if (desc.vertexLayout.attributes.empty()) {
        // 回退：未指定属性时，根据 stride 推导默认格式
        VkVertexInputAttributeDescription defaultAttr{};
        defaultAttr.location = 0;
        defaultAttr.binding  = 0;
        defaultAttr.offset   = 0;
        if (desc.vertexLayout.stride >= 32) defaultAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
        else if (desc.vertexLayout.stride >= 12) defaultAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
        else defaultAttr.format = VK_FORMAT_R32G32_SFLOAT;  // stride=8: vec2
        vkAttrs.push_back(defaultAttr);
    } else {
        for (auto& attr : desc.vertexLayout.attributes) {
            VkVertexInputAttributeDescription va{};
            va.location = attr.location;
            va.binding  = attr.binding;
            va.format   = toVkVertexFormat(attr.format);
            va.offset   = attr.offset;
            vkAttrs.push_back(va);
        }
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<u32>(vkAttrs.size());
    vertexInput.pVertexAttributeDescriptions    = vkAttrs.data();

    // 4. Input assembly
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 5. Dynamic viewport + scissor
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode  = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 6. Depth stencil state
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = ToVkCompareOp(desc.depthCompare);
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable     = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttach{};
    blendAttach.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttach;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    // 构建 push constant ranges（直接使用 stageMask 位掩码）
    std::vector<VkPushConstantRange> vkPushRanges;
    for (auto& pcRange : desc.pushConstantRanges) {
        VkPushConstantRange vkRange{};
        vkRange.stageFlags = pcRange.stageMask;  // 直接使用 Vulkan 兼容的位掩码
        vkRange.offset     = pcRange.offset;
        vkRange.size       = pcRange.size;
        vkPushRanges.push_back(vkRange);
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = static_cast<u32>(descLayouts.size());
    layoutInfo.pSetLayouts            = descLayouts.empty() ? nullptr : descLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<u32>(vkPushRanges.size());
    layoutInfo.pPushConstantRanges    = vkPushRanges.empty() ? nullptr : vkPushRanges.data();
    VkPipelineLayout pipelineLayout;
    vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

    // 6. Shader stages
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // 7. Create pipeline
    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.stageCount          = 2;
    pipeInfo.pStages             = stages;
    pipeInfo.pVertexInputState   = &vertexInput;
    pipeInfo.pInputAssemblyState = &inputAssembly;
    pipeInfo.pViewportState      = &viewportState;
    pipeInfo.pRasterizationState = &rasterizer;
    pipeInfo.pMultisampleState   = &ms;
    pipeInfo.pDepthStencilState  = &depthStencil;
    pipeInfo.pColorBlendState    = &colorBlend;
    pipeInfo.pDynamicState       = &dynState;
    pipeInfo.layout              = pipelineLayout;
    pipeInfo.renderPass          = renderPass;
    pipeInfo.subpass             = 0;

    VkPipeline pipeline;
    vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline);

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);

    HE_CORE_INFO("Vulkan pipeline created");
    return std::make_unique<VulkanPipelineState>(device, pipeline, pipelineLayout, renderPass);
}

} // namespace he::rhi
