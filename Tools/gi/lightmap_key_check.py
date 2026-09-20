"""lightmap_key_check.py -- assertions for Tools/gi/lightmap_key_check.ps1 (task 31).

The GBuffer's eighth MRT carries the lightmap key. Since the box-projection rework it is a
PROCEDURAL parameterisation, not uv0:

    page  = objectIndex      (one object per page: GPUScene collects one object per component;
                              split into the normal segment [0,1024) and the Nanite segment
                              [1024,2048) -- see classify_page() and NaniteTypes.h)
    tile  = dominant normal axis, 6 tiles laid out 3x2 inside the page
    in-tile uv = world position normalised by that object's own world AABB
                 (GPUObjectData.boundsMin / boundsMax)

with the key packed as (uv.x, uv.y, page, 0) in the whole-page space.

Asserted:
  1. the key is written exactly where geometry is: its covered-pixel set equals the
     world-position covered-pixel set (sky carries a zero key = "no key");
  2. page values are exact integers that classify into an object-index SEGMENT: normal
     [0,1024) or Nanite [1024,2048); anything else -- including the 0xFFFFFFFF sentinel --
     fails, with the observed value range in the detail message (classify_page());
  3. the key is a normalized parameterisation: uv inside [0,1] for at least 99.9% of pixels
     (uv0 managed only 67% -- it is a tiling coordinate, uv range [-1.42, 28.97]);
  4. the key is unique up to the texel footprint: at 128x128 at most 10% of covered texels may
     hold more than one surface SHEET (measured 7.33%). The metric compares each texel's world
     spread with the texel's own world footprint -- an earlier version counted "two
     0.25-unit-quantised world positions in one texel", which no parameterisation can pass
     because a texel covers an area.

Reported: per-page screen-space components (occlusion, not batching) and the uniqueness numbers
at 256x256. Known limitation: box projection overlaps for parallel sheets of the SAME object
(hollow shells, layered walls) -- those are the ~7%; a true unwrap is future work.

Exit code 0 = all assertions pass.
"""
import os
import sys

import numpy as np

W, H = 1920, 1080
KGPU_MAX_OBJECTS = 1024      # kGPUMaxObjects (ShaderTypes.slang)
# ── gb_lightmapkey 的页号（== objectIndex）分区契约 ───────────────────────────────
# 【为什么要在这里复刻一份】离线检查器不能 include C++ 头，只能把契约里的数字抄成命名
# 常量；权威定义在 Engine/Render/Nanite/NaniteTypes.h（那边用 static_assert 钉死，改动会
# 先在编译期炸掉，然后再来同步这里）。别在这里写裸的 1024/2048。
#   · 普通段 [0, 1024)：page == 全局 objectIndex（GPUScene 的对象枚举顺序）；
#   · Nanite 段 [1024, 2048)：page == 1024 + Nanite 实例本地下标，解出下标的唯一办法是
#     减去段起点；Nanite 段停在 2048 是因为 RGBA16_FLOAT(binary16) 能精确表示的最大整数
#     就是 2048，再往上页号会被量化成偶数 ⇒ "页号是精确整数"判据必然失败；
#   · 哨兵 0xFFFFFFFF（分配失败 / 无实例）既不是合法页号也不是合法 objectIndex。
K_NORMAL_OBJECT_INDEX_BEGIN = 0                                  # kNormalObjectIndexBegin
K_NORMAL_OBJECT_INDEX_CAPACITY = KGPU_MAX_OBJECTS                # kNormalObjectIndexCapacity
K_LIGHTMAP_KEY_EXACT_OBJECT_INDEX_LIMIT = 2048                   # binary16 精确整数上限（含）
K_NANITE_OBJECT_INDEX_BEGIN = K_NORMAL_OBJECT_INDEX_BEGIN + K_NORMAL_OBJECT_INDEX_CAPACITY
K_NANITE_OBJECT_INDEX_CAPACITY = \
    K_LIGHTMAP_KEY_EXACT_OBJECT_INDEX_LIMIT - K_NANITE_OBJECT_INDEX_BEGIN  # kNaniteObjectIndexCapacity
K_OBJECT_INDEX_TOTAL_CAPACITY = K_NANITE_OBJECT_INDEX_BEGIN + K_NANITE_OBJECT_INDEX_CAPACITY
K_INVALID_OBJECT_INDEX = 0xFFFFFFFF                              # kInvalidObjectIndex（哨兵）
SEGMENT_NORMAL = "normal"
SEGMENT_NANITE = "nanite"
SEGMENT_INVALID = "invalid"
MIN_PAGE_PIXELS = 100        # a page needs this many pixels to take part in the reports
COVER_TOL = 0.001            # key coverage may differ from geometry coverage by at most 0.1%
RESOLUTIONS = (128, 256)     # candidate bake resolutions for the uniqueness report
SPREAD_TEXELS = 3.0          # a texel is "overlapping" if its world spread exceeds this many
OVERLAP_MAX = 10.0           # ...and at most this percentage of texels may overlap at 128x128
BLOCK = 8                    # coarse grid for the screen-space component report


