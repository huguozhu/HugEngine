# ============================================================
# SSR planar-mirror check (docs section 10.1 task 32 / defect 9.2-W follow-up)
#
# Task 25 proved SSR's rays become valid again, that both march paths are in the same range and
# that Hi-Z is faster -- but NOT that the reflection lands in the right place. A planar mirror
# has a closed-form answer (mirror the object centre through the plane, project that virtual
# image back through the same camera), so "is the reflection where geometry says" becomes a
# pixel-accurate criterion instead of an eyeball check.
#
# WHAT THIS CHECK FOUND (and why it exists)
#   With the mirror rig the reflection was *nowhere near* the prediction: two independent bugs
#   in `GI/SSR.frag.slang`, both invisible to the earlier "validity / same order of magnitude"
#   criteria:
#     1. `reflect(-V, N)` mixed a **view-space** incident direction with a **world-space**
#        GBuffer normal -- the reflection was only right when the camera happened to be aligned
#        with the world axes. Fixed by passing the view matrix and rotating N into view space.
#     2. the depth->view reconstruction and the view->screen projection both skipped the
#        engine's y convention (negative-height viewport => `ndc.y = 1 - 2v`). The two omissions
#        cancelled each other for *positions* but left the reconstructed geometry vertically
#        mirrored: measured `viewPos.y = +70.3` where the truth was `-70.7`, so the reflection
#        direction was wrong. Fixed with the shared `UvToNdc` / `NdcToUv` helpers.
#   It also fixed two scale bugs: the self-intersection offset must exceed the hit tolerance
#   (a fixed 0.1 with an 11.6 unit tolerance made every ray "hit itself"), and the hit
#   tolerance / range / step must follow the scene scale instead of the historical metre-scale
#   defaults (Sponza is 3720 units wide).
#
# RUNS (all on the same rig: mirror slab on y=1700, red and green boxes standing on it)
#   base   : linear march with fine steps  -> the analytic criterion is ASSERTED here
#   jitter : same, camera moved            -> the reflection must follow the analytic shift
#   legacy : metre-scale march defaults    -> negative control: it must find essentially nothing
#   hiz    : default Hi-Z march            -> REPORTED only (it currently misses one reflection;
#                                            recorded as defect 9.2-AE / task 35)
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/ssr_mirror_check.ps1 [-Frame 120] [-Config Release]
# Exit code: 0 = all assertions pass, 1 = at least one failed.
# ============================================================
param(
    [int]$Frame = 120,
    [int]$TimeoutSec = 300,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe     = Join-Path $root "Build\bin\$Config\06.GILab.exe"
$outDir  = Join-Path $root 'Build\verify'
$baseCfg = Join-Path $root 'Content\Config\06_GILab.cfg'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not (Test-Path $exe))     { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}

function Run-Mirror([string]$tag, [hashtable]$overrides) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($w in 0..3) {
        $m["gi_blend_diffuse_w$w"]  = '0.000000'
        $m["gi_blend_specular_w$w"] = '0.000000'
        $m["gi_blend_ao_w$w"]       = '0.000000'
    }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m['gi_blend_diffuse_w0']  = '1.000000'   # IBL diffuse: some ambient on the boxes
    $m['gi_blend_specular_w1'] = '1.000000'   # SSR alone in the specular stack
    $m['gi_solo']    = '0'                    # direct light ON
    $m['ae_enabled'] = '0'
    $m['gi_shadow']  = '0'
    $m['pipeline_mode'] = '1'
    # camera: above the mirror, looking down at the rig (see the sample's HE_SSR_MIRROR block)
    $m['cam_pos_x'] = '0.000000'
    $m['cam_pos_y'] = '1950.000000'
    $m['cam_pos_z'] = '600.000000'
    $m['cam_yaw']   = '0.000000'
    $m['cam_pitch'] = '-0.394400'
    $m['cam_fov']   = '60.000000'
    $m['cam_near']  = '0.100000'
    $m['cam_far']   = '3000.000000'
    foreach ($k in $overrides.Keys) { $m[$k] = $overrides[$k] }
    $cfg = Join-Path $outDir "chk_$tag.cfg"
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log = Join-Path $outDir "chk_$tag.log"
    Get-ChildItem $outDir -Filter "gi_$tag*" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = $tag
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $env:HE_SSR_MIRROR    = '1'
    # pass GPU 时间戳：任务 35 要复核"Hi-Z 仍然比线性便宜"，脚本从 chk_<tag>.log 里读
    $env:HE_PASS_TIMING   = '1'
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "  !! $tag timed out" }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_SSR_MIRROR, Env:\HE_PASS_TIMING -ErrorAction SilentlyContinue
    if (-not (Test-Path (Join-Path $outDir "gi_${tag}_hdr.f16"))) { throw "$tag produced no dump" }
    Write-Output "sampled $tag"
}

# 1) analytic run: linear march, fine steps (step <= tolerance: no tunnelling)
Run-Mirror 'ssrmirror_base' @{
    ssr_use_hiz = '0'; ssr_auto_scale = '0'
    ssr_max_distance = '2000.000000'; ssr_thickness = '2.000000'
    ssr_step_size = '2.000000'; ssr_max_steps = '600.000000'
}
# 2) jitter run: same config, camera moved 40 units along x
Run-Mirror 'ssrmirror_jitter' @{
    ssr_use_hiz = '0'; ssr_auto_scale = '0'
    ssr_max_distance = '2000.000000'; ssr_thickness = '2.000000'
    ssr_step_size = '2.000000'; ssr_max_steps = '600.000000'
    cam_pos_x = '40.000000'
}
# 3) negative control: the historical metre-scale march parameters
Run-Mirror 'ssrmirror_legacy' @{
    ssr_use_hiz = '0'; ssr_auto_scale = '0'
    ssr_max_distance = '50.000000'; ssr_thickness = '0.100000'
    ssr_step_size = '0.500000'; ssr_max_steps = '64.000000'
}
# 4) the default path: Hi-Z hierarchy march with scene-scaled parameters.
#    Task 35 (defect 9.2-AE) made this path ASSERTED too: it must put BOTH reflections on the
#    predicted pixel exactly like the linear reference. It used to miss the green box entirely
#    because the screen-space march reused the screen fraction as the ray parameter (the depth
#    it compared belonged to a different point of the ray).
Run-Mirror 'ssrmirror_hiz' @{
    ssr_use_hiz = '1'; ssr_auto_scale = '1'
}

& python (Join-Path $PSScriptRoot 'ssr_mirror_check.py') $outDir ssrmirror_base ssrmirror_jitter ssrmirror_legacy ssrmirror_hiz
exit $LASTEXITCODE
