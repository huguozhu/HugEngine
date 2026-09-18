// ============================================================
// Tests/TestBindlessHeap.cpp — 任务 23：bindless 槽位环形化 / N 帧延迟释放
//
// 覆盖两个与后端无关的"环形化"基础件（不依赖 Vulkan 设备，可纯 CPU 断言）：
//   · BindlessSlotRing  ：槽位复用的空闲表 + 保护期（graceFrames）语义
//   · FrameRetireQueue  ：有界 N 帧延迟释放队列（资源真的被释放、数量有界）
// 至于"堆里数组不再增长"，由这两个件的 GetSlotCount 不增来保证（VulkanBindlessHeap
// 直接复用它们），运行期证据见 02.Cube 的 "[任务 23] 文字纹理重建" 日志。
// ============================================================

#include "doctest.h"

#include "Scene/World.h"
#include "Scene/TextRenderComponent.h"
#include "Scene/InstancedMeshComponent.h"
#include "Scene/SkeletalMeshComponent.h"
#include "RHI/BindlessSlotRing.h"
#include "RHI/FrameRetireQueue.h"

#include <memory>
#include <string>
#include <vector>

using namespace he;
using he::rhi::BindlessSlotRing;
using he::rhi::FrameRetireQueue;

namespace {
// 极简 RHI 替身：只为验证"资源真的被释放"（析构时计数归零），不涉及任何后端
struct MockTexture : rhi::IRHITexture {
    explicit MockTexture(int* alive) : m_Alive(alive) { ++(*m_Alive); }
    ~MockTexture() override { --(*m_Alive); }
    u32 GetWidth() const override { return 4; }
    u32 GetHeight() const override { return 4; }
    u32 GetDepth() const override { return 1; }
    u32 GetMipLevels() const override { return 1; }
    u32 GetArrayLayers() const override { return 1; }
    rhi::Format GetFormat() const override { return rhi::Format::RGBA8_UNORM; }
    void* GetNativeHandle() const override { return nullptr; }
    void* GetNativeHandle(u32) const override { return nullptr; }
    int* m_Alive;
};

struct MockSampler : rhi::IRHISampler {};

struct MockBuffer : rhi::IRHIBuffer {
    explicit MockBuffer(int* alive) : m_Alive(alive) { ++(*m_Alive); }
    ~MockBuffer() override { --(*m_Alive); }
    usize GetSize() const override { return 16; }
    void* Map() override { return nullptr; }
    void Unmap() override {}
    u64 GetDeviceAddress() const override { return 0; }
    int* m_Alive;
};
} // namespace

TEST_CASE("BindlessSlotRing：空闲表为空时新分配槽位（只增不减）") {
    BindlessSlotRing ring(3);
    CHECK(ring.GetSlotCount() == 0);
    CHECK(ring.GetGraceFrames() == 3);

    CHECK(ring.Acquire() == 0);
    CHECK(ring.Acquire() == 1);
    CHECK(ring.Acquire() == 2);
    CHECK(ring.GetSlotCount() == 3);
    CHECK(ring.GetFreeSlotCount() == 0);
    CHECK(ring.GetPendingFreeCount() == 0);
}

TEST_CASE("BindlessSlotRing：释放的槽位要过保护期才能复用") {
    BindlessSlotRing ring(3);
    const u32 a = ring.Acquire();   // 0
    const u32 b = ring.Acquire();   // 1
    CHECK(a == 0);
    CHECK(b == 1);

    ring.Release(a);
    CHECK(ring.GetPendingFreeCount() == 1);
    CHECK(ring.GetFreeSlotCount() == 0);

    // 保护期内不复用：仍分配新槽（保护期内改写描述符会让在飞的帧取到错误资源）
    const u32 c = ring.Acquire();
    CHECK(c == 2);
    CHECK(ring.GetSlotCount() == 3);

    // 第 1、2 帧推进：还没到保护期（释放于第 0 帧，grace=3 ⇒ 第 3 帧才回收）
    ring.BeginFrame();
    ring.BeginFrame();
    CHECK(ring.GetFreeSlotCount() == 0);
    CHECK(ring.GetPendingFreeCount() == 1);

    // 第 3 帧推进：回收 → 复用的是被释放的那个槽，槽位总数**不再增长**
    ring.BeginFrame();
    CHECK(ring.GetFreeSlotCount() == 1);
    CHECK(ring.GetPendingFreeCount() == 0);
    const u32 d = ring.Acquire();
    CHECK(d == a);
    CHECK(ring.GetSlotCount() == 3);   // 关键判据：复用而不是继续追加
}

