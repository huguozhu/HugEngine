// ============================================================================
//  D3D11Resources.h —— D3D11 的资源与管线对象
//
//  Buffer / Texture / Sampler 在 M2.3 实现；PipelineState 在 M2.4，QueryPool 在 M3.7
//  （目前仍是占位对象，保证设备的创建接口都返回非空对象）。
// ============================================================================
#pragma once

#if defined(_WIN32)

#include "D3D11Common.h"
#include "RHI/Shader.h"
#include "RHI/QueryPool.h"
#include <unordered_map>
#include <vector>

namespace he::rhi::d3d11 {

class D3D11Device;

// ---------------------------------------------------------------------------
//  Buffer
//
//  【CPU 可读写的语义（规划 3.10）】Vulkan 后端的缓冲全部是主机可见、持久映射的一致内存，
//  引擎因此对任何缓冲都可能 Map 读写，且能读到 GPU 写入的结果。D3D11 的 DEFAULT 缓冲不能
//  Map、DYNAMIC 缓冲不能作 UAV，所以这里用「GPU 缓冲 + CPU 影子内存」模拟：
//    · Map() 返回影子内存。若该缓冲自上次同步以来可能被 GPU 写过（作过 UAV / 拷贝目标），
//      先同步回读一次（会等 GPU，正确优先，性能问题记录不修）；
//    · Unmap() 把影子内存整块上传到 GPU 缓冲；
//    · Map 之后一直不 Unmap 的「持久映射」缓冲由设备跟踪：每次绘制 / 派发前若影子内存
//      相对上次同步有改动就上传，每次提交时若被 GPU 写过就回读（见 D3D11Device::*OpenMaps）。
//  影子内存在第一次 Map 时才分配（从不 Map 的顶点 / 索引缓冲不占双份内存）。
//
//  【结构化缓冲步长（规划 3.11）】D3D11 必须在创建时给出 StructureByteStride。BufferDesc
//  填了 structureStride 就直接按结构化缓冲创建；否则先按可建原始视图的缓冲创建，第一次以
//  结构化视图绑定时按着色器反射出的步长重建（内容经影子内存保留）。
// ---------------------------------------------------------------------------
class D3D11Buffer final : public IRHIBuffer {
public:
    ~D3D11Buffer() override;

    bool Initialize(D3D11Device* device, const BufferDesc& desc);

    usize GetSize() const override { return m_Size; }
    void* Map() override;
    void  Unmap() override;
    u64   GetDeviceAddress() const override { return 0; }   // D3D11 没有缓冲 GPU 地址

    /// 顶点 / 索引 / 间接参数用的原生缓冲
    ID3D11Buffer* GetBuffer() const { return m_Buffer.Get(); }
    /// 常量缓冲：用途只有 Uniform 时就是主缓冲；与其它用途混用时是一份同步更新的镜像
    ID3D11Buffer* GetConstantBuffer();
    /// 创建时给的顶点步长（0 表示未给）
    u32 GetStride() const { return m_Stride; }

    /// 着色器视图的种类（来自 DXBC 反射）
    enum class ViewKind : u8 { Structured, Raw, Typed };
    /// 取（必要时创建）SRV / UAV。structured 时 strideOrFormat 为元素字节数，typed 时为 DXGI_FORMAT。
    /// 与缓冲的创建方式冲突（如顶点缓冲要结构化视图）时打印一次错误并返回空
    ID3D11ShaderResourceView*  GetSrv(ViewKind kind, u32 strideOrFormat);
    ID3D11UnorderedAccessView* GetUav(ViewKind kind, u32 strideOrFormat);

    /// GPU 可能写了它（作为 UAV 绑定 / 拷贝目标）：下次 CPU 读取前必须回读
    void MarkGpuWritten() { m_GpuWritten = true; }
    /// 持久映射缓冲：影子内存相对上次同步有改动就上传（绘制 / 派发前由设备调用）
    void FlushPersistentWrites();
    /// 持久映射缓冲：若被 GPU 写过就回读到影子内存（提交时由设备调用）
    void RefreshPersistentReads();
    bool IsMapped() const { return m_MapCount > 0; }

private:
    /// 按当前的结构化步长（0 = 非结构化）创建 GPU 缓冲，initial 非空时作初始数据
    bool CreateGpuBuffer(u32 structStride, const void* initial);
    /// 确保影子内存存在且内容是最新的（第一次 Map 或 GPU 写过之后要回读）
    void EnsureShadowCurrent();
    /// GPU 缓冲 → 影子内存（同步等待 GPU）
    void ReadbackToShadow();
    /// 影子内存 → GPU 缓冲（以及常量缓冲镜像）
    void UploadShadow();
    /// 视图缓存的键：种类 + 步长 / 格式
    static u64 ViewKey(ViewKind kind, u32 v) { return (u64(kind) << 32) | v; }