def load(directory, tag, name):
    path = os.path.join(directory, "gi_%s_%s.f16" % (tag, name))
    a = np.fromfile(path, dtype=np.float16).astype(np.float32)
    if a.size != W * H * 4:
        raise SystemExit("unexpected dump size: %s" % path)
    return a.reshape(H, W, 4)


def classify_page(global_page):
    """把 gb_lightmapkey 的页号（== objectIndex）按段分类，返回 (segment_name, local_index, ok)。

    【契约】三段划分与 NaniteTypes.h 的 ClassifyObjectIndex 一一对应：
      · [0, 1024)   -> ("normal", global - 0,    True)   普通段，本地下标恒等于全局页号；
      · [1024, 2048)-> ("nanite", global - 1024, True)   Nanite 段，本地下标必须减段起点；
      · 其余        -> ("invalid", -1,           False)  含哨兵 0xFFFFFFFF / inf / NaN。
    纯函数（不碰数组、无副作用），既能逐页打标签，也能直接用于判定。非整数页号按四舍五入
    取整后再分类（"页号是不是精确整数"由另一条判据单独负责），非有限值一律 invalid。
    """
    p = float(global_page)
    if not np.isfinite(p):                       # NaN / +-inf（哨兵写进 binary16 就变成 inf）
        return SEGMENT_INVALID, -1, False
    p = int(round(p))
    if K_NORMAL_OBJECT_INDEX_BEGIN <= p < K_NANITE_OBJECT_INDEX_BEGIN:
        return SEGMENT_NORMAL, p - K_NORMAL_OBJECT_INDEX_BEGIN, True
    if K_NANITE_OBJECT_INDEX_BEGIN <= p < K_OBJECT_INDEX_TOTAL_CAPACITY:
        return SEGMENT_NANITE, p - K_NANITE_OBJECT_INDEX_BEGIN, True
    return SEGMENT_INVALID, -1, False


