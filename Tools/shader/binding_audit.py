# -*- coding: utf-8 -*-
"""binding_audit.py — C++/Shader 绑定契约的反射对账（Vulkan / DX12 / DX11）

背景
====
C++/Slang 共享契约（ShaderTypes/GPUBindings.slang）里的 kGPUBinding_* 只描述
**Vulkan 的 (set, binding)**。D3D 两端的落点由编译器决定，而且**不是**同一组数字：

  · DX12：`[[vk::binding]]` 被忽略；Slang 把资源按**声明顺序**在每个 register
    bank（t/s/b/u）内从 0 递增分配，`space` 也被紧凑重排（Vulkan set 1 → space 0）。
  · DX11：连 `space` 都没有（SM5.1 的 space 属 D3D12），且寄存器上限很小
    （b0–b13 / s0–s15 / u0–u7 / t0–t127）。

所以「一份源码三后端共用」成立的前提是：**D3D 侧按反射绑、不按 kGPUBinding_* 猜**。
本脚本就是这条前提的验收器：逐 shader × 逐后端编译一次，把三列落点摆在一起，
并对以下几类问题在退出码上报警：

  1. Vulkan 列的 (set, binding) 与源码里的 `[[vk::binding(常量, 常量)]]` 不一致
     （常量改了、shader 没同步，或者解析出的名字与反射对不上）；
  2. 同一个 shader 里两个资源占用同一个 (set, binding)（Vulkan 要求唯一）；
  3. 资源没有显式 `[[vk::binding]]`（等于让 Slang 自动分配，跨编译器不稳定）；
  4. DX11 侧超出寄存器上限（b13/s15/u7/t127）；
  5. RHI 镜像常量（Engine/RHI/RHI/Types.h）与共享契约的数值不一致。

用法
====
    python Tools/shader/binding_audit.py                     # 全量（146 入口 × 3 后端）
    python Tools/shader/binding_audit.py --only DeferredLighting
    python Tools/shader/binding_audit.py --targets vk,dx12   # 只跑两列
    python Tools/shader/binding_audit.py --out Build/shader_audit --jobs 8

产出：`<out>/contract.md`（人读）、`<out>/contract.csv`（机读）、
      `<out>/summary.txt`（计数与问题清单）。退出码 0 = 无契约问题。
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

# Windows 控制台默认 GBK，打印中文/符号会抛 UnicodeEncodeError；报告文件仍是 UTF-8。
try:
    sys.stdout.reconfigure(errors="replace")
except Exception:  # noqa: BLE001 — 老版本 Python 没有 reconfigure，忽略
    pass

# ---------------------------------------------------------------------------
# 常量与约定
# ---------------------------------------------------------------------------

# 后缀 → (Slang stage, 入口函数名)；与 Engine/Shader/CMakeLists.txt 的映射保持一致
STAGE_MAP = {
    "vert": ("vertex", "vertexMain"),
    "frag": ("fragment", "fragmentMain"),
    "comp": ("compute", "main"),
    "rgen": ("raygeneration", "main"),
    "rmiss": ("miss", "main"),
    "rchit": ("closesthit", "main"),
    "rahit": ("anyhit", "main"),
    "rcall": ("callable", "main"),
    "mesh": ("mesh", "main"),
    "task": ("task", "main"),
}

# 后端 → (slangc -target, 输出后缀)
BACKENDS = {
    "vk": ("spirv", ".spv"),
    "dx12": ("hlsl", ".hlsl"),
    "dx11": ("dxbc", ".dxbc"),
}

# 反射里的 kind → D3D register bank（DX11/DX12 通用；DX11 的 kind 名与 DX12 一致）
KIND_TO_BANK = {
    "shaderResource": "t",
    "samplerState": "s",
    "constantBuffer": "b",
    "uniform": "b",          # 某些后端把 cbuffer 报成 uniform
    "unorderedAccess": "u",
}

# DX11 的寄存器上限（D3D11 FL11_0；b 的上限是 14 个常量缓冲）
DX11_LIMITS = {"b": 14, "s": 16, "u": 8, "t": 128}

# 共享契约里允许定义 bindings/constants 的切片（GPU_CONST 可以出现在其中任一文件）
CONST_SLICE_GLOBS = ["ShaderTypes/*.slang", "ShaderTypes/PushConstants/*.slang"]

RE_BINDING_ATTR = re.compile(r"\[\[\s*vk::binding\(\s*([^,]+?)\s*,\s*([^)]+?)\s*\)\s*\]\]")
RE_GPU_CONST_INT = re.compile(r"GPU_CONST\(\s*GPU_UINT\s+(\w+)\s*=\s*(\d+)\s*\)")
RE_RHI_CONST_INT = re.compile(r"constexpr\s+u32\s+(\w+)\s*=\s*(\d+)\s*;")
RE_STAGE_FILE = re.compile(r"\.(" + "|".join(STAGE_MAP) + r")\.slang$")
RE_INCLUDE = re.compile(r'#include\s+"([^"]+)"')


@dataclass
class SourceBinding:
    """源码里一条 `[[vk::binding(...)]]` 声明。"""
    shader: str
    name: str           # GPU 参数名（纹理/缓冲/cbuffer 名）
    type_kw: str        # 声明开头的类型关键字（Texture2D / SamplerState / cbuffer …）
    is_array: bool      # 是否无界数组（`u_X[]`；bindless 的多个"视图"会共用同一个槽）
    set_expr: str
    binding_expr: str
    line: int
    set_value: int | None = None
    binding_value: int | None = None


@dataclass
class BackendResult:
    """一次后端的编译 + 反射结果。"""
    ok: bool = False
    error: str = ""
    # 参数名 → {"kind": str, "space": int, "index": int}
    params: dict[str, dict] = field(default_factory=dict)


@dataclass
class ShaderResult:
    shader: str
    path: Path
    stage: str
    entry: str
    source: list[SourceBinding]
    push_constants: list[str] = field(default_factory=list)   # `[[vk::push_constant]]` 的名字
    backends: dict[str, BackendResult] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# 解析：契约常量、RHI 镜像、shader 源码里的绑定声明
# ---------------------------------------------------------------------------

def find_slangc(explicit: str | None) -> str:
    """按 命令行 → VULKAN_SDK → PATH 的顺序找 slangc。"""
    cands = []
    if explicit:
        cands.append(explicit)
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        cands += [str(Path(sdk) / "Bin" / "slangc.exe"), str(Path(sdk) / "bin" / "slangc")]
    cands.append("slangc")
    for c in cands:
        p = shutil.which(c) or (c if Path(c).is_file() else None)
        if p:
            return str(p)
    sys.exit("找不到 slangc：用 --slangc 指定，或设置 VULKAN_SDK")


def load_constants(shaders_dir: Path) -> dict[str, int]:
    """解析共享契约里的 GPU_CONST 整数常量（kGPUBinding_* / kGPUDescSet_* 等）。"""
    out: dict[str, int] = {}
    for pat in CONST_SLICE_GLOBS:
        for f in sorted(shaders_dir.glob(pat)):
            for m in RE_GPU_CONST_INT.finditer(f.read_text(encoding="utf-8")):
                out[m.group(1)] = int(m.group(2))
    return out


def load_rhi_mirror(types_h: Path) -> dict[str, int]:
    """解析 RHI 侧的镜像常量（kBinding* / kDescSet* / kMax* …）。"""
    if not types_h.is_file():
        return {}
    return {m.group(1): int(m.group(2))
            for m in RE_RHI_CONST_INT.finditer(types_h.read_text(encoding="utf-8"))}


def norm(name: str) -> str:
    """名字归一化：去掉下划线并小写，用于跨侧比对（kGPUBinding_X ↔ kBindingX）。"""
    return name.replace("_", "").lower()


def check_rhi_mirror(consts: dict[str, int], mirror: dict[str, int]) -> list[str]:
    """RHI 镜像 vs 共享契约：kXxx ↔ kGPUXxx（忽略下划线/大小写）。"""
    problems = []
    by_norm = {norm(k): (k, v) for k, v in consts.items()}
    for rhi_name, rhi_val in mirror.items():
        if not rhi_name.startswith("k"):
            continue
        cand = "kGPU" + rhi_name[1:]
        hit = by_norm.get(norm(cand))
        if not hit:
            continue
        cname, cval = hit
        if rhi_val != cval:
            problems.append(f"RHI 镜像 {rhi_name}={rhi_val} 与契约 {cname}={cval} 不一致")
    return problems


def iter_shader_files(shaders_dir: Path):
    """枚举编译入口（带 stage 后缀的 .slang），按路径排序保证输出稳定。"""
    for f in sorted(shaders_dir.rglob("*.slang")):
        m = RE_STAGE_FILE.search(f.name)
        if m:
            yield f, m.group(1)


def parse_shader_source(path: Path, consts: dict[str, int]) -> list[SourceBinding]:
    """从 shader 源码里抽 `[[vk::binding(X, Y)]] <声明>` 的 (参数名, set, binding)。

    只做**文本层**解析：属性后面跟到第一个 `;` 为止，从这一段里取参数名
    （`cbuffer NAME {` 取 NAME；否则取最后一个标识符，兼容 `u_X[]` 这种数组写法）。
    """
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    out: list[SourceBinding] = []
    for i, line in enumerate(lines):
        m = RE_BINDING_ATTR.search(line)
        if not m:
            continue
        # 从属性所在行开始拼到第一个 ';'（声明可能换行）
        chunk = line[m.end():]
        j = i
        while ";" not in chunk and j + 1 < len(lines) and j - i < 6:
            j += 1
            chunk += " " + lines[j]
        chunk = chunk.split(";")[0]

        name = ""
        type_kw = ""
        mc = re.search(r"\bcbuffer\s+(\w+)", chunk)
        if mc:
            name = mc.group(1)
            type_kw = "cbuffer"
        else:
            mn = re.findall(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*$", chunk.strip())
            if mn:
                name = mn[-1]
            mt = re.match(r"\s*(?:const\s+|uniform\s+)?([A-Za-z_]\w*)", chunk)
            type_kw = mt.group(1) if mt else ""
        if not name:
            continue

        def resolve(expr: str) -> int | None:
            expr = expr.strip()
            if re.fullmatch(r"\d+", expr):
                return int(expr)
            return consts.get(expr)

        out.append(SourceBinding(
            shader=path.name, name=name, type_kw=type_kw,
            is_array=bool(re.search(rf"\b{re.escape(name)}\s*\[", chunk)),
            set_expr=m.group(2).strip(), binding_expr=m.group(1).strip(),
            line=i + 1,
            set_value=resolve(m.group(2)), binding_value=resolve(m.group(1)),
        ))
    return out


def include_closure(path: Path, root: Path, seen: set[Path] | None = None) -> list[Path]:
    """按 `#include` 递归收集依赖（深度优先、去重、防环）。

    为什么要跟随 include：`u_Textures[]` / `u_Samplers[]` / `u_Materials[]` 这类
    bindless 数组声明在 `common.slang` 里，只解析 shader 本身会误报"无显式 vk::binding"。
    """
    if seen is None:
        seen = set()
    p = path.resolve()
    if p in seen or not p.is_file():
        return []
    seen.add(p)
    out = [p]
    text = p.read_text(encoding="utf-8", errors="replace")
    for inc in RE_INCLUDE.findall(text):
        for cand in (p.parent / inc, root / inc):
            if cand.is_file():
                out += include_closure(cand, root, seen)
                break
    return out


def collect_source_bindings(src: Path, root: Path, cache: dict) -> tuple[list[SourceBinding], list[str]]:
    """合并 shader 自身 + 其 include 闭包里的绑定声明（同名以先出现者为准）。"""
    bindings: list[SourceBinding] = []
    push: list[str] = []
    seen_names: set[str] = set()
    for f in include_closure(src, root):
        bs, ps = cache.get(f, ([], []))
        for b in bs:
            if b.name not in seen_names:
                seen_names.add(b.name)
                bindings.append(b)
        for n in ps:
            if n not in push:
                push.append(n)
    return bindings, push


def parse_push_constants(path: Path) -> list[str]:
    """抽取 `[[vk::push_constant]] cbuffer NAME { … }` 的名字（DX 侧没有 push constant）。"""
    lines = path.read_text(encoding="utf-8").splitlines()
    names: list[str] = []
    for i, line in enumerate(lines):
        if "[[vk::push_constant]]" not in line:
            continue
        chunk = line.split("[[vk::push_constant]]", 1)[1]
        j = i
        while "{" not in chunk and ";" not in chunk and j + 1 < len(lines) and j - i < 6:
            j += 1
            chunk += " " + lines[j]
        mc = re.search(r"\bcbuffer\s+(\w+)", chunk)
        if mc:
            names.append(mc.group(1))
            continue
        mn = re.findall(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[;{]", chunk)
        if mn:
            names.append(mn[-1])
    return names


# ---------------------------------------------------------------------------
# 编译 + 反射
# ---------------------------------------------------------------------------

def compile_and_reflect(slangc: str, src: Path, shaders_dir: Path, target: str,
                        stage: str, entry: str, workdir: Path) -> BackendResult:
    """编译一个 shader 到一个后端，并读取反射出的绑定落点。"""
    tag = re.sub(r"[^A-Za-z0-9_.-]", "_", src.name) + "." + target
    out = workdir / (tag + BACKENDS[target][1])
    refl = workdir / (tag + ".json")
    cmd = [slangc, str(src), "-target", BACKENDS[target][0],
           "-entry", entry, "-stage", stage,
           "-I", str(shaders_dir), "-o", str(out),
           "-reflection-json", str(refl), "-Wno-39001"]

    res = BackendResult()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        res.error = "编译超时（120s）"
        return res

    if proc.returncode != 0 or not refl.is_file():
        # 只保留第一行 error，避免把 warning 也带进来
        lines = [l.strip() for l in (proc.stderr or "").splitlines() if l.strip()]
        err_lines = [l for l in lines if "error" in l.lower()]
        if err_lines:
            res.error = err_lines[0]
        else:
            res.error = lines[-1] if lines else "编译失败（无错误输出）"
        return res

    try:
        data = json.loads(refl.read_text(encoding="utf-8"))
    except Exception as e:                      # noqa: BLE001 — 反射文件坏了也算失败
        res.error = f"反射 JSON 解析失败：{e}"
        return res

    for p in data.get("parameters", []):
        b = p.get("binding") or {}
        kind = b.get("kind", "")
        # push constant 没有 (space,index) 意义上的落点，跳过（单独报告）
        if kind in ("pushConstant", "descriptorTableSlot") or kind in KIND_TO_BANK:
            idx = b.get("index")
            if idx is None:
                continue
            res.params[p.get("name", "")] = {
                "kind": kind,
                "space": int(b.get("space", 0) or 0),
                "index": int(idx),
                "count": b.get("count"),
            }
    res.ok = True
    return res


def audit_one(args_tuple) -> ShaderResult:
    """一个 shader 的全部后端（线程池任务）。"""
    (slangc, src, shaders_dir, workdir, cache, targets) = args_tuple
    m = RE_STAGE_FILE.search(src.name)
    ext = m.group(1)
    stage, entry = STAGE_MAP[ext]
    source, push = collect_source_bindings(src, shaders_dir, cache)
    res = ShaderResult(shader=str(src.relative_to(shaders_dir)).replace("\\", "/"),
                       path=src, stage=stage, entry=entry,
                       source=source, push_constants=push)
    for t in targets:
        res.backends[t] = compile_and_reflect(slangc, src, shaders_dir, t, stage, entry, workdir)
    return res


# ---------------------------------------------------------------------------
# 对账
# ---------------------------------------------------------------------------

def analyze(results: list[ShaderResult]) -> tuple[list[dict], list[str], list[str]]:
    """返回 (表格行, 错误清单, 提示清单)。"""
    rows: list[dict] = []
    errors: list[str] = []
    notes: list[str] = []

    for r in results:
        vk = r.backends.get("vk")
        if vk and not vk.ok:
            errors.append(f"[vk 编译失败] {r.shader}: {vk.error}")

        # 每个后端的失败都单独记录（D3D 后端缺能力是正常现象，算提示不算错误）
        for t in ("dx12", "dx11"):
            b = r.backends.get(t)
            if b and not b.ok:
                notes.append(f"[{t} 不支持/编译失败] {r.shader}: {b.error}")

        src_by_name = {s.name: s for s in r.source}

        # ── 1. 源码里有属性、但反射里没有（可能被裁掉，或属性写在了非资源上）──
        # ── 2. Vulkan 契约：源码 (set,binding) vs 反射 (space,index) ──
        used_slots: dict[tuple[int, int], tuple[str, str, bool]] = {}
        if vk and vk.ok:
            for name, b in sorted(vk.params.items()):
                if b["kind"] != "descriptorTableSlot":
                    continue
                src = src_by_name.get(name)
                if not src:
                    errors.append(f"[无显式 vk::binding] {r.shader}: {name} 在源码里找不到绑定属性")
                    continue
                if src.set_value is None or src.binding_value is None:
                    errors.append(f"[常量未定义] {r.shader}:{src.line} {name} 的 "
                                  f"set={src.set_expr} binding={src.binding_expr} 解析失败")
                    continue
                got = (b["space"], b["index"])
                exp = (src.set_value, src.binding_value)
                if got != exp:
                    errors.append(f"[契约不一致] {r.shader}: {name} 源码 "
                                  f"({src.set_expr}={exp[0]}, {src.binding_expr}={exp[1]}) "
                                  f"vs 反射 set={got[0]} binding={got[1]}")
                if exp in used_slots:
                    prev_name, prev_type, prev_array = used_slots[exp]
                    # 「纹理 + 采样器」共用同一槽是合法的：Vulkan 侧是 CombinedImageSampler，
                    # 引擎本来就这么建布局（如 u_GBufferA + u_GBufferSampler 都在 binding 0）。
                    is_tex_sampler_pair = (prev_type == "SamplerState") != (src.type_kw == "SamplerState")
                    # bindless 数组的多个「类型视图」共用同一个描述符数组：本仓库 binding 30
                    # 被 u_Materials[] / u_InstanceTransforms[] / u_BoneMatrices[] 等共同占用
                    # （同一个 unbounded SSBO 数组，按变量类型解释元素）。
                    is_bindless_alias = prev_array and src.is_array
                    if not (is_tex_sampler_pair or is_bindless_alias):
                        errors.append(f"[槽位重复] {r.shader}: (set={exp[0]}, binding={exp[1]}) "
                                      f"被 {prev_name}({prev_type}) 与 {name}({src.type_kw}) 同时占用")
                else:
                    used_slots[exp] = (name, src.type_kw, src.is_array)

        # ── 3. 三列并排 ──
        dx12 = r.backends.get("dx12")
        dx11 = r.backends.get("dx11")
        names = sorted(set(src_by_name) | set(vk.params if vk and vk.ok else {})
                       | set(dx12.params if dx12 and dx12.ok else {})
                       | set(dx11.params if dx11 and dx11.ok else {}))

        for name in names:
            src = src_by_name.get(name)
            row = {
                "shader": r.shader,
                "resource": name,
                "vk_set": src.set_value if src else "",
                "vk_binding": src.binding_value if src else "",
                "dx12_space": "", "dx12_reg": "", "dx12_bank": "",
                "dx11_slot": "", "dx11_bank": "",
                "note": "",
            }
            if dx12 and dx12.ok and name in dx12.params:
                b = dx12.params[name]
                bank = KIND_TO_BANK.get(b["kind"], "?")
                row.update(dx12_space=b["space"], dx12_reg=b["index"], dx12_bank=bank)
            if dx11 and dx11.ok and name in dx11.params:
                b = dx11.params[name]
                bank = KIND_TO_BANK.get(b["kind"], "?")
                row.update(dx11_slot=b["index"], dx11_bank=bank)
                limit = DX11_LIMITS.get(bank)
                if limit is not None and b["index"] >= limit:
                    errors.append(f"[DX11 超限] {r.shader}: {name} 落在 {bank}{b['index']}，"
                                  f"超过 {bank}0–{bank}{limit - 1}")
            if name in r.push_constants:
                row["note"] = "push constant（Vulkan 无 binding；DX 侧降级为 cbuffer）"
            elif row["dx12_reg"] == "" and row["dx11_slot"] == "":
                row["note"] = "仅 Vulkan（D3D 反射里没有，或被裁掉/该后端无此资源）"
            elif src is None:
                row["note"] = "无显式 vk::binding"
            rows.append(row)

        # 源码里有属性但 Vulkan 反射里没有 → 可能被裁掉（信息级）
        if vk and vk.ok:
            for s in r.source:
                if s.name not in vk.params:
                    notes.append(f"[已裁剪] {r.shader}: {s.name}（源码声明了，反射里没有）")

    return rows, errors, notes


def write_report(out_dir: Path, results: list[ShaderResult], rows: list[dict],
                 errors: list[str], notes: list[str]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)

    # ── CSV（机读）──
    with open(out_dir / "contract.csv", "w", encoding="utf-8-sig", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()) if rows else ["shader"])
        w.writeheader()
        w.writerows(rows)

    # ── Markdown（人读）──
    with open(out_dir / "contract.md", "w", encoding="utf-8") as f:
        f.write("# Shader 绑定契约对账（Vulkan / DX12 / DX11）\n\n")
        f.write("> 由 `Tools/shader/binding_audit.py` 生成。Vulkan 列的来源是源码里的\n")
        f.write("> `[[vk::binding(常量, 常量)]]`（契约）；D3D 两列是 Slang 反射出的**实际**落点。\n")
        f.write("> 注意：DX12 的 `space` 与 `register` 都由声明顺序决定，**不保证**等于 Vulkan 的 set/binding。\n\n")
        f.write("| shader | 资源 | vk set | vk binding | dx12 space | dx12 reg | dx11 slot | 备注 |\n")
        f.write("|---|---|---:|---:|---:|---|---:|---|\n")
        for r in rows:
            f.write("| `{shader}` | `{resource}` | {vk_set} | {vk_binding} | {dx12_space} | "
                    "{dx12_bank}{dx12_reg} | {dx11_bank}{dx11_slot} | {note} |\n".format(**r))

        if errors:
            f.write("\n## 契约问题（会导致绑定错位，必须修）\n\n")
            for e in errors:
                f.write(f"- {e}\n")
        if notes:
            f.write("\n## 提示（后端能力差异 / 已裁剪资源）\n\n")
            for n in notes:
                f.write(f"- {n}\n")

    # ── summary.txt ──
    counts: dict[str, int] = {}
    for r in results:
        for t, b in r.backends.items():
            counts.setdefault(t, 0)
            counts[t] += 1 if b.ok else 0
    total = len(results)
    with open(out_dir / "summary.txt", "w", encoding="utf-8") as f:
        f.write(f"入口 shader 总数: {total}\n")
        for t in sorted(counts):
            f.write(f"  {t}: 编译成功 {counts[t]}/{total}\n")
        f.write(f"契约问题: {len(errors)}\n提示: {len(notes)}\n")


def main(argv: list[str]) -> int:
    here = Path(__file__).resolve()
    repo = here.parents[2]                     # Tools/shader/x.py → 仓库根
    ap = argparse.ArgumentParser(description="Shader 绑定契约对账（Vulkan/DX12/DX11）")
    ap.add_argument("--shaders", default=str(repo / "Engine" / "Shader" / "Shaders"),
                    help="Shaders 根目录（默认 Engine/Shader/Shaders）")
    ap.add_argument("--slangc", default=None, help="slangc 路径（默认查 VULKAN_SDK / PATH）")
    ap.add_argument("--targets", default="vk,dx12,dx11", help="要跑的后端，逗号分隔")
    ap.add_argument("--only", default=None, help="只处理路径里包含该子串的 shader")
    ap.add_argument("--limit", type=int, default=0, help="只处理前 N 个入口（调试用）")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2),
                    help="并行编译数")
    ap.add_argument("--out", default=str(repo / "Build" / "shader_audit"),
                    help="报告输出目录")
    ap.add_argument("--no-rhi-mirror", action="store_true", help="跳过 RHI 镜像常量比对")
    args = ap.parse_args(argv)

    shaders_dir = Path(args.shaders).resolve()
    if not shaders_dir.is_dir():
        sys.exit(f"Shaders 目录不存在：{shaders_dir}")
    targets = [t.strip() for t in args.targets.split(",") if t.strip()]
    bad = [t for t in targets if t not in BACKENDS]
    if bad:
        sys.exit(f"未知后端：{bad}（可选 {list(BACKENDS)}）")

    slangc = find_slangc(args.slangc)
    consts = load_constants(shaders_dir)

    errors: list[str] = []
    if not args.no_rhi_mirror:
        errors += check_rhi_mirror(consts, load_rhi_mirror(repo / "Engine/RHI/RHI/Types.h"))

    entries = list(iter_shader_files(shaders_dir))
    if args.only:
        entries = [(f, e) for f, e in entries if args.only in str(f)]
    if args.limit:
        entries = entries[:args.limit]
    if not entries:
        sys.exit("没有匹配的 shader 入口")

    print(f"slangc    : {slangc}")
    print(f"shaders   : {shaders_dir}")
    print(f"后端      : {', '.join(targets)}")
    print(f"入口数    : {len(entries)}（并行 {args.jobs}）")

    workdir = Path(tempfile.mkdtemp(prefix="shader_audit_"))
    # 预解析所有 .slang（含共享头）的绑定声明：线程池里只读缓存，避免重复解析
    cache = {f.resolve(): (parse_shader_source(f, consts), parse_push_constants(f))
             for f in shaders_dir.rglob("*.slang")}
    results: list[ShaderResult] = []
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            tasks = [(slangc, f, shaders_dir, workdir, cache, targets) for f, _ in entries]
            for i, r in enumerate(pool.map(audit_one, tasks), 1):
                results.append(r)
                if i % 20 == 0 or i == len(entries):
                    print(f"  已编译 {i}/{len(entries)}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    rows, more_errors, notes = analyze(results)
    errors += more_errors
    write_report(Path(args.out), results, rows, errors, notes)

    print(f"\n报告：{Path(args.out) / 'contract.md'}")
    print(f"契约问题 {len(errors)} 条，提示 {len(notes)} 条")
    for e in errors[:20]:
        print("  [x] " + e)
    if len(errors) > 20:
        print(f"  …另有 {len(errors) - 20} 条，见报告")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