    D3D11Device*         m_Device = nullptr;
    ComPtr<ID3D11Buffer> m_Buffer;
    ComPtr<ID3D11Buffer> m_ConstantMirror;   // Uniform 与其它用途混用时的常量缓冲镜像
    ComPtr<ID3D11Buffer> m_Staging;          // 回读用（懒创建）
    std::unordered_map<u64, ComPtr<ID3D11ShaderResourceView>>  m_Srvs;
    std::unordered_map<u64, ComPtr<ID3D11UnorderedAccessView>> m_Uavs;
    std::vector<u8>      m_Shadow;           // CPU 影子内存（第一次 Map 时分配）
    std::vector<u8>      m_Synced;           // 持久映射时：上次与 GPU 同步时的内容（判断 CPU 是否改过）
    BufferUsage          m_Usage = BufferUsage::None;
    usize                m_Size = 0;         // 引擎请求的大小
    u32                  m_AllocSize = 0;    // 实际分配大小（按 16 字节 / 结构步长向上取整）
    u32                  m_Stride = 0;
    u32                  m_StructStride = 0; // 当前 GPU 缓冲的结构化步长（0 = 非结构化）
    u32                  m_MapCount = 0;
    bool                 m_GpuWritten = false;
    bool                 m_ConstantOnly = false;   // 用途只有 Uniform：主缓冲就是常量缓冲
    bool                 m_ReportedConflict = false;
};

// ---------------------------------------------------------------------------
//  Texture
//
//  【视图句柄语义】引擎把 GetNativeHandle() 返回的 void* 当「图像视图」传给
//  BeginOffscreenPass（渲染目标 / 深度附件）与 UpdateDescriptorSetWithImageView（采样 / 存储）。
//  这里返回 D3D11 视图对象的指针：渲染目标返回 RTV，深度返回 DSV，其余返回整资源 SRV；
//  每个指针都在设备的视图登记表里登记了「属于哪张纹理的哪个子资源」，命令列表与描述符集
//  据此查回纹理，再按需要现建 SRV / UAV（见 D3D11Device::FindView）。
// ---------------------------------------------------------------------------
class D3D11Texture final : public IRHITexture {
public:
    ~D3D11Texture() override;

    bool Initialize(D3D11Device* device, const TextureDesc& desc);

    u32    GetWidth()  const override { return m_Width; }
    u32    GetHeight() const override { return m_Height; }
    u32    GetDepth()  const override { return m_Depth; }
    u32    GetMipLevels()   const override { return m_MipLevels; }
    u32    GetArrayLayers() const override { return m_ArrayLayers; }
    Format GetFormat() const override { return m_Format; }
    void*  GetNativeHandle() const override;
    /// 逐面 / 逐层视图（mip 0）：渲染目标返回该层 RTV，深度返回该层 DSV
    void*  GetNativeHandle(u32 index) const override;

    ID3D11Resource* GetResource() const { return m_Resource.Get(); }
    bool IsDepth() const { return m_IsDepth; }
    bool IsCube()  const { return m_IsCube; }
    bool Is3D()    const { return m_Depth > 1; }
    u32  GetSampleCount() const { return m_SampleCount; }
    TextureUsage GetUsage() const { return m_Usage; }

    /// 子资源视图（mip = 起始 mip；single = 只含这一个 mip / 层，否则为全部 mip / 层）
    ID3D11ShaderResourceView*  GetSrv(u32 mip = 0, u32 layer = 0, bool single = false);
    ID3D11UnorderedAccessView* GetUav(u32 mip = 0, u32 layer = 0, bool single = false);
    ID3D11RenderTargetView*    GetRtv(u32 mip = 0, u32 layer = 0);
    ID3D11DepthStencilView*    GetDsv(u32 mip = 0, u32 layer = 0, bool readOnly = false);

private:
    /// 上传 TextureDesc::initialData：各数组层的 mip 0 依次紧密排列（与 Vulkan / D3D12 后端一致）
    void UploadInitialData(const TextureDesc& desc);
    static u64 ViewKey(u32 mip, u32 layer, bool single, bool extra = false) {
        return (u64(mip) << 33) | (u64(layer) << 2) | (single ? 2u : 0u) | (extra ? 1u : 0u);
    }