TEST_CASE("BindlessSlotRing：越界/重复释放/已空闲槽重复释放都安全忽略") {
    BindlessSlotRing ring(2);
    ring.Acquire();   // 0
    ring.Acquire();   // 1

    ring.Release(99);                     // 越界
    CHECK(ring.GetPendingFreeCount() == 0);
    ring.Release(0);
    ring.Release(0);                      // 重复
    CHECK(ring.GetPendingFreeCount() == 1);
    ring.Release(1);
    CHECK(ring.GetPendingFreeCount() == 2);

    // 全部回收后重复释放空闲槽：仍不产生新的待回收项
    ring.BeginFrame();
    ring.BeginFrame();
    CHECK(ring.GetFreeSlotCount() == 2);
    ring.Release(0);
    ring.Release(1);
    CHECK(ring.GetPendingFreeCount() == 0);
    CHECK(ring.GetFreeSlotCount() == 2);
}

TEST_CASE("BindlessSlotRing：graceFrames=0 视为 1，高频复用不会把槽位用空") {
    BindlessSlotRing ring(0);
    CHECK(ring.GetGraceFrames() == 1);
    ring.Acquire();
    ring.Release(0);
    CHECK(ring.Acquire() == 1);   // 当帧仍不能复用
    ring.BeginFrame();
    CHECK(ring.Acquire() == 0);   // 下一帧即可复用
}

TEST_CASE("BindlessSlotRing：长期高频注册/释放，槽位总数稳定（环形化核心判据）") {
    BindlessSlotRing ring(rhi::kMaxFramesInFlight);
    // 模拟"每帧重建纹理"：注册新槽 → 释放上一槽 → 帧边界推进，共 200 帧
    u32 slot = ring.Acquire();
    for (int frame = 0; frame < 200; ++frame) {
        ring.Release(slot);
        ring.BeginFrame();
        slot = ring.Acquire();       // 保护期已过 → 复用同一个槽
    }
    // append-only 时这里会是 201；环形化后稳定在"飞行帧数 + 1"量级
    CHECK(ring.GetSlotCount() <= rhi::kMaxFramesInFlight + 1);
}

TEST_CASE("FrameRetireQueue：N 帧后真的释放，且待释放数量有界") {
    using Queue = FrameRetireQueue<std::shared_ptr<std::string>>;
    Queue queue;
    std::weak_ptr<std::string> w1;

    {
        auto res = std::make_shared<std::string>("gpu-texture");
        w1 = res;
        queue.Retire(std::move(res));
        CHECK(queue.GetPendingCount() == 1);
        CHECK(!w1.expired());                     // 入队后仍被队列持有
    }

    // 未推进到该槽位之前不释放
    for (u32 i = 0; i + 1 < Queue::kSlots; ++i) queue.Advance();
    CHECK(!w1.expired());

    // 槽位轮转一圈 → 最老槽位被释放
    queue.Advance();
    CHECK(w1.expired());
    CHECK(queue.GetPendingCount() == 0);
    CHECK(Queue::kSlots == rhi::kMaxFramesInFlight * 2);   // 留一倍余量（同 DDQ 教训）
}

TEST_CASE("FrameRetireQueue：每帧退役 1 个持续 500 帧，队列长度保持有界") {
    using Queue = FrameRetireQueue<std::shared_ptr<int>>;
    Queue queue;
    u32 maxPending = 0;
    for (int frame = 0; frame < 500; ++frame) {
        queue.Advance();                          // 先推进（释放最老槽）
        queue.Retire(std::make_shared<int>(frame));   // 再入队本帧退役
        maxPending = std::max(maxPending, queue.GetPendingCount());
    }
    // 有界：最多 kSlots 个（原来"无界 vector 保活"这里会是 500）
    CHECK(maxPending <= Queue::kSlots);
    CHECK(queue.GetPendingCount() <= Queue::kSlots);
}

