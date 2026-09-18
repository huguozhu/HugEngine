# ============================================================
# RSM indirect-light check (docs section 10.2 task 16 / defect 9.2-AA)
#
# Two runs of 06.GILab with an identical config except the diffuse stack:
#   A) empty stack        -> Lighting does not evaluate the RSM VPL sum at all
#   B) diffuse = {RSM}    -> RSM_Indirect pass runs + Lighting samples its result
#
# What this check asserts:
#   1. the dedicated `RSM_Indirect` pass is registered and its GPU time is non-zero
#      (that is the whole point of task 16: the 16-tap VPL sum must no longer be evaluated
#       per full-resolution pixel inside Lighting);
#   2. the `RSM` raster pass is present in the RSM case (a sanity check that case B really has
#      RSM enabled rather than silently skipped);
#   3. TASK 30 -- the RSM chain must produce data at every level (position map holds geometry,
#      normal map covers the same texels, the VPL radiance map is non-empty and of real
#      magnitude, the half-resolution result is non-empty, and the source reaches the HDR).
#      All of that failed for the lifetime of defect 9.2-AA: the pass ran, the cost was paid,
#      and the attachments held nothing but the clear value.
#
# It also reports the pass timings and S_rsm for the record. The timing verdict that is *not*
# here is explained in the .py.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/rsm_indirect_check.ps1 [-Frame 120] [-Config Release]
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
if (-not (Test-Path $exe)) { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}

$failed = 0
foreach ($case in @('none', 'rsm')) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($w in 0..3) {
        $m["gi_blend_diffuse_w$w"]  = '0.000000'
        $m["gi_blend_specular_w$w"] = '0.000000'
        $m["gi_blend_ao_w$w"]       = '0.000000'
    }
    # keep the specular channel non-empty: an empty one triggers defect 9.2-X (a 463-level
    # firefly) and would drown the numbers we are after
    $m['gi_blend_specular_w0'] = '1.000000'
    if ($case -eq 'rsm') { $m['gi_blend_diffuse_rsm'] = '1.000000' }
    else                 { $m['gi_blend_diffuse_rsm'] = '0.000000' }
    $m['gi_rsm_indirect'] = '1'
    $m['gi_shadow']  = '1'
    $m['gi_solo']    = '0'
    $m['ae_enabled'] = '0'
    $name = "rsmind_$case"
    $cfg  = Join-Path $outDir "chk_$name.cfg"
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log  = Join-Path $outDir "chk_$name.log"
    Get-ChildItem $outDir -Filter "gi_$name*" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = $name
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $env:HE_PASS_TIMING   = '1'
    $t0 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "  !! $name timed out" }
    $hdr = Join-Path $outDir "gi_${name}_hdr.f16"
    if (-not (Test-Path $hdr) -or (Get-Item $hdr).LastWriteTime -lt $t0) {
        Write-Output "  !! FAILED: $name produced no fresh dump"
        $failed++
    } else {
        Write-Output "sampled $name"
    }
}
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_PASS_TIMING -ErrorAction SilentlyContinue

$py = Join-Path $PSScriptRoot 'rsm_indirect_check.py'
python $py $outDir
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all RSM indirect checks passed"
exit 0
