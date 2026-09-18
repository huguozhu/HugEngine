# ============================================================
# RTGI source-independence check (docs section 9.2-I / 10.1 task 3)
#
# Regression test for: "RTGI must not embed DDGI when DDGI is itself a diffuse-stack source".
#
# It runs 06.GILab twice, changing ONLY the diffuse stack, and compares the RTGI provider's
# raw output (registry index 9 = RTGI, quarter-res 480x270):
#
#   A: diffuse = { RTGI }            -> DDGI is NOT a stack source, so the miss fallback
#                                       (DDGI probes) is legitimate: this is the degraded
#                                       path and must stay byte-identical to before.
#   B: diffuse = { DDGI, RTGI }      -> DDGI IS a stack source, so RTGI's misses must NOT
#                                       query DDGI. RTGI's output must therefore equal A's.
#
# PASS = A and B produce the SAME RTGI mean/bytes (source independence restored).
# Before the fix B was 3.5x A (0.2034 vs 0.0575) -- DDGI counted twice.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/rtgi_coupling_check.ps1 [-Frame 30] [-Config Release]
# ============================================================
param(
    [int]$Frame = 30,
    [int]$TimeoutSec = 120,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe    = Join-Path $root "build\bin\$Config\06.GILab.exe"
$outDir = Join-Path $root 'Build\verify'
$baseCfg = Join-Path $root 'Content\Config\06_GILab.cfg'
if (-not (Test-Path $exe))     { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}
function New-Cfg([string]$path, [hashtable]$ov) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($k in $ov.Keys) { $m[$k] = $ov[$k] }
    [System.IO.File]::WriteAllLines($path, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
}

function Run-Case([string]$tag, [string]$cfgPath) {
    $log = Join-Path $outDir "rtgichk_$tag.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    $env:HE_GILAB_CONFIG  = $cfgPath
    $env:HE_DUMP_GI       = "rtgichk$tag"
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $fin = $p.WaitForExit($TimeoutSec * 1000)
    if (-not $fin) { $p.Kill() }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue
    $dump = Join-Path $outDir "gi_rtgichk${tag}_prov9_raw.f16"
    if (-not $fin -or -not (Test-Path $dump)) { throw "case $tag produced no RTGI dump (run crashed or timed out)" }
    # Write-Host, not Write-Output: this function RETURNS the dump path, and anything written
    # to the output stream would be appended to it (that bug made the first version compare
    # garbage and report a false FAIL).
    Write-Host "  case $tag -> $dump"
    return $dump
}

$cfgA = Join-Path $outDir 'rtgichk_A.cfg'
$cfgB = Join-Path $outDir 'rtgichk_B.cfg'
# gi_solo=0 keeps direct lights on, ae_enabled=0 pins auto exposure: without these the two
# runs are NOT comparable (auto exposure rescales the HDR target every run, and an unlit
# screen makes RTGI read ~0 in both cases).
New-Cfg $cfgA @{ gi_blend_diffuse_w1 = '0.000000'; gi_blend_diffuse_w3 = '1.000000'
                 gi_solo = '0'; ae_enabled = '0' }
New-Cfg $cfgB @{ gi_blend_diffuse_w1 = '1.000000'; gi_blend_diffuse_w3 = '1.000000'
                 gi_solo = '0'; ae_enabled = '0' }

Write-Output "RTGI source-independence check (frame $Frame)"
$fileA = Run-Case 'A' $cfgA   # diffuse = { RTGI }
$fileB = Run-Case 'B' $cfgB   # diffuse = { DDGI, RTGI }

$hashA = (Get-FileHash $fileA -Algorithm SHA256).Hash
$hashB = (Get-FileHash $fileB -Algorithm SHA256).Hash

# Compare the MEAN, not the bytes: the two cases legitimately differ per-pixel because the
# ray jitter is seeded from the frame counter, and adding the DDGI pass changes how many
# submits a frame consumes (async compute path) -> different seed, same estimator.
# What must NOT differ is the estimate itself: before the fix B was 3.5x A.
function Get-MeanLum([string]$path) {
    $py = @"
import numpy as np
a = np.fromfile(r'$path', dtype=np.float16).astype(np.float32).reshape(-1, 4)
lum = 0.2126*a[:,0] + 0.7152*a[:,1] + 0.0722*a[:,2]
print('%.9g' % lum.mean())
"@
    $tmp = Join-Path $env:TEMP 'rtgi_mean.py'
    Set-Content -Path $tmp -Value $py -Encoding ASCII
    return [double](python $tmp)
}
$meanA = Get-MeanLum $fileA
$meanB = Get-MeanLum $fileB
$rel   = [math]::Abs($meanA - $meanB) / [math]::Max($meanA, 1e-12)

Write-Output ""
Write-Output ("A (diffuse = RTGI)        mean={0:F9}  sha256={1}" -f $meanA, $hashA.Substring(0,16))
Write-Output ("B (diffuse = DDGI+RTGI)   mean={0:F9}  sha256={1}" -f $meanB, $hashB.Substring(0,16))
Write-Output ("relative mean difference  {0:P3}" -f $rel)
if ($rel -lt 0.001) {
    Write-Output "PASS: RTGI's estimate does not depend on DDGI being a stack source (no double counting)."
    Write-Output "      (per-pixel bytes differ only because the ray-jitter seed follows the frame counter)"
    exit 0
}
Write-Output "FAIL: RTGI depends on whether DDGI is a stack source -> DDGI is embedded in RTGI's misses."
Write-Output "      (before the fix this was a 3.5x mean difference: 0.0575 vs 0.2034)"
exit 1
