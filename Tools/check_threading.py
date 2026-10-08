#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_threading.py — 渲染线程化方案的"跨线程会炸"调用点清点与闸门（阶段 0 T0.2 / 附录 B）

为什么要它
----------
渲染要搬到独立线程，最危险的不是"渲染代码写了什么"，而是**哪些调用必须在某根特定线程上执行**：
每帧上传（写映射缓冲）、每帧读回、等待型提交、帧内资源创建、交换链操作。靠肉眼在 4 万行渲染代码里
找这些点既慢又会漏；本脚本把它们按类别扫出来，并用"所属函数名"粗判**帧内**还是**加载期**，
作为方案 §5 阶段 0 T0.2 的登记清单，同时是附录 B 各阶段退出闸门的可执行版本。

判定口径（刻意保守，宁可多报帧内）
----------------------------------
- 类别见 `CATEGORIES`；注释行一律跳过（`//` 与 `/* ... */` 起始行）。
- **加载期白名单**：所属函数名含 Initialize / Init / Shutdown / Resize / Load / Upload / Setup /
  Construct / OnCreate 之一 ⇒ 记为"加载期"（阶段 2 之后这些点允许存在，但要走 ResourceCreationService）。
  其余一律记为"帧内"——包括 `BuildFrameGraph` 这类**名字里有 Build 但每帧都跑**的函数，
  所以白名单里**故意不含 Build / Create**（否则会把真正的帧内创建藏起来）。
- 函数名取"该行向上最近的、形如 `名字(...)` 且以 `{` 或行尾结束的定义行"；多行签名会退化成上一个
  能识别的函数名 ⇒ 结果只用于**排序与登记**，不当作证明。

用法
----
    python Tools/check_threading.py                 # 汇总（Engine/Render）
    python Tools/check_threading.py --detail        # 附每条 file:line + 所属函数
    python Tools/check_threading.py --root Samples  # 换目录（附录 B 的 B2 查样例）
    python Tools/check_threading.py --handles       # 附带 T0.7 的资源持有者统计（基线即上限）
    python Tools/check_threading.py --world-deps     # 附带附录 B1 的世界依赖统计（阶段 1 退出目标 = 0）
    python Tools/check_threading.py --gate          # 闸门模式：帧内命中非 0（或持有者/世界依赖超基线）返回 1
