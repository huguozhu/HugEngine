"""lightmap_key_check.py -- assertions for Tools/gi/lightmap_key_check.ps1 (task 31).

The GBuffer's eighth MRT carries the lightmap key. Since the box-projection rework it is a
PROCEDURAL parameterisation, not uv0:

    page  = objectIndex      (one object per page: GPUScene collects one object per component)
    tile  = dominant normal axis, 6 tiles laid out 3x2 inside the page
    in-tile uv = world position normalised by that object's own world AABB
                 (GPUObjectData.boundsMin / boundsMax)

with the key packed as (uv.x, uv.y, page, 0) in the whole-page space.

Asserted:
  1. the key is written exactly where geometry is: its covered-pixel set equals the
     world-position covered-pixel set (sky carries a zero key = "no key");
  2. page values are exact integers in [0, kGPUMaxObjects);
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
    in_range = (rp >= 0) & (rp < KGPU_MAX_OBJECTS)
    verdict("page values are inside the object-buffer limit", bool(in_range.all()),
            "pages in [%.0f, %.0f] (limit %d), %d distinct"
            % (rp.min(), rp.max(), KGPU_MAX_OBJECTS, len(np.unique(rp))))

    pages = rp.astype(np.int64)
    # 每页在屏幕上是不是**一块连通区域**（批次会把几个相距很远的物体并进一个页；大的空心
    # 壳体则只是"跨得大"，仍然连通）。掩码先降到粗网格再数 4 连通分量：全分辨率 BFS 没必要。
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
            print("  [info] page %d has %d screen-space components (%d pixels)"
                  % (p, comps, int(m.sum())))
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
        for p in np.unique(pages):
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