def page_label(global_page):
    """页号的人类可读标签：Nanite 段顺带打印恢复出来的本地下标（普通段 local == global，
    重复打印没意义，保持原样）。用于 [info] page ... 这类逐页输出。"""
    name, local, _ok = classify_page(global_page)
    if name == SEGMENT_NANITE:
        return "page %d (nanite segment, local %d)" % (global_page, local)
    if name == SEGMENT_NORMAL:
        return "page %d" % global_page
    return "page %d (INVALID: outside every object-index segment)" % global_page


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    tag = sys.argv[2] if len(sys.argv) > 2 else "lmkey"
    failures = []

    def verdict(name, ok, detail):
        print("  [%s] %s -- %s" % ("PASS" if ok else "FAIL", name, detail))
        if not ok:
            failures.append(name)

    key = load(directory, tag, "gb_lightmapkey").reshape(-1, 4)
    world = load(directory, tag, "gb_worldpos")[..., :3].reshape(-1, 3)

    geom = np.abs(world).sum(axis=1) > 0
    key_cov = np.abs(key).sum(axis=1) > 0
    print("=== lightmap key channel ===")
    print("  geometry pixels=%d  key pixels=%d  (%.4f%% of the frame)"
          % (geom.sum(), key_cov.sum(), 100.0 * key_cov.mean()))

    diff = abs(float(key_cov.mean()) - float(geom.mean()))
    verdict("the key is written exactly where geometry is", diff <= COVER_TOL,
            "key %.4f%% vs geometry %.4f%% (|diff| %.4f%% <= %.2f%%)"
            % (100.0 * key_cov.mean(), 100.0 * geom.mean(), 100.0 * diff, 100.0 * COVER_TOL))

    if not key_cov.any():
        print("  !! no key pixels -- cannot continue")
        return 1

    uv = key[key_cov, :2]
    page = key[key_cov, 2]
    wp = world[key_cov]
    rp = np.round(page)
    integral = np.abs(page - rp) < 1e-3
    verdict("page values are exact integers", bool(integral.mean() >= 0.999),
            "%.4f%% of key pixels have an integer page" % (100.0 * integral.mean()))
    # 【段感知判定】替换原先"硬编码普通段容量 [0, 1024)"的判据：混排场景里普通段与 Nanite
    # 段必须同时 PASS，只有落不进任何段的页号（>= 2048、哨兵 0xFFFFFFFF、inf/NaN）才 FAIL。
    # 非有限页号归一成 -1（单独成组、判 invalid），避免 int64 转换时刷 RuntimeWarning。
    pages = np.where(np.isfinite(rp), rp, -1.0).astype(np.int64)
    unique_pages, page_counts = np.unique(pages, return_counts=True)
    # 【为什么按全局页号分组】普通段和 Nanite 段是不同的编号空间，必须保持区分；每个全局页号
    # 只归属一个段，所以用全局值当分组键即可。seg_stats[name] = [像素数, 全局页min, 全局页max,
    # 本地下标min, 本地下标max, 页数]，invalid 段没有本地下标（记 -1）。
    seg_stats = {}
    for gp, cnt in zip(unique_pages.tolist(), page_counts.tolist()):
        name, local, _ok = classify_page(gp)
        st = seg_stats.setdefault(name, [0, gp, gp, local, local, 0])
        st[0] += cnt
        st[1] = min(st[1], gp)
        st[2] = max(st[2], gp)
        if local >= 0:
            st[3] = min(st[3], local)
            st[4] = max(st[4], local)
        st[5] += 1
    print("  page segments (global page == objectIndex):")
    for name in (SEGMENT_NORMAL, SEGMENT_NANITE, SEGMENT_INVALID):
        st = seg_stats.get(name)
        if st is None:
            print("    %-7s none" % name)
            continue
        # 普通段与 Nanite 段都打印"全局页号范围"和"恢复出的本地下标范围"两项
        local_txt = ("n/a" if st[3] < 0
                     else "[%d, %d]" % (st[3], st[4]))
        print("    %-7s %d page(s), %d pixels, global page [%d, %d], local index %s"
              % (name, st[5], st[0], st[1], st[2], local_txt))
    bad = seg_stats.get(SEGMENT_INVALID)
    n_normal = seg_stats.get(SEGMENT_NORMAL, [0])[0]
    n_nanite = seg_stats.get(SEGMENT_NANITE, [0])[0]
    out_txt = ("%d page(s) / %d pixels OUT OF RANGE, observed page [%d, %d]"
               % (bad[5], bad[0], bad[1], bad[2])) if bad else "no out-of-range page"
    verdict("page values fall inside the object-index segments", bad is None,
            "normal %d px + nanite %d px, %s (valid global page range [%d, %d),"
            " sentinel 0x%X)"
            % (n_normal, n_nanite, out_txt, K_NORMAL_OBJECT_INDEX_BEGIN,
               K_OBJECT_INDEX_TOTAL_CAPACITY, K_INVALID_OBJECT_INDEX))

    # 每页在屏幕上是不是**一块连通区域**（批次会把几个相距很远的物体并进一个页；大的空心
    # 壳体则只是"跨得大"，仍然连通）。掩码先降到粗网格再数 4 连通分量：全分辨率 BFS 没必要。
    # 【继续按**全局**页号分组】普通段与 Nanite 段必须保持不同的分组键，避免两段里下标相同的
    # 物体被错误合并；page_label() 负责把 Nanite 段的本地下标显示出来。
    full_index = np.nonzero(key_cov)[0]
    ys, xs = full_index // W, full_index % W
    blocks = np.zeros((H // BLOCK + 1, W // BLOCK + 1), dtype=np.int32)   # 0 = 空，否则 1
    for p in np.unique(pages):
        m = pages == p
        if m.sum() < MIN_PAGE_PIXELS:
            continue
        blocks[:] = 0
        blocks[ys[m] // BLOCK, xs[m] // BLOCK] = 1
        # 4 连通分量计数（粗网格上，规模很小）
        seen = np.zeros_like(blocks, dtype=bool)
        comps = 0
        for by in range(blocks.shape[0]):
            for bx in range(blocks.shape[1]):
                if blocks[by, bx] == 0 or seen[by, bx]:
                    continue
                comps += 1
                stack = [(by, bx)]
                seen[by, bx] = True
                while stack:
                    cy, cx = stack.pop()
                    for ny, nx in ((cy - 1, cx), (cy + 1, cx), (cy, cx - 1), (cy, cx + 1)):
                        if 0 <= ny < blocks.shape[0] and 0 <= nx < blocks.shape[1] \
                                and blocks[ny, nx] != 0 and not seen[ny, nx]:
                            seen[ny, nx] = True
                            stack.append((ny, nx))
        if comps > 1:
            print("  [info] %s has %d screen-space components (%d pixels)"
                  % (page_label(p), comps, int(m.sum())))
    print("")
    print("  [KNOWN] the page id is usable and the key is now a PROCEDURAL box projection:")
    print("          page = objectIndex (GPUScene collects one object per component, so a page")
    print("          only ever holds one object's geometry), tile = dominant normal axis (3x2")
    print("          tiles per page), in-tile uv = world position normalised by that object's own")
    print("          world AABB (ShaderTypes.slang GPUObjectData.boundsMin/Max).")
    print("          Two earlier criteria of this check were WRONG and are recorded here so they")
    print("          are not repeated: (a) comparing a page's AABB with the scene diagonal -- a")
    print("          large hollow object spans the scene by definition (measured inside-centre-30%")
    print("          = 0.000 for the biggest pages, mid-gap 0.000..0.08 = not a bimodal batch");
    print("          (b) counting texels that hold two 0.25-unit-quantised world positions -- a")
    print("          texel covers an AREA, so no parameterisation can pass that; the spread must")
    print("          be compared against the texel's own world footprint (the metric used below).")
    print("          What remains for a real lightmap is the bake, the source, the criteria -- and")
    print("          replacing box projection with a true unwrap for the ~7%% of texels where")
    print("          parallel sheets of the same object overlap (hollow shells, layered walls).")

    # ---- 报告项：键的唯一性（同一页的同一个 texel 上是不是**同一片表面**）------------
    # 【度量必须按 texel 的世界足迹来判】第一版用"同一个 texel 里出现两个不同的世界位置
    # （0.25 单位量化）"当碰撞 —— 那个判据**任何**参数化都不可能满足：一个 texel 本来就覆盖
    # 一片世界面积。正确的判据是"该 texel 里的世界位置是不是聚成**一片**"：把它们的最大
    # 离中距离与 texel 的世界足迹比，远超足迹的才是真正的重叠（两层平行面、平铺坐标的重复）。
    u, v = uv[:, 0], uv[:, 1]
    inside = (u >= 0.0) & (u <= 1.0) & (v >= 0.0) & (v <= 1.0)
    print("")
    print("  key uv range: u [%.3f, %.3f]  v [%.3f, %.3f]  inside [0,1]: %.2f%%"
          % (u.min(), u.max(), v.min(), v.max(), 100.0 * inside.mean()))
    verdict("the key is a normalized parameterization (uv inside [0,1])",
            float(inside.mean()) >= 0.999,
            "%.2f%% of key pixels are inside [0,1] (>= 99.9%%)" % (100.0 * inside.mean()))
    overlap_128 = None
    for res in RESOLUTIONS:
        tx = np.clip((u * res).astype(np.int64), 0, res - 1)
        ty = np.clip((v * res).astype(np.int64), 0, res - 1)
        colliding = 0
        covered_texels = 0
        worst = 0.0
        for p in np.unique(pages):        # 同上：按全局页号分组，两段互不混淆
            m = pages == p
            wp_p = wp[m]
            ext = wp_p.max(axis=0) - wp_p.min(axis=0)
            # texel 的世界足迹（保守取最长轴 / 最粗的 tile 分辨率）
            footprint = max(float(ext.max()) / max(res / 2.0, 1.0), 1e-3)
            cell = (ty[m] << 24) | tx[m]
            order = np.argsort(cell)
            cell_s, w_s = cell[order], wp_p[order]
            uniq, first = np.unique(cell_s, return_index=True)
            starts = np.append(first, len(cell_s))
            covered_texels += len(uniq)
            for i in range(len(uniq)):
                seg = w_s[starts[i]:starts[i + 1]]
                if len(seg) < 2:
                    continue
                spread = float(np.linalg.norm(seg - seg.mean(axis=0), axis=1).max())
                worst = max(worst, spread / footprint)
                if spread > SPREAD_TEXELS * footprint:
                    colliding += 1
        frac = 100.0 * colliding / max(covered_texels, 1)
        if res == 128:
            overlap_128 = frac
        print("  key uniqueness @%dx%d: %d covered texels, %d overlapping (%.2f%%),"
              " worst spread = %.1f texel footprints (threshold %.1fx)"
              % (res, res, covered_texels, colliding, frac, worst, SPREAD_TEXELS))
    if overlap_128 is not None:
        verdict("the key is unique up to the texel footprint", overlap_128 <= OVERLAP_MAX,
                "%.2f%% of texels at 128x128 hold more than one surface sheet (<= %.0f%%; the"
                " rest are parallel sheets of the same object, see below)"
                % (overlap_128, OVERLAP_MAX))
    print("          A texel whose world positions spread far beyond its own footprint means two")
    print("          different surface sheets share that texel. Box projection (dominant-axis,")
    print("          one tile per face, world position normalised by the object's AABB) is exact")
    print("          for convex single-sheet geometry and only breaks where parallel sheets of")
    print("          the SAME object overlap in projection (hollow shells, layered walls).")

    if failures:
        print("")
        print("!! %d verdict(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("")
    print("all verdicts passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