"""
import argparse
import os
import re
import sys

# --- 类别：名称 → (正则, 说明, 处置与所属阶段) ---
CATEGORIES = [
    ("SWAPCHAIN", re.compile(r"AcquireNextImage|(?<!\.)\bPresent\s*\("),
     "交换链操作（拿图 / 呈现）", "阶段 2 T2.2：整体迁入渲染线程（C2）"),
    ("WAIT", re.compile(r"WaitFenceOnQueue|WaitForFence|DeviceWaitIdle|QueueWaitIdle|\bWaitIdle\b"),
     "等待型提交 / 空闲等待", "阶段 2：只允许渲染线程，且帧内禁止（铁律 3）；加载期保留"),
    ("CREATE", re.compile(r"(?:->|\.)\s*Create(Transient)?(Buffer|Texture|Sampler|PipelineState|"
                          r"GraphicsPipeline|ComputePipeline|SwapChain|CommandList|DescriptorSet|"
                          r"DescriptorPool|QueryPool|Fence|Semaphore|RayTracingPipeline|MeshPipeline)\s*\("),
     "资源创建（设备调用形态 `x->Create*()`）",
     "阶段 2 T2.3：帧内改走 ResourceCreationService（步 1 同步转发，仅限加载/重建）"),
    ("MAP", re.compile(r"->\s*Map\s*\(\s*\)"),
     "映射缓冲访问（直写上传 或 GPU→CPU 读回）",
     "阶段 2 T2.6：读回改『登记 + N 帧后取值』（FrameRetireQueue）；"
     "直写改入队拷贝（铁律 2）"),
    ("UPLOAD_DESC", re.compile(r"\binitialData\b"),
     "创建期上传（desc.initialData）", "阶段 2 T2.3：创建期上传经创建服务在渲染线程执行"),
]

# --- T0.7 的 grep 闸门：资源持有者（`unique_ptr<IRHIBuffer/IRHITexture>`）数量只允许下降 ---
# 判据（方案 §9 T0.7）："新代码不再新增 unique_ptr<IRHIBuffer> 成员"。基线是 2026-09-24 在
# multi_thread 分支上的实测值：277 处、76 个文件（正是 §12.5 所说"200+ 处持有者"）。
# 注意它是**上限**而不是目标：迁移可以慢慢做，但新增一个就说明新代码没走句柄。
HOLDER_PATTERN = re.compile(r"unique_ptr<\s*(?:rhi::)?IRHI(?:Buffer|Texture)")
HOLDER_BASELINE = 277
HOLDER_ROOTS = ("Engine", "Samples")
HOLDER_EXTS = (".h", ".hpp", ".cpp")

# --- 附录 B1 的 grep 闸门：`Engine/Render/` 里的"渲染期世界依赖" ---
# 判据（方案 §5 阶段 1 T1.5 / 附录 B1）：渲染期函数签名不再出现 `he::World&` / `SceneGraph&`，
# 阶段 1 退出时应为 **0**（加载期例外逐个白名单）。
# 统计口径与调用点清点一致：只看代码行（跳过注释），按**所属函数名**判帧内/加载期。
# 迁移期间它是**上限**（只允许下降），因此基线随每次收敛手动下调。
# 【口径注意】同一函数的**声明与定义各算一处**（头文件 + .cpp），所以这个数大于"函数个数"；
# 作为闸门它只需要前后一致、单调下降即可。
WORLD_DEP_PATTERN = re.compile(r"\b(?:he::)?(?:World|SceneGraph)\s*&")
WORLD_DEP_ROOTS = ("Engine/Render",)
# 【白名单】快照层（`Engine/Render/Threading/`）是渲染侧**唯一**允许读世界的代码 —— 它就是干这个的：
# 在游戏线程把渲染输入取齐成不可变快照。把它的命中排除在外，度量才对准"渲染期泄漏"。
WORLD_DEP_WHITELIST_PATHS = ("Engine/Render/Threading/",)
WORLD_DEP_BASELINE = 69        # 2026-10-09 实测（第③段第 1 批后：GBufferRenderer 家族 6 处
                               # + `GPUScene::Collect` 过渡重载 2 处去掉 —— 前者不再需要 world/sg，
                               # 后者改由调用方直接消费自己的快照。阶段 1 退出目标 = 0；
                               # 注：该脚本按行计数，参数行合并/拆分会造成 ±1 的行计数效应）

# --- 附录 E 的度量：渲染侧的**组件指针依赖**（`MeshComponent*` 等）---
# 【为什么需要第二项】B1 统计的是签名里的 `World&` / `SceneGraph&`，量不出 E-1/E-2/E-3 消除的东西 ——
# 它们的产物是"渲染侧不再持有 `MeshComponent*`"（快照只带 `meshIndex`）。这两项是**不同度量**，
# 已在方案 §14.5 里写明；本项让 E-3 的收敛可量化。
MESH_PTR_PATTERN = re.compile(r"\b(?:he::)?(?:Mesh|SkeletalMesh|InstancedMesh|SplineMesh|Decal)Component\s*\*")
MESH_PTR_BASELINE = 13         # 2026-10-09 实测（第②段后：`SceneRenderer.{h,cpp}` 的 1+4 处已清除
                               # —— `DrawItem::mesh` 字段与其地址兜底分支全部删除；剩下的是
                               # `Pipeline/RTPass.{h,cpp}` 的 4+9 处：BLAS 缓存键 `MeshComponent*`、
                               # 顶点拉取路径与场景材质纹理的逐网格取值。该文件属 E-3② 的收尾项
                               # （方案 §14.5 建议与 E-4 口径统一一起做），目标仍是渲染期 0）

LOAD_TIME_WHITELIST = ("Initialize", "Init", "Shutdown", "Resize", "Load", "Upload",
                       "Setup", "Construct", "OnCreate")

FUNC_RE = re.compile(
    r"^[A-Za-z_][\w:<>,~*&\s]*?\b([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\([^;{}]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{?\s*$")

SKIP_PREFIX = ("//", "*", "/*", "#")


def enclosing_function(lines, index):
    """向上找最近的可识别函数定义名；找不到返回 '<文件作用域>'。"""
    for i in range(index, -1, -1):
        stripped = lines[i].strip()
        if not stripped or stripped.startswith("//"):
            continue
        m = FUNC_RE.match(lines[i].rstrip())
        if m:
            return m.group(1)
    return "<文件作用域>"


def classify_categories(line):
    return [name for name, rx, _desc, _fix in CATEGORIES if rx.search(line)]


def is_load_time(func_name):
    return any(hint in func_name for hint in LOAD_TIME_WHITELIST)


def scan_file(path, root):
    """返回 [(类别, 行号, 函数名, 是否加载期)]"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError as exc:                       # 读不了就跳过（不中断盘点）
        print("  跳过 %s（%s）" % (path, exc), file=sys.stderr)
        return []

    hits = []
    for idx, raw in enumerate(lines):
        stripped = raw.strip()
        if not stripped or stripped.startswith(SKIP_PREFIX):
            continue                             # 注释 / 预处理行不计入
        for name in classify_categories(raw):
            func = enclosing_function(lines, idx)
            hits.append((name, idx + 1, func, is_load_time(func)))
    return hits


