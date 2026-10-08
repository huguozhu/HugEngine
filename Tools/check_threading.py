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
    python Tools/check_threading.py --gate          # 闸门模式：帧内命中非 0 时返回 1（各阶段退出用）
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


def main():
    ap = argparse.ArgumentParser(description="渲染线程化方案：跨线程调用点清点 / 闸门")
    ap.add_argument("--root", default="Engine/Render", help="扫描目录（默认 Engine/Render）")
    ap.add_argument("--ext", default=".cpp,.h,.slang", help="参与扫描的扩展名（逗号分隔）")
    ap.add_argument("--detail", action="store_true", help="打印每条 file:line + 所属函数")
    ap.add_argument("--gate", action="store_true",
                    help="闸门模式：帧内命中 > 0 时退出码 1（阶段退出判据用；默认只看报告）")
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
    if args.gate and total_frame > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
