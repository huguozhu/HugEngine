#pragma once

// ============================================================
// TransientAliasTest.h — P6：瞬态纹理 / 内存别名的真实用例自检
//
// 背景：引擎已经实现了"瞬态纹理 + 内存别名"（生命周期不重叠的纹理共用同一段内存），
// 但样例里 RenderGraph 自建的纹理数为 0，这条路径从未在真实场景跑过。本自检用
// 三张瞬态纹理把判据全部跑一遍，并打印可核对的证据：
//
//   A[1,2] —— 生命周期 pass 1..2
//   B[2,3] —— 生命周期 pass 2..3（与 A、C 都重叠）
//   C[3,4] —— 生命周期 pass 3..4（与 A 不重叠）
//
// 判据：
//   1. A 与 C 生命周期不重叠 ⇒ 共用同一段内存（堆内偏移相同）；
//   2. B 与二者都重叠 ⇒ 必须单独分配（偏移与 A、C 都不同）；
//   3. 三张图各自"首次使用"时要走别名屏障（D3D12：Aliasing barrier + DiscardResource；
//      Vulkan：UNDEFINED → COLOR_ATTACHMENT 的布局转换，UNDEFINED 即丢弃原内容）；
//   4. 打印瞬态堆的总占用字节数，并与"逐张独立分配"的朴素总量对比给出节省。
//
// 用法：任意样例/测试在持有 IRHIDevice 时调用 he::samples::RunTransientAliasTest(device)。
//       本仓库把它接到 02.Cube：设置 HE_TRANSIENT_ALIAS_TEST=1 后启动，跑完即打印并退出。
// ============================================================

namespace he::rhi { class IRHIDevice; }

namespace he::samples {

/// 运行瞬态纹理内存别名自检。全部判据通过返回 true，否则返回 false（并打印失败原因）。
bool RunTransientAliasTest(he::rhi::IRHIDevice* device);

} // namespace he::samples