    D3D11Device*           m_Device = nullptr;
    ComPtr<ID3D11Resource> m_Resource;
    DXGI_FORMAT m_SrvFormat = DXGI_FORMAT_UNKNOWN, m_RtvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_DsvFormat = DXGI_FORMAT_UNKNOWN, m_UavFormat = DXGI_FORMAT_UNKNOWN;
    std::unordered_map<u64, ComPtr<ID3D11ShaderResourceView>>  m_Srvs;
    std::unordered_map<u64, ComPtr<ID3D11UnorderedAccessView>> m_Uavs;
    std::unordered_map<u64, ComPtr<ID3D11RenderTargetView>>    m_Rtvs;
    std::unordered_map<u64, ComPtr<ID3D11DepthStencilView>>    m_Dsvs;
    std::vector<void*> m_FaceViews;   // GetNativeHandle(index) 返回的逐层视图
    void*  m_DefaultView = nullptr;   // GetNativeHandle() 返回的视图
    u32    m_Width = 1, m_Height = 1, m_Depth = 1;
    u32    m_MipLevels = 1, m_ArrayLayers = 1, m_SampleCount = 1;
    Format m_Format = Format::RGBA8_UNORM;
    TextureUsage m_Usage = TextureUsage::None;
    bool   m_IsDepth = false;
    bool   m_IsCube  = false;
};

// ---------------------------------------------------------------------------
//  Sampler
// ---------------------------------------------------------------------------
class D3D11Sampler final : public IRHISampler {
public:
    bool Initialize(D3D11Device* device, const SamplerDesc& desc);
    ID3D11SamplerState* GetState() const { return m_State.Get(); }
private:
    ComPtr<ID3D11SamplerState> m_State;
};

// ---------------------------------------------------------------------------
//  PipelineState
//
//  D3D11 没有 PSO 对象：这里持有着色器对象 + 输入布局 + 光栅 / 深度模板 / 混合状态对象
//  （相同描述的状态对象由 D3D11 运行时自动复用，不必再自建缓存）。
//
//  【绑定槽位】每个阶段记录着色器**实际用到**的资源（DXBC 反射）及其描述符集位置
//  （ShaderToDxil 附在 DXBC 之后的绑定表，见 RHI/DxbcBindTable.h），命令列表在绘制 / 派发前
//  据此把描述符集里的资源绑到 D3D11 槽位。
//
//  【不可用的着色器】DXBC 编译失败的着色器只有 1 字节占位（见 cmake/CompileDxbc.cmake）。
//  这时仍返回一个 PSO 对象（引擎不检查空指针），但标记为不可用：用它的绘制 / 派发被跳过，
//  并打印一次「该 pass 在 D3D11 下不可用」。
// ---------------------------------------------------------------------------
class D3D11PipelineState final : public IRHIPipelineState {
public:
    /// 一个着色器资源绑定：描述符集位置 → D3D11 槽位，以及视图需要的形态
    struct Binding {
        u32  set = 0, binding = 0;     // 描述符集位置（推送常量为 0, 0）
        u32  type = 0;                 // dxbc::DxbcBindType
        u32  slot = 0, count = 1;      // D3D11 寄存器槽位与数组长度
        bool texture = false;          // true：纹理视图；false：缓冲视图（结构化 / 原始）
        bool rawBuffer = false;        // 缓冲视图是 ByteAddressBuffer
        u32  stride = 0;               // 结构化缓冲元素字节数（来自反射）
        D3D_SRV_DIMENSION dimension = D3D_SRV_DIMENSION_UNKNOWN;   // 纹理视图维度（来自反射）
    };
    enum Stage : u32 { VS = 0, PS = 1, CS = 2, StageCount = 3 };

    bool Initialize(D3D11Device* device, const PipelineStateDesc& desc);