def count_resource_holders(repo_root):
    """统计 `unique_ptr<IRHIBuffer/IRHITexture>` 的出现次数与文件数（T0.7 的 grep 闸门）。

    为什么按"出现次数"而不是"成员数"：成员声明、函数参数、局部变量都算"持有者"，
    它们都是句柄化要替换的对象；用同一个口径做基数比较即可，不需要区分语法位置。
    """
    total = 0
    per_file = {}
    for root_name in HOLDER_ROOTS:
        root_path = os.path.join(repo_root, root_name)
        if not os.path.isdir(root_path):
            continue
        for dirpath, _dirnames, filenames in os.walk(root_path):
            for fn in sorted(filenames):
                if not fn.endswith(HOLDER_EXTS):
                    continue
                full = os.path.join(dirpath, fn)
                count = 0
                try:
                    with open(full, "r", encoding="utf-8", errors="replace") as fh:
                        for line in fh:
                            if line.strip().startswith(("//", "*", "/*")):
                                continue                       # 注释里提到不算
                            count += len(HOLDER_PATTERN.findall(line))
                except OSError:
                    continue
                if count:
                    total += count
                    per_file[os.path.relpath(full, repo_root).replace("\\", "/")] = count
    return total, per_file


def count_world_deps(repo_root):
    """统计 `Engine/Render/` 里的世界依赖（`World&` / `SceneGraph&`）：返回 (帧内, 加载期, 明细)。

    为什么按"所属函数名"再分一次：方案允许**加载期**（Initialize/OnResize/加载器）访问世界，
    要收敛的是**渲染期**。分类口径与调用点清点完全一致（同一份白名单），避免两套口径。
    """
    frame_total = 0
    load_total = 0
    per_file = {}
    for root_name in WORLD_DEP_ROOTS:
        root_path = os.path.join(repo_root, root_name)
        if not os.path.isdir(root_path):
            continue
        for dirpath, _dirnames, filenames in os.walk(root_path):
            for fn in sorted(filenames):
                if not fn.endswith(HOLDER_EXTS):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, repo_root).replace("\\", "/")
                if any(rel.startswith(w) for w in WORLD_DEP_WHITELIST_PATHS):
                    continue                       # 快照层：渲染侧唯一允许读世界的地方（见常量处说明）
                try:
                    with open(full, "r", encoding="utf-8", errors="replace") as fh:
                        lines = fh.read().splitlines()
                except OSError:
                    continue
                for idx, raw in enumerate(lines):
                    stripped = raw.strip()
                    if not stripped or stripped.startswith(("//", "*", "/*")):
                        continue
                    if not WORLD_DEP_PATTERN.search(raw):
                        continue
                    func = enclosing_function(lines, idx)
                    if is_load_time(func):
                        load_total += 1
                        per_file.setdefault(rel, {"帧内": 0, "加载期": 0})["加载期"] += 1
                    else:
                        frame_total += 1
                        per_file.setdefault(rel, {"帧内": 0, "加载期": 0})["帧内"] += 1
    return frame_total, load_total, per_file


