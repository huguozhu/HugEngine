# ============================================================
# Per-pixel confidence check (docs section 3.2 / task 9)
#
# The confidence multiplier is a RELATIVE re-weighting: it decides which sources a pixel
# trusts and lets the normalisation give their share to the others. Two consequences that
# shape this test:
#   * it is invisible with a single source in the channel (w cancels in num/den), so every
#     case below puts TWO sources in the channel;
#   * its effect scales with the per-channel fade band, which is what makes the band itself
#     observable -- and the band now comes from the UBO (gi_edge_fade), not a shader constant.
#
#   Test 1 (diffuse, band = 5%):
#     A = diffuse{IBL}   B = diffuse{IBL, SSGI}
#     In the outermost pixel ring SSGI's confidence is ~0, so the blend collapses onto IBL and
#     |A-B| must be a tiny fraction of the centre value. The centre must stay clearly above
#     zero -- positive control: if confidence zeroed everything, that check fails.
#
#   Test 2 (same pair, band = 50%): the ring 5%..15% away from the edge is fully trusted at
#     band 5% but only ~10..30% trusted at band 50%, so |A-B| there must drop by roughly the
#     ratio of the bands. This is what proves the band is data-driven and the path is live.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/confidence_check.ps1 [-Frame 60] [-Config Release]
# ============================================================
param(
    [int]$Frame = 60,
    [int]$TimeoutSec = 240,
    [string]$Config = 'Release',
    [string]$Exe = ''      # point at another (pre-change) binary to show the check fails there
)
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe     = if ($Exe -ne '') { $Exe } else { Join-Path $root "Build\bin\$Config\06.GILab.exe" }
$outDir  = Join-Path $root 'Build\verify'
$baseCfg = Join-Path $root 'Content\Config\06_GILab.cfg'
if (-not (Test-Path $exe))     { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}

function Run-Case {
    param([string]$Tag, [hashtable]$Overrides)
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    foreach ($w in 0..3) {
        $m["gi_blend_diffuse_w$w"]  = '0.000000'
        $m["gi_blend_specular_w$w"] = '0.000000'
        $m["gi_blend_ao_w$w"]       = '0.000000'
    }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    $m['gi_solo']      = '0'          # direct light on: screen-space sources need a lit screen
    $m['ae_enabled']   = '0'          # no auto exposure, otherwise the runs are not comparable
    $m['gi_shadow']    = '1'
    $m['gi_edge_fade'] = '0.050000'
    foreach ($k in $Overrides.Keys) { $m[$k] = $Overrides[$k] }

    $cfg = Join-Path $outDir "conf_$Tag.cfg"
    [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
    $log = Join-Path $outDir "conf_$Tag.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    Get-ChildItem $outDir -Filter "gi_$Tag*" -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = $Tag
    $env:HE_DUMP_GI_FRAME = "$Frame"
    $t0 = Get-Date
    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill() }
    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

    $hdr = Join-Path $outDir "gi_${Tag}_hdr.f16"
    if (-not (Test-Path $hdr)) { throw "case $Tag produced no dump (crashed or never reached frame $Frame)" }
    if ((Get-Item $hdr).LastWriteTime -lt $t0) { throw "case $Tag dump is stale" }
    Write-Output "sampled $Tag"
}

# band = 5% (default): border must drop to ~0, centre must keep contributing
Run-Case -Tag 'c1_ibl'      -Overrides @{ 'gi_blend_diffuse_w0' = '1.000000' }
Run-Case -Tag 'c1_ibl_ssgi' -Overrides @{ 'gi_blend_diffuse_w0' = '1.000000'
                                          'gi_blend_diffuse_w2' = '1.000000' }
# band = 50%: the same sources, but the fade reaches far into the frame
Run-Case -Tag 'c3_ibl_wide'      -Overrides @{ 'gi_blend_diffuse_w0' = '1.000000'
                                               'gi_edge_fade'        = '0.500000' }
Run-Case -Tag 'c3_ibl_ssgi_wide' -Overrides @{ 'gi_blend_diffuse_w0' = '1.000000'
                                               'gi_blend_diffuse_w2' = '1.000000'
                                               'gi_edge_fade'        = '0.500000' }

& python (Join-Path $PSScriptRoot 'confidence_check.py') $outDir
exit $LASTEXITCODE