    bool IsValid()   const { return m_Valid; }
    bool IsCompute() const { return m_IsCompute; }
    const char* GetName() const { return m_Name.c_str(); }
    /// 不可用的 PSO 第一次被使用时打印一次说明，之后静默跳过
    void ReportUnusableOnce();

    ID3D11VertexShader*      GetVS() const { return m_VS.Get(); }
    ID3D11PixelShader*       GetPS() const { return m_PS.Get(); }
    ID3D11ComputeShader*     GetCS() const { return m_CS.Get(); }
    ID3D11InputLayout*       GetInputLayout() const { return m_InputLayout.Get(); }
    ID3D11RasterizerState*   GetRasterizerState() const { return m_Rasterizer.Get(); }
    ID3D11DepthStencilState* GetDepthStencilState() const { return m_DepthStencil.Get(); }
    ID3D11BlendState*        GetBlendState() const { return m_Blend.Get(); }
    D3D11_PRIMITIVE_TOPOLOGY GetTopology() const { return m_Topology; }
    const std::vector<Binding>& GetBindings(Stage s) const { return m_Bindings[s]; }

    u32    GetVertexStride() const { return m_VertexStride; }
    LoadOp GetColorLoadOp() const { return m_ColorLoadOp; }
    LoadOp GetDepthLoadOp() const { return m_DepthLoadOp; }
    bool   GetDepthWrite()  const { return m_DepthWrite; }
    /// 零缓冲所在的输入槽：着色器声明了、但引擎顶点布局未提供（或超出顶点步长）的属性从这里取 0
    static constexpr u32 kZeroVertexSlot = 15;
    /// 实例序号缓冲所在的输入槽：顶点着色器的 SV_InstanceID 经 ShaderToDxil 改为逐实例属性
    /// HE_INSTANCEINDEX，从这里读 0,1,2…；IA 读逐实例数据时会加上起始实例，与 Vulkan 的
    /// gl_InstanceIndex 一致（D3D11 的 SV_InstanceID 同样不含起始实例）
    static constexpr u32 kInstanceIndexSlot = 14;
    bool NeedsZeroVertexBuffer()    const { return m_NeedsZeroVB; }
    bool NeedsInstanceIndexBuffer() const { return m_NeedsInstanceIndexVB; }

private:
    /// 取一个阶段的 DXBC 容器与绑定表，建立该阶段的 Binding 列表；字节码不可用返回 false
    bool LoadStage(D3D11Device* device, const ShaderBytecode* bc, Stage stage);
    bool BuildInputLayout(D3D11Device* device, const PipelineStateDesc& desc);
    void CreateFixedFunctionStates(D3D11Device* device, const PipelineStateDesc& desc);

    std::string                  m_Name;
    bool                         m_Valid = false;
    bool                         m_IsCompute = false;
    bool                         m_Reported = false;
    std::vector<u8>              m_VsBytecode;    // DXBC 容器（CreateInputLayout 要用）
    ComPtr<ID3D11VertexShader>   m_VS;
    ComPtr<ID3D11PixelShader>    m_PS;
    ComPtr<ID3D11ComputeShader>  m_CS;
    ComPtr<ID3D11InputLayout>    m_InputLayout;
    ComPtr<ID3D11RasterizerState>   m_Rasterizer;
    ComPtr<ID3D11DepthStencilState> m_DepthStencil;
    ComPtr<ID3D11BlendState>        m_Blend;
    D3D11_PRIMITIVE_TOPOLOGY     m_Topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<Binding>         m_Bindings[StageCount];
    u32    m_VertexStride = 0;
    bool   m_NeedsZeroVB = false;
    bool   m_NeedsInstanceIndexVB = false;
    LoadOp m_ColorLoadOp = LoadOp::Clear;
    LoadOp m_DepthLoadOp = LoadOp::Clear;
    bool   m_DepthWrite  = true;
};

// ---------------------------------------------------------------------------
//  QueryPool（占位，M3.7 实现）
// ---------------------------------------------------------------------------

class D3D11QueryPool final : public IRHIQueryPool {
public:
    D3D11QueryPool(u32 count, QueryType type) : m_Count(count), m_Type(type) {}
    u32       GetQueryCount() const override { return m_Count; }
    QueryType GetQueryType()  const override { return m_Type; }
private:
    u32       m_Count = 0;
    QueryType m_Type  = QueryType::Timestamp;
};

} // namespace he::rhi::d3d11

#endif // _WIN32