def count_mesh_pointers(repo_root):
    """统计 `Engine/Render/`（排除快照层）里的**组件指针依赖**：返回 (帧内, 加载期, 明细)。

    与 B1 同一套分类口径（所属函数名 + 同一份白名单），保证两项闸门可比。
    """
    frame_total = 0
    load_total = 0
    per_file = {}
    for root_name in WORLD_DEP_ROOTS:
        root_path = os.path.join(repo_root, root_name)
        if not os.path.isdir(root_path):
            continue
        for dirpath, _dirnames, filenames in os.walk(root_path):
            for fn in sorted(filenames):
                if not fn.endswith(HOLDER_EXTS):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, repo_root).replace("\\", "/")
                if any(rel.startswith(w) for w in WORLD_DEP_WHITELIST_PATHS):
                    continue
                try:
                    with open(full, "r", encoding="utf-8", errors="replace") as fh:
                        lines = fh.read().splitlines()
                except OSError:
                    continue
                for idx, raw in enumerate(lines):
                    stripped = raw.strip()
                    if not stripped or stripped.startswith(("//", "*", "/*")):
                        continue
                    if not MESH_PTR_PATTERN.search(raw):
                        continue
                    func = enclosing_function(lines, idx)
                    if is_load_time(func):
                        load_total += 1
                        per_file.setdefault(rel, {"帧内": 0, "加载期": 0})["加载期"] += 1
                    else:
                        frame_total += 1
                        per_file.setdefault(rel, {"帧内": 0, "加载期": 0})["帧内"] += 1
    return frame_total, load_total, per_file


