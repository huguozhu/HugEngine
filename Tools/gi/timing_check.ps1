# ============================================================
# GI GPU timing readout check (docs task 29 / section 9.2-Z)
#
# The defect this guards: GIDebugData::avgRenderTimeMs had NO assignment anywhere in the
# repository, so the panel's four "per-source ms" rows were always 0.00 -- a readout that
# looks usable and always answers zero. A perf task naturally reads it and concludes nothing.
#
# The assertions therefore test that the readout is REAL, i.e. that it responds to work:
#   1. a source that runs reports a non-zero time, and a source that is not in the stack
#      reports exactly 0 (it has no pass);
#   2. scaling the work scales the reading: SSGI with 64 samples must be clearly above the
#      same run with 16 samples (same everything else);
#   3. repeatability: two identical runs agree within a documented spread.
#
# It deliberately does NOT assert that DDGI amortisation shows up in the reading: measured,
# the probe update costs ~0.02 ms (vs SSGI 0.43 ms and the IBL bake 3.7 ms), so at this grid
# size the amortisation benefit is below the noise floor -- see the docs' task 12 notes.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/timing_check.ps1 [-Frame 240] [-Config Release]
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
    foreach ($w in 0..3) { $m["gi_blend_diffuse_w$w"] = '0.000000' }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m['gi_solo']    = '0'
    $m['ae_enabled'] = '0'
    foreach ($k in $ov.Keys) { $m[$k] = $ov[$k] }

    $cfg = Join-Path $outDir "timing_$tag.cfg"     # private copy: the sample rewrites it on exit
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log = Join-Path $outDir "timing_$tag.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    Get-ChildItem $outDir -Filter "gi_timing_$tag*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = "timing_$tag"
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $env:HE_GI_TIMING     = '1'
    $t0 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill() }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_GI_TIMING -ErrorAction SilentlyContinue

    $hdr = Join-Path $outDir "gi_timing_${tag}_hdr.f16"
    if (-not (Test-Path $hdr)) { throw "$tag produced no dump" }
    if ((Get-Item $hdr).LastWriteTime -lt $t0) { throw "$tag dump is stale" }
    Write-Output "sampled $tag"
}

# SSGI in the stack at two sample counts, then a config without SSGI at all
Run-Case 'ssgi16'   @{ 'gi_blend_diffuse_w2' = '1.000000'; 'ssgi_samples' = '16' }
Run-Case 'ssgi64'   @{ 'gi_blend_diffuse_w2' = '1.000000'; 'ssgi_samples' = '64' }
Run-Case 'ssgi16b'  @{ 'gi_blend_diffuse_w2' = '1.000000'; 'ssgi_samples' = '16' }
Run-Case 'nossgi'   @{ 'gi_blend_diffuse_w1' = '1.000000'; 'ssgi_samples' = '16' }

& python (Join-Path $PSScriptRoot 'timing_check.py') $outDir
exit $LASTEXITCODE
