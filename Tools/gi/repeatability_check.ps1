# ============================================================
# Config-state repeatability check (docs section 9.2-Y / task 28)
#
# 9.2-Y was "the same byte-identical config landed on two different effective stacks, 32%
# apart": one experiment kept the diffuse stack empty (baseline 0.05773), another quietly got
# IBL back-filled (0.07554) and the sample wrote that back to the config file on exit. Two
# separate things had to be nailed down, and this check guards the *measurement* half:
#
#   1. running the same private config N times must leave the file byte-identical (any silent
#      state change shows up here -- that is exactly how the back-fill became visible), and
#   2. all N readings must agree (the criterion in the docs: "5 consecutive runs must give the
#      same effective stack and the same reading"), and
#   3. the reading must be in the EMPTY-stack range, not the IBL-refilled range: an empty
#      diffuse stack means "indirect diffuse contributes nothing" (docs 3.1), so a reading in
#      the ~0.0755 range would mean something re-filled the stack behind our back.
#
# The code-level half of the fix (`GIRegistry::Degrade` no longer back-fills an empty channel)
# is guarded by a unit test -- `Tests/TestGITypes.cpp`, "只裁不加" -- because that property is
# about a function, not about a picture.
#
# The config used here: diffuse stack empty (all four weights 0, RSM 0), specular = { IBL }
# (kept so the run does not also trip 9.2-X's un-baked-BRDF-LUT path), direct light on,
# auto exposure off.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/repeatability_check.ps1 [-Runs 5] [-Frame 120]
# Exit code: 0 = all assertions pass, 1 = at least one failed.
# ============================================================
param(
    [int]$Runs = 5,
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
$m = [ordered]@{}
foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
foreach ($w in 0..3) { $m["gi_blend_diffuse_w$w"] = '0.000000' }
$m['gi_blend_diffuse_rsm'] = '0.000000'
$m['gi_blend_specular_w0'] = '1.000000'
$m['gi_solo']    = '0'
$m['ae_enabled'] = '0'

$template = Join-Path $outDir 'rep_template.cfg'
[System.IO.File]::WriteAllLines($template, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))

# The sample rewrites the config on exit by design (it persists panel state), so comparing the
# whole file is too strict -- the save path also ADDS keys that the template lacks (new defaults
# of later tasks). What must not change is the CONFIG STATE: the layer-stack keys plus the
# switches that decide which stack is effective. A silent back-fill shows up as a changed
# `gi_blend_*` value (9.2-Y's fingerprint was gi_blend_diffuse_w0 going 0 -> 1).
$stateKeys = @(
    'gi_blend_diffuse_w0', 'gi_blend_diffuse_w1', 'gi_blend_diffuse_w2', 'gi_blend_diffuse_w3',
    'gi_blend_diffuse_rsm',
    'gi_blend_specular_w0', 'gi_blend_specular_w1', 'gi_blend_specular_w2', 'gi_blend_specular_w3',
    'gi_blend_ao_w0', 'gi_blend_ao_w1', 'gi_blend_ao_w2', 'gi_blend_ao_w3',
    'gi_preset', 'gi_solo', 'gi_intensity', 'gi_rsm_indirect', 'gi_half_res'
)
function Read-State([string]$path) {
    $map = @{}
    foreach ($l in [System.IO.File]::ReadAllLines($path)) {
        if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $map[$Matches[1]] = $Matches[2] }
    }
    $out = [ordered]@{}
    foreach ($k in $stateKeys) { $out[$k] = if ($map.ContainsKey($k)) { $map[$k] } else { '<missing>' } }
    return $out
}

$failed = 0
$changed = 0
for ($i = 1; $i -le $Runs; $i++) {
    $tag = "rep$i"
    $cfg = Join-Path $outDir "chk_rep_$i.cfg"
    Copy-Item $template $cfg -Force
    $before = Read-State $cfg
    $log = Join-Path $outDir "chk_rep_$i.log"
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
    $hdr = Join-Path $outDir "gi_${tag}_hdr.f16"
    if (-not (Test-Path $hdr) -or (Get-Item $hdr).LastWriteTime -lt $t0) {
        Write-Output "  !! FAILED: $tag produced no fresh dump"
        $failed++
    }
    # (1) the config STATE must come back unchanged: a changed gi_blend_* value is exactly how
    #     a silent back-fill would show up (9.2-Y: gi_blend_diffuse_w0 going 0 -> 1)
    $after = Read-State $cfg
    foreach ($k in $stateKeys) {
        if ($after[$k] -ne $before[$k]) {
            Write-Output "  !! ${tag}: config state changed  $k : $($before[$k]) -> $($after[$k])"
            $changed++
        }
    }
}
if ($changed -gt 0) { $failed++ }

$py = Join-Path $PSScriptRoot 'repeatability_check.py'
python $py $outDir $Runs
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all config-state repeatability checks passed"
exit 0