def main():
    ap = argparse.ArgumentParser(description="渲染线程化方案：跨线程调用点清点 / 闸门")
    ap.add_argument("--root", default="Engine/Render", help="扫描目录（默认 Engine/Render）")
    ap.add_argument("--ext", default=".cpp,.h,.slang", help="参与扫描的扩展名（逗号分隔）")
    ap.add_argument("--detail", action="store_true", help="打印每条 file:line + 所属函数")
    ap.add_argument("--gate", action="store_true",
                    help="闸门模式：帧内命中 > 0（或资源持有者超过基线）时退出码 1")
    ap.add_argument("--handles", action="store_true",
                    help="附带 T0.7 的资源持有者统计（`unique_ptr<IRHIBuffer/IRHITexture>`）")
    ap.add_argument("--world-deps", action="store_true",
                    help="附带附录 B1 的世界依赖统计（Engine/Render 内的 World& / SceneGraph&）")
    ap.add_argument("--mesh-ptrs", action="store_true",
                    help="附带附录 E 的组件指针依赖统计（Engine/Render 内的 *Component*）")
    args = ap.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    scan_root = os.path.join(repo_root, args.root)
    if not os.path.isdir(scan_root):
        print("目录不存在：%s" % scan_root, file=sys.stderr)
        return 2

    exts = tuple(e.strip() for e in args.ext.split(",") if e.strip())
    per_cat = {name: {"帧内": 0, "加载期": 0, "detail": []} for name, _r, _d, _f in CATEGORIES}
    scanned = 0

    for dirpath, _dirnames, filenames in os.walk(scan_root):
        for fn in sorted(filenames):
            if not fn.endswith(exts):
                continue
            scanned += 1
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, repo_root).replace("\\", "/")
            for name, line_no, func, load in scan_file(full, repo_root):
                bucket = per_cat[name]
                bucket["加载期" if load else "帧内"] += 1
                bucket["detail"].append((rel, line_no, func, load))

    print("=== 渲染线程化清点（%s，扫描 %d 个文件）===" % (args.root, scanned))
    print("%-12s %8s %8s   %s" % ("类别", "帧内", "加载期", "处置"))
    total_frame = 0
    for name, _rx, desc, fix in CATEGORIES:
        b = per_cat[name]
        total_frame += b["帧内"]
        print("%-12s %8d %8d   %s" % (name, b["帧内"], b["加载期"], fix))
        print("%-12s %s" % ("", desc))

    if args.detail:
        for name, _rx, _desc, _fix in CATEGORIES:
            rows = per_cat[name]["detail"]
            if not rows:
                continue
            print("\n--- %s（%d 条）---" % (name, len(rows)))
            for rel, line_no, func, load in rows:
                print("  %s%-70s %s:%d" % ("[加载]" if load else "[帧内] ", func, rel, line_no))

    print("\n帧内命中合计 = %d（阶段 2 退出时应为 0；加载期用例保留但需经创建服务）" % total_frame)

    holders_over = False
    if args.handles:
        total, per_file = count_resource_holders(repo_root)
        print("\n=== T0.7 资源持有者（unique_ptr<IRHIBuffer/IRHITexture>）===")
        print("当前 %d 处 / %d 个文件；基线 %d（只允许下降：新代码必须用句柄）"
              % (total, len(per_file), HOLDER_BASELINE))
        if total > HOLDER_BASELINE:
            holders_over = True
            print("超出基线的文件（新增持有者必须改为 RHIBufferHandle / RHITextureHandle）：")
            for path, count in sorted(per_file.items(), key=lambda kv: -kv[1])[:10]:
                print("  %-70s %d" % (path, count))

    world_deps_over = False
    if args.world_deps:
        frame_total, load_total, per_file = count_world_deps(repo_root)
        print("\n=== 附录 B1 世界依赖（Engine/Render 内的 World& / SceneGraph&）===")
        print("渲染期 %d 处 / 加载期 %d 处；基线 %d（阶段 1 退出目标 = 0，迁移期只允许下降）"
              % (frame_total, load_total, WORLD_DEP_BASELINE))
        if frame_total > WORLD_DEP_BASELINE:
            world_deps_over = True
        print("渲染期命中最多的文件（这些就是 T1.4/T1.5 要收敛的对象）：")
        ranked = sorted(((p, c["帧内"]) for p, c in per_file.items() if c["帧内"] > 0),
                        key=lambda kv: -kv[1])[:12]
        for path, count in ranked:
            print("  %-70s %d" % (path, count))

    mesh_ptr_over = False
    if args.mesh_ptrs:
        mf, ml, mper = count_mesh_pointers(repo_root)
        print("\n=== 附录 E 组件指针依赖（Engine/Render 内的 *Component*）===")
        print("渲染期 %d 处 / 加载期 %d 处；基线 %d（E-3 的收敛目标：渲染期不再持有组件指针）"
              % (mf, ml, MESH_PTR_BASELINE))
        if mf > MESH_PTR_BASELINE:
            mesh_ptr_over = True
        for path, c in sorted(((p, c["帧内"]) for p, c in mper.items() if c["帧内"] > 0),
                              key=lambda kv: -kv[1])[:12]:
            print("  %-70s %d" % (path, c))

    if args.gate and (total_frame > 0 or holders_over or world_deps_over or mesh_ptr_over):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
