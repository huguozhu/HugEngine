# ============================================================
# Lightmap key check (docs section 10.1 task 31 / defect list)
#
# Task 31 (make a lightmap real) has a hard prerequisite recorded back in task 18: a PER-PIXEL
# lightmap key. A lightmap stores one value per (object, texel), so the lighting pass must be
# able to look up "which page and which texel" for every pixel; world position cannot do that
# (one world point can appear in several UV islands) and neither can any single existing GBuffer
# channel. This check verifies the eighth GBuffer MRT that carries the key:
#
#     R = uv0.x, G = uv0.y, B = objectIndex (page), A = unused
#
# Asserted here (properties that hold for the key channel itself):
#   * the key is written exactly where geometry is (its coverage equals the world-position
#     coverage -- sky pixels carry a zero key, which the lighting pass reads as "no key");
#   * page values are exact integers and stay below the object-buffer limit (kGPUMaxObjects);
#   * the page really identifies ONE object: the world positions sharing a page must fall in a
#     compact box (per-page AABB diagonal well below the scene diagonal).
#
# REPORTED here (the open requirement, see the [KNOWN] block): uv0 is a TILING texture
# coordinate, not a lightmap unwrap, so it does not uniquely identify a surface point. The
# script measures the per-page texel collision rate at a candidate bake resolution and prints
# it. Measured on the 06.GILab scene: 70.60% of the covered texels at 128x128 are hit by two or
# more distinct world points (uv range spans [-1.42, 28.97], only 67% of pixels are inside
# [0,1]). That is why the remaining work is a PROCEDURAL per-object unwrap (dominant-axis box
# projection into 6 tiles of a page) rather than uv0 -- recorded in the plan document.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/lightmap_key_check.ps1 [-Frame 120] [-Config Release]
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
$tag     = 'lmkey'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not (Test-Path $exe))     { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$m = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $m[$Matches[1]] = $Matches[2] }
}
$m['pipeline_mode'] = '1'      # Deferred: the key is a GBuffer attachment
$m['gi_solo']       = '0'      # direct light stays on (only used as a scene to key)
$m['ae_enabled']    = '0'
$cfg = Join-Path $outDir "chk_$tag.cfg"
[System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))

$log = Join-Path $outDir "chk_$tag.log"
Get-ChildItem $outDir -Filter "gi_$tag*" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
$env:HE_GILAB_CONFIG  = $cfg
$env:HE_DUMP_GI       = $tag
$env:HE_DUMP_GI_FRAME = "$Frame"
$t0 = Get-Date
$p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                   -RedirectStandardOutput $log -RedirectStandardError "$log.err"
if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "  !! $tag timed out" }
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

$key = Join-Path $outDir "gi_${tag}_gb_lightmapkey.f16"
if (-not (Test-Path $key) -or (Get-Item $key).LastWriteTime -lt $t0) {
    Write-Output "!! FAILED: no fresh lightmap-key dump (did the sample crash? see chk_$tag.log)"
    exit 1
}
Write-Output "sampled $tag"

python (Join-Path $PSScriptRoot 'lightmap_key_check.py') $outDir $tag
if ($LASTEXITCODE -ne 0) { exit 1 }
Write-Output ""
Write-Output "all lightmap-key checks passed"
exit 0