TEST_CASE("FrameRetireQueue：FlushAll 立即释放全部") {
    using Queue = FrameRetireQueue<std::unique_ptr<int>>;
    Queue queue;
    for (int i = 0; i < 5; ++i) queue.Retire(std::make_unique<int>(i));
    CHECK(queue.GetPendingCount() == 5);
    queue.FlushAll();
    CHECK(queue.GetPendingCount() == 0);
}

// ============================================================
// 组件侧：高频更新时"旧资源保活"变成"有界延迟释放"
// ============================================================

TEST_CASE("TextRender 组件：高频重建纹理时退役有界、旧纹理真的被释放") {
    using Queue = FrameRetireQueue<TextRenderComponent::RetiredTexture>;
    World world;
    Entity e = world.CreateEntity("Text");
    auto* tr = world.AddComponent<TextRenderComponent>(e);
    REQUIRE(tr != nullptr);

    int alive = 0;
    u32 maxPending = 0;
    int maxAlive = 0;
    for (int i = 0; i < 100; ++i) {           // 模拟 100 次"文字内容变化 → 重建纹理"
        tr->AdvanceRetireQueue();             // TextRenderSystem::Update 每帧调用
        tr->RetireRuntimeTexture();           // 旧纹理退役（原先：无界保活）
        tr->runtimeTexture = std::make_unique<MockTexture>(&alive);
        tr->runtimeSampler = std::make_unique<MockSampler>();
        maxPending = std::max(maxPending, tr->GetRetiredTextureCount());
        maxAlive   = std::max(maxAlive, alive);
    }
    CHECK(maxPending <= Queue::kSlots);                       // 待释放有界
    CHECK(maxAlive <= (int)Queue::kSlots + 1);                // 存活纹理有界（当前 + 队列）
    CHECK(alive > 1);                                         // 队列里还压着"保护期内"的旧纹理
    tr->retiredTextures.FlushAll();                           // 立即释放（等价于 GPU 已 idle）
    CHECK(alive == 1);                                        // 只剩当前这张
    CHECK(Queue::kSlots == rhi::kMaxFramesInFlight * 2);
}

TEST_CASE("InstancedMesh / SkeletalMesh 组件：缓冲退役有界、旧缓冲真的被释放") {
    using BufferQueue = FrameRetireQueue<std::unique_ptr<rhi::IRHIBuffer>>;
    World world;

    Entity e1 = world.CreateEntity("IM");
    auto* im = world.AddComponent<InstancedMeshComponent>(e1);
    Entity e2 = world.CreateEntity("SM");
    auto* sm = world.AddComponent<SkeletalMeshComponent>(e2);
    REQUIRE(im != nullptr);
    REQUIRE(sm != nullptr);

    int alive = 0;
    u32 maxPending = 0;
    int maxAlive = 0;
    for (int i = 0; i < 50; ++i) {            // 模拟 50 次"扩容重建"
        im->AdvanceRetireQueue();
        sm->AdvanceRetireQueue();
        im->RetireInstanceBuffer();
        sm->RetireBoneBuffer();
        im->instanceBuffer = std::make_unique<MockBuffer>(&alive);
        sm->boneBuffer     = std::make_unique<MockBuffer>(&alive);
        im->instanceBufferCapacity = 8;
        sm->boneBufferCapacity     = 24;
        im->instanceSSBOHandle = 3;
        sm->boneSSBOHandle     = 5;
        maxPending = std::max(maxPending, im->GetRetiredBufferCount());
        maxAlive   = std::max(maxAlive, alive);
    }
    CHECK(maxPending <= BufferQueue::kSlots);
    CHECK(maxAlive <= (int)BufferQueue::kSlots * 2 + 2);      // 两个组件的当前 + 队列
    im->retiredBuffers.FlushAll();                            // 立即释放（GPU 已 idle）
    sm->retiredBoneBuffers.FlushAll();
    CHECK(alive == 2);                                        // 各自只剩当前缓冲
    CHECK(im->GetInstanceBufferCapacity() == 8);
    CHECK(sm->boneBufferCapacity == 24);
    CHECK(im->instanceSSBOHandle == 3);                       // 复用容量时句柄保持不变
    CHECK(sm->boneSSBOHandle == 5);
}
