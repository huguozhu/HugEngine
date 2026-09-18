# ============================================================
# DDGI temporal amortisation check (docs section 10.1 task 12 / AMORTIZE)
#
# The probe update is "every frame, all probes": gridX*gridY*gridZ probes x numSamples rays.
# Amortisation turns it into "every frame, 1/N of the probes" (probeIndex % N == phase),
# relying on the existing temporal blend (blendAlpha) so that each probe still walks the same
# time constant -- just over N times as many frames. Probes that are not due must INHERIT their
# previous value; since the two probe buffers ping-pong every frame, that means copying the
# history buffer into the current one (if that copy is missing, every skipped probe rolls back
# two frames and the field never converges).
#
# The criterion is therefore "equal work -> equal result":
#   S(N, N*k)  ~=  S(1, k)        (same number of probe updates, same blend schedule)
# plus "convergence is merely slower":  S(N, N*k) ~= S(1, k)  for several k.
# S = mean luminance of (HDR{variant} - HDR{none}), i.e. the source's contribution.
#
# Every run gets its OWN config copy: the sample rewrites HE_GILAB_CONFIG on exit, so reusing
# one file makes the next run read the previous run's effective configuration (docs 11.3).
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/amortize_check.ps1 [-Config Release]
# ============================================================
param(
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

function Run-Case([int]$stride, [int]$frame, [string]$variant) {
    $tag = "amort_s${stride}_f${frame}_$variant"
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($w in 0..3) { $m["gi_blend_diffuse_w$w"] = '0.000000' }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m['gi_solo']    = '0'
    $m['ae_enabled'] = '0'
    $m['ddgi_update_stride'] = "$stride"
    if ($variant -eq 'ddgi') { $m['gi_blend_diffuse_w1'] = '1.000000' }

    $cfg = Join-Path $outDir "$tag.cfg"     # private copy: never reused by another run
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log = Join-Path $outDir "$tag.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    Get-ChildItem $outDir -Filter "gi_$tag*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = $tag
    $env:HE_DUMP_GI_FRAME = "$frame"
    $t0 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill() }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

    $hdr = Join-Path $outDir "gi_${tag}_hdr.f16"
    if (-not (Test-Path $hdr)) { throw "$tag produced no dump" }
    if ((Get-Item $hdr).LastWriteTime -lt $t0) { throw "$tag dump is stale" }
    Write-Output "sampled $tag"
}

# (stride, frame) pairs: k frames at stride 1 vs 4k frames at stride 4 -> same update count
$cases = @(
    @(1, 30), @(4, 120),
    @(1, 60), @(4, 240),
    @(1, 120), @(4, 480)
)
foreach ($c in $cases) {
    Run-Case -stride $c[0] -frame $c[1] -variant 'none'
    Run-Case -stride $c[0] -frame $c[1] -variant 'ddgi'
}

& python (Join-Path $PSScriptRoot 'amortize_check.py') $outDir
exit $LASTEXITCODE
