# ============================================================
# Stack vs subsystem-switch consistency check (docs invariant 1 / section 9.2-G / task 4)
#
# The stack is the single source of truth: if a channel stack lists a source, that source's
# pass MUST be registered. The defect this guards against: the subsystem `enabled` flag was
# derived from the stack only once, at pipeline Initialize. Any later stack change (config
# load, preset, panel) then desynchronised them -- the source sat in the stack with a
# positive weight while its provider reported IsValid=false and no pass was registered, so
# it silently contributed nothing (and the normalisation divided by its weight anyway).
#
# Method: put a source in a channel stack, run with HE_TRACE_PASSES=1, and require its pass
# name to appear in the frame's pass list. SSR is the cheap case: the default preset (Medium)
# has specular = { IBL } only, so at Initialize the SSR switch is off while the config below
# puts SSR into the specular stack.
#
# Before the fix (with the sample's hand patch removed) the pass list had no SSR at all.
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/stack_switch_check.ps1 [-Frame 5] [-Config Release]
# ============================================================
param(
    [int]$Frame = 5,
    [int]$TimeoutSec = 120,
    [string]$Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe     = Join-Path $root "build\bin\$Config\06.GILab.exe"
$outDir  = Join-Path $root 'Build\verify'
$baseCfg = Join-Path $root 'Content\Config\06_GILab.cfg'
if (-not (Test-Path $exe))     { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}
$m = [ordered]@{}
foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
# specular = { SSR }  (slot order: w0=IBL, w1=SSR, w2=RTReflection)
$m['gi_blend_specular_w0'] = '0.000000'
$m['gi_blend_specular_w1'] = '1.000000'
$m['gi_blend_specular_w2'] = '0.000000'
# diffuse empty: keeps the frame simple; the check only looks at pass registration
$m['gi_blend_diffuse_w0'] = '0.000000'
$m['gi_blend_diffuse_w1'] = '0.000000'
$m['gi_blend_diffuse_w2'] = '0.000000'
$m['gi_blend_diffuse_w3'] = '0.000000'
$m['gi_solo']    = '0'
$m['ae_enabled'] = '0'
$cfg = Join-Path $outDir 'stack_switch_check.cfg'
[System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))

$log = Join-Path $outDir 'stack_switch_check.log'
if (Test-Path $log) { Remove-Item $log -Force }
$env:HE_GILAB_CONFIG  = $cfg
$env:HE_TRACE_PASSES  = '1'
$env:HE_DUMP_GI       = 'ssrchk'
$env:HE_DUMP_GI_FRAME = "$Frame"
$p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                   -RedirectStandardOutput $log -RedirectStandardError "$log.err"
$fin = $p.WaitForExit($TimeoutSec * 1000)
if (-not $fin) { $p.Kill() }
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_TRACE_PASSES, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

if (-not $fin) { Write-Output "FAIL: run did not finish within ${TimeoutSec}s (see $log)"; exit 1 }

# Pass names come from HE_TRACE_PASSES ("[PASS] <name>"). The last frame's list is what matters,
# but any occurrence proves registration (the list is identical every frame here).
$passes = Select-String -Path $log -Pattern '\[PASS\] (.+)$' |
          ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } | Sort-Object -Unique
Write-Output ("passes seen: " + ($passes -join ', '))

if ($passes -contains 'SSR') {
    Write-Output "PASS: specular stack lists SSR and the SSR pass is registered (stack and subsystem agree)."
    exit 0
}
Write-Output "FAIL: SSR is in the specular stack but its pass was never registered"
Write-Output "      -> the subsystem switch did not follow the stack (invariant 1 broken)"
exit 1
