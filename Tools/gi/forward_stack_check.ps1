# ============================================================
# Forward layer-stack check (docs section 10.2 task 26 / 9.2-H)
#
# `pipeline_mode=0` runs four diffuse stacks over the same scene/camera: empty, { IBL },
# { RSM } and { IBL, RSM }. Two facts have to hold for "Forward really went through the
# layer stack + normalized composite":
#
#   1. the HDR reading must DEPEND on the stack (before task 26 the cfg keys were applied to
#      the Deferred pipeline only, so `pipeline_mode=0` ignored them entirely);
#   2. the two-source reading must be the WEIGHTED MEAN of the two single-source readings,
#      not their sum: equal weights (1,1) => mean({IBL,RSM}) == (mean({IBL}) + mean({RSM}))/2.
#      That is the same criterion the Deferred path is held to (SSGI-CAL); it is what makes
#      "adding a source does not brighten the picture" quantitative instead of hand-wavy.
#
# WHAT THESE CRITERIA CANNOT SEE (defect 9.2-AD): all three hold even when one source
# contributes EXACTLY zero -- measured: `fwd_rsm` (0.0865436) is byte-identical to the EMPTY
# stack, i.e. Forward's RSM currently has no producer at all (task 34). The empty-stack run
# and the reported differential in the .py exist to make that visible; the differential is
# reported, not asserted, because asserting it would freeze an open defect into the suite.
#
# The numbers come from `gi_<tag>_hdr.f16`, which the sample only started writing for the
# Forward pipeline in task 26 (before that it always dumped the *Deferred* HDR target, so the
# Forward picture was literally unmeasurable -- see docs 11.3).
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/forward_stack_check.ps1 [-Frame 120] [-Config Release]
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

# diffuse stacks: w0=IBL, w1=DDGI, w2=SSGI, w3=RTGI; RSM has its own key.
# `fwd_none` is the EMPTY diffuse stack: without it the three criteria below cannot tell
# "RSM contributes a dim picture" from "RSM contributes nothing at all" (see the .py header
# and defect 9.2-AD -- the {RSM} reading measured here is byte-identical to the empty stack).
$cases = @(
    @{ tag = 'fwd_none';    ibl = '0.000000'; rsm = '0.000000' },
    @{ tag = 'fwd_ibl';     ibl = '1.000000'; rsm = '0.000000' },
    @{ tag = 'fwd_rsm';     ibl = '0.000000'; rsm = '1.000000' },
    @{ tag = 'fwd_ibl_rsm'; ibl = '1.000000'; rsm = '1.000000' }
)

$failed = 0
foreach ($c in $cases) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    $m['pipeline_mode'] = '0'                      # Forward
    $m['gi_blend_diffuse_w0'] = $c.ibl
    $m['gi_blend_diffuse_w1'] = '0.000000'
    $m['gi_blend_diffuse_w2'] = '0.000000'
    $m['gi_blend_diffuse_w3'] = '0.000000'
    $m['gi_blend_diffuse_rsm'] = $c.rsm
    # specular stays at { IBL } in all three cases: this check isolates the DIFFUSE channel
    $m['gi_blend_specular_w0'] = '1.000000'
    $m['gi_blend_specular_w1'] = '0.000000'
    $m['gi_blend_specular_w2'] = '0.000000'
    $m['gi_blend_specular_w3'] = '0.000000'
    $m['gi_rsm_indirect'] = '1'
    $m['gi_shadow']  = '1'
    $m['gi_solo']    = '0'
    $m['ae_enabled'] = '0'
    $name = $c.tag
    $cfg  = Join-Path $outDir "chk_$name.cfg"
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log  = Join-Path $outDir "chk_$name.log"
    Get-ChildItem $outDir -Filter "gi_$name*" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = $name
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $t0 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "  !! $name timed out" }
    $hdr = Join-Path $outDir "gi_${name}_hdr.f16"
    if (-not (Test-Path $hdr) -or (Get-Item $hdr).LastWriteTime -lt $t0) {
        Write-Output "  !! FAILED: $name produced no fresh dump"
        $failed++
    } else {
        Write-Output "sampled $name (forward: IBL=$($c.ibl) RSM=$($c.rsm))"
    }
}
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

$py = Join-Path $PSScriptRoot 'forward_stack_check.py'
python $py $outDir
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all Forward layer-stack checks passed"
exit 0
