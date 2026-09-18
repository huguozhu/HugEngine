# ============================================================
# SSGI calibration check (docs section 10.1 task 10 / section 9.2-P)
#
# Three assertions, in increasing order of "end to end":
#
#   1. ANALYTIC (furnace): the estimator's normalisation is exact. The furnace condition is
#      "incident radiance = 1 in every direction + receiver albedo = 1", under which the true
#      answer for a diffuse source is E/pi = 1. SSGI now evaluates its REAL path in furnace mode
#      (no short circuit), so the probe reads Sigma(L*cos)/Sigma(cos) = 1 exactly when the
#      normalisation is right -- the old "divide by N" form reads ~0.44 instead (verified by
#      temporarily restoring it). This is a harder reference than a path tracer: analytic,
#      no convergence noise.
#
#   2. BLEND: with equal weights the two-source differential must be the weighted mean of the
#      two single-source differentials (small positive bias expected: the screen-edge confidence
#      drops SSGI at the border but not DDGI).
#
#   3. MAGNITUDE: DDGI and SSGI must estimate the same physical quantity, so their differential
#      magnitudes must be within the same order. Before SSGI-CAL this ratio was 21x.
#
# Tests 2 and 3 use the differentials produced by dump_gi.ps1 -- the SAME harness that produces
# the numbers quoted in the docs, so all variants share one baseline recipe. Do NOT rebuild those
# configs here: different writers can land on different fallback paths in this sample (measured
# 0.0572 vs 0.0755 for the same written keys, see section 9.2-X), and then the differences no
# longer isolate the stack.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/ssgi_cal_check.ps1 [-Frame 120] [-Config Release]
# ============================================================
param(
    [int]$Frame = 120,
    [int]$TimeoutSec = 240,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe    = Join-Path $root "Build\bin\$Config\06.GILab.exe"
$outDir = Join-Path $root 'Build\verify'
$base   = Join-Path $root 'Content\Config\06_GILab.cfg'
if (-not (Test-Path $exe)) { throw "missing exe: $exe" }

# --- (2)(3) differentials straight from the canonical harness ------------------------------
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'dump_gi.ps1') `
    -Frame $Frame -TimeoutSec $TimeoutSec -Config $Config | Out-Null
if ($LASTEXITCODE -ne 0) { throw "dump_gi.ps1 failed (stale/crashed run) -- numbers unusable" }

# --- (1) furnace: SSGI alone, analytical truth = 1 -----------------------------------------
$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($base)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}
$m = [ordered]@{}
foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
foreach ($w in 0..3) { $m["gi_blend_diffuse_w$w"] = '0.000000' }
$m['gi_blend_diffuse_w2'] = '1.000000'     # SSGI only
# gi_solo MUST be 1: the furnace condition includes "direct light off". Leaving it at 0 re-enables
# the direct lights under HE_FURNACE=1 and the probe then reads direct + E/pi (measured: centre
# 1.0078 but background 1.7053 -- the background pixel was direct-lit).
$m['gi_solo']    = '1'
$m['ae_enabled'] = '0'
$cfg = Join-Path $outDir 'ssgical_furnace.cfg'
[System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
$log = Join-Path $outDir 'ssgical_furnace.log'
if (Test-Path $log) { Remove-Item $log -Force }
Get-ChildItem $outDir -Filter 'gi_ssgical_furnace*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
$env:HE_GILAB_CONFIG  = $cfg
$env:HE_DUMP_GI       = 'ssgical_furnace'
$env:HE_DUMP_GI_FRAME = "$Frame"
$env:HE_FURNACE       = '1'
$env:HE_FURNACE_PROBE = '1'
$t0 = Get-Date
$p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                   -RedirectStandardOutput $log -RedirectStandardError "$log.err"
if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill() }
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_FURNACE, Env:\HE_FURNACE_PROBE -ErrorAction SilentlyContinue
$hdr = Join-Path $outDir 'gi_ssgical_furnace_hdr.f16'
if (-not (Test-Path $hdr)) { throw "furnace case produced no dump" }
if ((Get-Item $hdr).LastWriteTime -lt $t0) { throw "furnace dump is stale" }
Write-Output "sampled ssgical_furnace"

& python (Join-Path $PSScriptRoot 'ssgi_cal_check.py') $outDir
exit $LASTEXITCODE
