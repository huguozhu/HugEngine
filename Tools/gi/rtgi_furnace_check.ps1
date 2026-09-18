# ============================================================
# RTGI furnace check (docs section 10.1 task 33 / defect 9.2-AC)
#
# WHAT THIS GUARDS
#   `RT_HitCommon.slang`'s `EvaluateHitRadiance` is the shared one-bounce shading used by RTGI,
#   RT reflection and the DDGI probe trace. It used to return `albedo * E` -- i.e. it both
#   scaled the direct-light term by no albedo at all and skipped the Lambert `1/pi` -- so
#   RTGI and RT reflection were roughly 3x too bright (only `DDGI_Trace.rgen` divided by pi
#   at its call site). Fixed: the function now returns radiance `albedo/pi * (E_ambient + E_direct)`
#   and all three call sites do no conversion of their own.
#
# THE CRITERION (an analytic one, so it can see absolute magnitude)
#   White furnace = uniform white environment (radiance 1) + albedo 1 + direct light off.
#   Then every incident direction has radiance 1 => E = pi => L_o = albedo/pi * pi = albedo.
#   The RT pass therefore returns exactly 1 for BOTH hit and miss, INDEPENDENT of scene
#   geometry -- unlike the composed HDR reading, which cannot resolve a 3.14x scale error
#   against a source whose estimator differs in other ways.
#   Measured: reading 1.0000 (centre and background) with the fix; 0.0000 with the pass's
#   furnace branches disabled (negative control, see docs 10.2 task 33).
#   SSGI is checked in the same run shape because it must keep reading 1.0000 (task 10).
#
# SHADER FRESHNESS GUARD
#   The incremental Slang build does NOT reliably recompile every shader that includes an
#   edited shared header. Measured while doing this task: editing `RT_HitCommon.slang`
#   recompiled `RT_GI.rchit` but not `RT_Reflection.rchit`, and one build path produced no
#   effect at all until the sources were touched. A stale .spv silently keeps the OLD
#   behaviour, which looks exactly like "the fix did nothing". So this check refuses to run
#   on stale bytecode.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/rtgi_furnace_check.ps1 [-Frame 120] [-Config Release]
# Exit code: 0 = all assertions pass, 1 = at least one failed.
# ============================================================
param(
    [int]$Frame = 120,
    [int]$TimeoutSec = 240,
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

$failed = 0
function Assert-That([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Output "  [PASS] $name -- $detail" }
    else     { Write-Output "  [FAIL] $name -- $detail"; $script:failed++ }
}

# ---------------- shader freshness (see header) ----------------
Write-Output "=== shader freshness ==="
$shared = Join-Path $root 'Engine\Shader\Shaders\RT_HitCommon.slang'
$sharedT = (Get-Item $shared).LastWriteTime
$spvDir = Join-Path $root 'build\Engine\Shader\Shaders'
foreach ($spv in @('RT_GI.rchit.spv', 'RT_Reflection.rchit.spv', 'DDGI_Trace.rgen.spv')) {
    $p = Join-Path $spvDir $spv
    $fresh = (Test-Path $p) -and ((Get-Item $p).LastWriteTime -ge $sharedT)
    Assert-That "bytecode newer than RT_HitCommon.slang ($spv)" $fresh `
        $(if (Test-Path $p) { "spv=$((Get-Item $p).LastWriteTime) src=$sharedT" } else { "missing $p" })
}
if ($failed -gt 0) {
    Write-Output ""
    Write-Output "!! stale shader bytecode -- touch the shader sources and rebuild before running this check"
    Write-Output "   (incremental Slang builds do not track includes reliably; see the script header)"
    exit 1
}

# ---------------- run the two furnace cases ----------------
$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}

function Run-Furnace([string]$tag, [string]$slotKey) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($w in 0..3) {
        $m["gi_blend_diffuse_w$w"]  = '0.000000'
        $m["gi_blend_specular_w$w"] = '0.000000'
        $m["gi_blend_ao_w$w"]       = '0.000000'
    }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m[$slotKey] = '1.000000'          # the source under test alone
    $m['gi_solo']    = '1'             # furnace condition includes "direct light off"
    $m['ae_enabled'] = '0'
    $cfg = Join-Path $outDir "chk_$tag.cfg"
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log = Join-Path $outDir "chk_$tag.log"
    Get-ChildItem $outDir -Filter "gi_$tag*" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = $tag
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $env:HE_FURNACE       = '1'
    $env:HE_FURNACE_PROBE = '1'
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "  !! $tag timed out" }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, `
               Env:\HE_FURNACE, Env:\HE_FURNACE_PROBE -ErrorAction SilentlyContinue

    # The probe line carries ASCII numbers only; parse those (the labels are UTF-8 Chinese,
    # which PowerShell 5.1 cannot match reliably).
    $line = Select-String -Path $log -Pattern '\[.*\] .*=(\(.*\)) .*=(\(.*\))' -Encoding UTF8 |
            Select-Object -Last 1
    if (-not $line) { return $null }
    $m2 = [regex]::Match($line.Line, '=\(([\d\.]+),([\d\.]+),([\d\.]+)\).*=\(([\d\.]+),([\d\.]+),([\d\.]+)\)')
    if (-not $m2.Success) { return $null }
    $centre = [double]$m2.Groups[1].Value
    return [pscustomobject]@{ centre = $centre }
}

Write-Output ""
Write-Output "=== furnace readings (analytic truth = 1.0000) ==="
$cases = @(
    @{ tag = 'chk_rtgi_furnace'; slot = 'gi_blend_diffuse_w3'; label = 'RTGI (diffuse slot 3)' },
    @{ tag = 'chk_ssgi_furnace'; slot = 'gi_blend_diffuse_w2'; label = 'SSGI (diffuse slot 2)' }
)
foreach ($c in $cases) {
    $r = Run-Furnace $c.tag $c.slot
    if ($null -eq $r) {
        Assert-That "$($c.label) furnace reading" $false "no probe line parsed from chk_$($c.tag).log"
        continue
    }
    $ok = ($r.centre -gt 0.98) -and ($r.centre -lt 1.02)
    Assert-That "$($c.label) furnace reading" $ok ("centre={0:N4} (accept 0.98..1.02)" -f $r.centre)
}

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all RTGI furnace checks passed"
exit 0
