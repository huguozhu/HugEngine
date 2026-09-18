# ============================================================
# IBL bake gating check (docs section 9.2-X / task 27)
#
# The defect this guards: the IBL bake (irradiance / prefilter / BRDF LUT) used to be
# registered only when a *GI channel stack* asked for IBL. But the BRDF LUT is sampled
# unconditionally by the lighting shader's direct-light BRDF, so with an empty specular
# stack nothing ever baked it -- the direct-light BRDF read uninitialised memory and the
# picture came out roughly 4.7x too bright with a 463-level firefly at a fixed pixel.
#
# Two assertions, both over configurations whose specular stack is EMPTY (that is the
# trigger):
#   1. STRUCTURE: the `IBL_Bake` pass must still be registered (checked in the pass list
#      that HE_TRACE_PASSES=1 prints). Before the fix it was absent in these configs.
#   2. NUMBERS: the HDR maximum must stay in the ~42 range; before the fix the empty-stack
#      configurations read max = 463.32 while the IBL-specular one read 42.20.
#
# Cases:
#   empty  : diffuse empty, specular empty, ao empty  (the original repro)
#   ssao   : same but ao = { SSAO }                   (the doc's third case)
#   ibl    : specular = { IBL }                       (control: the bake was always triggered)
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/ibl_lut_gate_check.ps1 [-Frame 120] [-Config Release]
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

$cases = @(
    @{ tag = 'iblgate_empty'; spec = '0.000000'; ao = '0.000000' },
    @{ tag = 'iblgate_ssao';  spec = '0.000000'; ao = '1.000000' },
    @{ tag = 'iblgate_ibl';   spec = '1.000000'; ao = '0.000000' }
)

$failed = 0
foreach ($c in $cases) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($w in 0..3) {
        $m["gi_blend_diffuse_w$w"]  = '0.000000'
        $m["gi_blend_specular_w$w"] = '0.000000'
        $m["gi_blend_ao_w$w"]       = '0.000000'
    }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m['gi_blend_specular_w0'] = $c.spec
    $m['gi_blend_ao_w0']       = $c.ao
    $m['gi_solo']    = '0'
    $m['ae_enabled'] = '0'
    $name = $c.tag
    $cfg  = Join-Path $outDir "chk_$name.cfg"
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log  = Join-Path $outDir "chk_$name.log"
    Get-ChildItem $outDir -Filter "gi_$name*" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    $env:HE_GILAB_CONFIG   = $cfg
    $env:HE_DUMP_GI        = $name
    $env:HE_DUMP_GI_FRAME  = "$Frame"
    $env:HE_TRACE_PASSES   = '1'      # pass list -> structural assertion
    $t0 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "  !! $name timed out" }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_TRACE_PASSES -ErrorAction SilentlyContinue
    $hdr = Join-Path $outDir "gi_${name}_hdr.f16"
    if (-not (Test-Path $hdr) -or (Get-Item $hdr).LastWriteTime -lt $t0) {
        Write-Output "  !! FAILED: $name produced no fresh dump"
        $failed++
    } else {
        $baked = (Select-String -Path $log -Pattern 'RG pass: IBL_Bake' -ErrorAction SilentlyContinue |
                  Measure-Object).Count
        Write-Output "sampled $name (specular=$($c.spec) ao=$($c.ao))  IBL_Bake pass lines=$baked"
    }
}
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_TRACE_PASSES -ErrorAction SilentlyContinue

$py = Join-Path $PSScriptRoot 'ibl_lut_gate_check.py'
python $py $outDir
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all IBL bake-gate checks passed"
exit 0
