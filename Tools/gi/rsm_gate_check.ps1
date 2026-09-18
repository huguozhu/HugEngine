# ============================================================
# RSM independent-gating check (section 9.2-F / task 6)
#
# Defect: the whole RSM block in DeferredPipeline_FrameGraph was nested inside
# `if (m_GIConfig.ShouldRunDDGI() && ...)`. RSM has TWO consumers -- the Lighting
# indirect-diffuse channel (stack contains RSM) and the DDGI probe radiance source --
# and the Forward pipeline already gated RSM on its own ShouldRunRSM(). So with DDGI off
# and RSM selected, the Deferred path never registered the RSM pass while the Forward path
# would: the same config behaved differently in the two pipelines, and the panel switch lied.
#
# Three cases, all with gi_solo=0 (direct light on -> HasActiveShadows() is true):
#   A) DDGI off, RSM in the diffuse stack  -> RSM pass MUST be registered   (the fix)
#   B) DDGI off, RSM NOT in the stack      -> RSM pass MUST NOT be registered (no over-fix)
#   C) DDGI on,  RSM NOT in the stack      -> RSM pass MUST NOT be registered
#      (guards section 9.2-R: the probe must fall back to IBL, not sample an empty RSM)
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/rsm_gate_check.ps1 [-Frame 5] [-Config Release]
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
if (-not (Test-Path $outDir))  { New-Item -ItemType Directory -Path $outDir | Out-Null }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}

# Runs one case and returns the sorted unique pass names seen in the log.
function Invoke-Case {
    param([string]$Tag, [hashtable]$Overrides)
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    # clean slate: empty diffuse/specular/ao stacks, then the per-case weights
    foreach ($w in 0..3) {
        $m["gi_blend_diffuse_w$w"]  = '0.000000'
        $m["gi_blend_specular_w$w"] = '0.000000'
        $m["gi_blend_ao_w$w"]       = '0.000000'
    }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m['gi_solo']              = '0'   # direct light on -> active shadows -> RSM can render
    $m['ae_enabled']           = '0'
    $m['gi_shadow']            = '1'   # ShadowChannel::Raster
    foreach ($k in $Overrides.Keys) { $m[$k] = $Overrides[$k] }

    $cfg = Join-Path $outDir "rsm_gate_$Tag.cfg"
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))

    $log = Join-Path $outDir "rsm_gate_$Tag.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_TRACE_PASSES  = '1'
    $env:HE_DUMP_GI       = "rsmgate$Tag"
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $fin = $p.WaitForExit($TimeoutSec * 1000)
    if (-not $fin) { $p.Kill() }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_TRACE_PASSES, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue
    if (-not $fin) { throw "case $Tag did not finish within ${TimeoutSec}s (see $log)" }

    $text = [System.IO.File]::ReadAllText($log)
    if ($text -match 'EXCEPTION_ACCESS_VIOLATION') { throw "case $Tag crashed (see $log)" }
    $passes = @(Select-String -Path $log -Pattern '\[PASS\] (.+)$' |
                ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() } | Sort-Object -Unique)
    if ($passes.Count -eq 0) { throw "case $Tag produced no pass list (see $log)" }
    Write-Output ("case $Tag passes: " + ($passes -join ', '))
    return $passes
}

$fail = 0

# A) DDGI off + RSM in the diffuse stack -> RSM must register
$pA = Invoke-Case -Tag 'a_rsm_only' -Overrides @{
    'gi_blend_diffuse_w1'  = '0.000000'   # DDGI off
    'gi_blend_diffuse_rsm' = '1.000000'   # RSM in the stack
}
if ($pA -contains 'RSM') {
    Write-Output "PASS A: DDGI off + RSM in stack -> RSM pass registered"
} else {
    Write-Output "FAIL A: RSM is in the diffuse stack with DDGI off but its pass was never registered"
    Write-Output "        -> RSM is still coupled to the DDGI gate (section 9.2-F)"
    $fail++
}

# B) DDGI off + RSM not in the stack -> RSM must NOT register
$pB = Invoke-Case -Tag 'b_none' -Overrides @{}
if ($pB -notcontains 'RSM') {
    Write-Output "PASS B: DDGI off + RSM not in stack -> no RSM pass"
} else {
    Write-Output "FAIL B: RSM pass registered although nothing asked for it (over-fix)"
    $fail++
}

# C) DDGI on + RSM not in the stack -> RSM must NOT register (probe falls back to IBL)
$pC = Invoke-Case -Tag 'c_ddgi_only' -Overrides @{
    'gi_blend_diffuse_w1' = '1.000000'   # DDGI on, RSM absent
}
if ($pC -notcontains 'RSM') {
    Write-Output "PASS C: DDGI on + RSM not in stack -> no RSM pass (probe uses IBL)"
} else {
    Write-Output "FAIL C: DDGI alone registered an RSM pass whose output feeds the probe (section 9.2-R)"
    $fail++
}

if ($fail -eq 0) { Write-Output "RESULT: PASS (3/3)"; exit 0 }
Write-Output "RESULT: FAIL ($fail case(s))"
exit 1
