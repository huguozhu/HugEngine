# ============================================================
# GI GPU cost report (docs section 3.5 / task 13 evidence)
#
# Runs a few configurations with HE_GI_TIMING=1 and prints each GI item's GPU time. This is
# the tool behind the "measured cost structure" in section 3.5 and the evidence that closed
# task 13 (tile/scissor culling): the numbers show where the headroom actually is.
#
# Configurations (diffuse stack unless noted; direct light on, no auto exposure):
#   ssgi     : diffuse = {SSGI}                     -> the only four-figure item
#   rtgi     : diffuse = {RTGI}                     -> RT dispatch + AS_Build
#   ddgi     : diffuse = {DDGI}                     -> probe-grid cost (task 14 fit the grid to the
#                                                     scene AABB, so the probe count is no longer 256)
#   rtfull   : diffuse = {DDGI,RTGI} + specular {RT reflection} + RT shadow
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/cost_report.ps1 [-Frame 240] [-Config Release]
# ============================================================
param(
    [int]$Frame = 240,
    [int]$TimeoutSec = 300,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe    = Join-Path $root "Build\bin\$Config\06.GILab.exe"
$outDir = Join-Path $root 'Build\verify'
$base   = Join-Path $root 'Content\Config\06_GILab.cfg'
if (-not (Test-Path $exe)) { throw "missing exe: $exe" }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($base)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}

function Run-Case([string]$tag, [hashtable]$ov) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($w in 0..3) {
        $m["gi_blend_diffuse_w$w"]  = '0.000000'
        $m["gi_blend_specular_w$w"] = '0.000000'
        $m["gi_blend_ao_w$w"]       = '0.000000'
    }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m['gi_solo']    = '0'
    $m['ae_enabled'] = '0'
    foreach ($k in $ov.Keys) { $m[$k] = $ov[$k] }
    $cfg = Join-Path $outDir "cost_$tag.cfg"       # private copy: the sample rewrites it on exit
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log = Join-Path $outDir "cost_$tag.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    Get-ChildItem $outDir -Filter "gi_cost_$tag*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = "cost_$tag"
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $env:HE_GI_TIMING     = '1'
    $t0 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill() }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_GI_TIMING -ErrorAction SilentlyContinue
    $hdr = Join-Path $outDir "gi_cost_${tag}_hdr.f16"
    if (-not (Test-Path $hdr)) { throw "$tag produced no dump" }
    if ((Get-Item $hdr).LastWriteTime -lt $t0) { throw "$tag dump is stale" }
    Write-Output "sampled $tag"
}

Run-Case 'ssgi'   @{ 'gi_blend_diffuse_w2' = '1.000000' }
Run-Case 'rtgi'   @{ 'gi_blend_diffuse_w3' = '1.000000' }
Run-Case 'ddgi'   @{ 'gi_blend_diffuse_w1' = '1.000000' }
Run-Case 'rtfull' @{ 'gi_blend_diffuse_w1' = '1.000000'; 'gi_blend_diffuse_w3' = '1.000000'
                     'gi_blend_specular_w2' = '1.000000'; 'gi_shadow' = '2' }

Write-Output ""
Write-Output "=== GI GPU cost (ms, rolling average of the last ~10 rounds) ==="
foreach ($tag in 'ssgi', 'rtgi', 'ddgi', 'rtfull') {
    $log = Join-Path $outDir "cost_$tag.log"
    $last = (Select-String -Path $log -Pattern '\[GI ' | Select-Object -Last 1).Line
    Write-Output ("{0,-8} {1}" -f $tag, ($last -replace '^.*\[GI [^\]]*\]', '').Trim())
}
Write-Output ""
Write-Output "Reading: only items that actually ran are non-zero (a source outside the stack has no"
Write-Output "pass; the IBL bake only runs when the skybox changes). 0 therefore means 'did not run'."
