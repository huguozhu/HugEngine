#!/usr/bin/env python3
# ============================================================
#  Tools/MeshPreprocess/process_meshes.py
#
#  T6（《01.Nanite离线预处理与全Mesh管线方案》）：构建期批量生成 `Content/Processed/*.nanite`。
#
#  【增量判据（方案 §5.4 的三重哈希）】
#    ① builderVersion + settingsHash：由工具 `--print-settings-hash` 给出，写在
#       `Processed/.process_stamp`；一旦变化 ⇒ 清空缓存全量重建（算法或参数改了，旧产物作废）。
#    ② sourceHash：源资产指纹（glTF 及其同目录的 .bin/.jpg 的最大 mtime + 大小）。
#       单个 glTF 变了只重建它自己。
#    ③ 产物存在性：清单里记录的 .nanite 文件缺失 ⇒ 重建该资产。
#
#  【失败不阻塞构建】任何一次工具调用失败都只打警告并继续（运行时回退运行时构建）；
#  脚本自身的退出码恒为 0，除非参数非法。
#
#  【源缺失空跑】`Content/gltf` 不入库，CI 上没有源资产时打印一行说明并成功退出。
# ============================================================

import argparse
import hashlib
import json
import os
import subprocess
import sys

try:                                  # 控制台编码兜底：脚本绝不能因打印中文/箭头而崩溃
    sys.stdout.reconfigure(errors="replace")   # 保留控制台自身编码（GBK 控制台也能读中文）
    sys.stderr.reconfigure(errors="replace")
except (AttributeError, ValueError):
    pass

MANIFEST_NAME = ".process_manifest.json"
STAMP_NAME = ".process_stamp"


def log(message):
    print(f"[ProcessMeshes] {message}", flush=True)


def read_stamp(tool):
    """从工具拿 builderVersion + settingsHash（唯一真值在工具里，脚本不复制常量）"""
    try:
        out = subprocess.run([tool, "--print-settings-hash"], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        log(f"警告：无法读取工具参数指纹（{exc}），跳过离线预处理")
        return None
    line = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else ""
    return line if line else None


def source_signature(gltf_path):
    """源指纹：glTF 自身 + 同目录所有资源的 (路径, 大小, mtime) 摘要。

    【为什么整目录】glTF 的几何在 .bin、材质在 .jpg/.png；只盯 .gltf 会把
    "换了 .bin 但 .gltf 没变"的情况漏掉。只 stat 不读内容，大目录也很快。
    """
    digest = hashlib.sha256()
    base = os.path.dirname(os.path.abspath(gltf_path))
    entries = []
    for name in sorted(os.listdir(base)):
        path = os.path.join(base, name)
        try:
            stat = os.stat(path)
        except OSError:
            continue
        if os.path.isfile(path):
            entries.append((name, stat.st_size, int(stat.st_mtime)))
    for name, size, mtime in entries:
        digest.update(f"{name}|{size}|{mtime}\n".encode("utf-8"))
    return digest.hexdigest()


def collect_sources(content_dir):
    gltf_dir = os.path.join(content_dir, "gltf")
    found = []
    for root, _dirs, files in os.walk(gltf_dir):
        for name in sorted(files):
            if name.lower().endswith(".gltf"):
                found.append(os.path.join(root, name))
    return sorted(found)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, help="MeshPreprocess 可执行文件")
    parser.add_argument("--content", required=True, help="Content 目录")
    parser.add_argument("--granularity", default="mesh", choices=["primitive", "mesh"])
    parser.add_argument("--force", action="store_true", help="忽略缓存全量重建")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if not os.path.isfile(args.tool):
        log(f"警告：工具不存在（{args.tool}），跳过离线预处理")
        return 0

    out_dir = os.path.join(args.content, "Processed")
    os.makedirs(out_dir, exist_ok=True)
    manifest_path = os.path.join(out_dir, MANIFEST_NAME)
    stamp_path = os.path.join(out_dir, STAMP_NAME)

    stamp = read_stamp(args.tool)
    if stamp is None:
        return 0

    # ── ① 三重哈希之一：工具参数变了 ⇒ 旧产物全部作废 ──
    previous_stamp = None
    if os.path.isfile(stamp_path):
        with open(stamp_path, "r", encoding="utf-8") as handle:
            previous_stamp = handle.read().strip().lstrip("\ufeff")
    if args.force or previous_stamp != stamp:
        if previous_stamp is not None:
            log(f"参数指纹变化（{previous_stamp} → {stamp}）⇒ 清空 Processed 全量重建")
        for name in os.listdir(out_dir):
            if name.endswith(".nanite"):
                os.remove(os.path.join(out_dir, name))
        with open(stamp_path, "w", encoding="utf-8") as handle:
            handle.write(stamp)

    manifest = {}
    if os.path.isfile(manifest_path):
        try:
            with open(manifest_path, "r", encoding="utf-8") as handle:
                manifest = json.load(handle)
        except (OSError, json.JSONDecodeError):
            manifest = {}

    sources = collect_sources(args.content)
    if not sources:
        log("Content/gltf 下没有源资产（源不入库）⇒ 空跑成功")
        return 0

    rebuilt = 0
    skipped = 0
    failed = 0
    for gltf_path in sources:
        rel = os.path.relpath(gltf_path, args.content).replace("\\", "/")
        signature = source_signature(gltf_path)
        entry = manifest.get(rel)
        outputs = entry.get("outputs", []) if isinstance(entry, dict) else []
        up_to_date = (
            not args.force
            and isinstance(entry, dict)
            and entry.get("signature") == signature
            and outputs
            and all(os.path.isfile(os.path.join(out_dir, name)) for name in outputs)
        )
        if up_to_date:
            skipped += 1
            continue

        # ── ② 只重建这一个资产 ──
        for name in outputs:   # 旧产物先删掉：避免同名残留让"存在性"判断失真
            path = os.path.join(out_dir, name)
            if os.path.isfile(path):
                os.remove(path)

        before = set(os.listdir(out_dir))
        command = [args.tool, gltf_path, out_dir, f"--granularity={args.granularity}"]
        if not args.quiet:
            command.append("--stats")
        result = subprocess.run(command, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")   # 工具输出含中文，按 UTF-8 解，坏字节替换
        after = set(os.listdir(out_dir))
        produced = sorted(name for name in (after - before) if name.endswith(".nanite"))

        if result.returncode != 0 or not produced:
            failed += 1
            log(f"警告：{rel} 预处理失败（工具退出码 {result.returncode}），运行时将回退运行时构建")
            for line in (result.stderr or "").strip().splitlines()[:3]:
                log(f"    {line}")
            manifest.pop(rel, None)
            continue

        manifest[rel] = {"signature": signature, "outputs": produced, "stamp": stamp}
        rebuilt += 1

    with open(manifest_path, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True, ensure_ascii=False)

    if rebuilt == 0 and failed == 0 and not args.force:
        log(f"全部命中缓存（{skipped} 个资产，零工作量）")
    else:
        log(f"完成：重建 {rebuilt} 个，命中 {skipped} 个，失败 {failed} 个")
    return 0


if __name__ == "__main__":
    # 【失败不阻塞构建】脚本自身异常也只打警告并以 0 退出（运行时回退运行时构建）
    try:
        sys.exit(main())
    except Exception as exc:   # noqa: BLE001 —— 这里就是要吞掉一切，保证构建不断
        log(f"警告：离线预处理脚本异常（{exc}），本次跳过")
        sys.exit(0)
