# ============================================================
# GI sampling harness for 06.GILab  (see docs: "HugEngine GI架构与开发计划" §11.3)
#
# Runs the sample once per diffuse-channel-stack variant. Everything else is identical
# between runs, so differencing two runs isolates the stack difference exactly:
#
#     S_x = lum(HDR(stack=[x]) - HDR(stack=empty))
#
# Direct light / sky / specular cancel in that difference. The "none" baseline uses an
# EMPTY diffuse stack, which the shader handles safely (`den > 0 ? num/den : 0`).
#
# Two pitfalls this harness encodes, both learned the hard way:
#   * direct lights MUST stay on (`gi_solo=0`). SSGI is a screen-space estimator and needs
#     a lit screen as input; with direct light off and no IBL in the stack the screen is
#     black and both SSGI and the baseline read exactly 0, which is indistinguishable from
#     "SSGI produces nothing".
#   * configs are written to the output dir and pointed at with HE_GILAB_CONFIG, so the
#     sample never rewrites the repo's Content/Config/06_GILab.cfg (it saves on exit).
#
# Output: <repo>/Build/verify/gi_<tag>_{hdr,albedo,provN_raw,provN_final}.f16 plus
#         gi_<tag>_meta.txt. Analyse with analyze_gi.py / p5_spectrum.py next to this file.
# ============================================================
param(
    [int]$Frame = 120,           # sampling frame: warmup for DDGI probe converge / SSGI accumulation
    [int]$TimeoutSec = 240,
    [string]$Config = 'Release'  # Build configuration holding 06.GILab.exe
)
$ErrorActionPreference = 'Stop'
# <repo>/Tools/gi/dump_gi.ps1  ->  <repo>
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe     = Join-Path $root "Build\bin\$Config\06.GILab.exe"
$outDir  = Join-Path $root 'Build\verify'
$baseCfg = Join-Path $root 'Content\Config\06_GILab.cfg'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (-not (Test-Path $exe)) { throw "missing exe: $exe (build the 06.GILab target for config '$Config' first)" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

# --- read the baseline config as a key/value table (keeps camera / scene / quality settings) ---
$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}
if ($baseMap.Count -eq 0) { throw "empty baseline config: $baseCfg" }
Write-Output "baseline config: $($baseMap.Count) keys from $baseCfg"

function New-Cfg([string]$path, [System.Collections.IDictionary]$overrides) {
    $map = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $map[$k] = $baseMap[$k] }
    foreach ($k in $overrides.Keys) { $map[$k] = $overrides[$k] }
    $lines = @(foreach ($k in $map.Keys) { "$k=$($map[$k])" })
    [System.IO.File]::WriteAllLines($path, $lines)
}

# diffuse slot order matches the panel / serialization: w0=IBL, w1=DDGI, w2=SSGI, w3=RTGI
$variants = @(
    @{ tag = 'none'; w = @(0, 0, 0, 0) },   # empty stack: baseline (direct + specular + sky)
    @{ tag = 'ddgi'; w = @(0, 1, 0, 0) },
    @{ tag = 'ssgi'; w = @(0, 0, 1, 0) }
)

foreach ($v in $variants) {
    $tag = $v.tag
    $cfg = Join-Path $outDir "dump_$tag.cfg"
    New-Cfg $cfg ([ordered]@{
        gi_blend_diffuse_w0 = "$($v.w[0]).000000"
        gi_blend_diffuse_w1 = "$($v.w[1]).000000"
        gi_blend_diffuse_w2 = "$($v.w[2]).000000"
        gi_blend_diffuse_w3 = "$($v.w[3]).000000"
        gi_solo             = '0'    # KEEP direct lights -- see the header note
        ae_enabled          = '0'    # no auto exposure, so the runs stay comparable
    })
    $log = Join-Path $outDir "dump_$tag.log"
    $err = Join-Path $outDir "dump_$tag.err.log"

    $env:HE_GILAB_CONFIG  = $cfg
    $env:HE_DUMP_GI       = $tag
    $env:HE_DUMP_GI_FRAME = "$Frame"
    Write-Output "=== sampling $tag (diffuse w=$($v.w -join ',')) ==="

    $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                       -RedirectStandardOutput $log -RedirectStandardError $err
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $p.Kill(); Write-Output "  !! timed out after ${TimeoutSec}s, killed"
    }
    Select-String -Path $log, $err -Pattern 'GI' -ErrorAction SilentlyContinue |
        Where-Object { $_.Line -match '\[GI' } |
        ForEach-Object { "  " + $_.Line.Trim() }
    if ((Test-Path $log) -and -not (Select-String -Path $log -Pattern 'GI' -Quiet)) {
        Write-Output "  (no [GI...] lines in $log -- check the sample actually dumped)"
    }
}

Remove-Item Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME, Env:\HE_GILAB_CONFIG -ErrorAction SilentlyContinue
Write-Output ""
Write-Output "=== artifacts ($outDir) ==="
Get-ChildItem $outDir -Filter 'gi_*.f16' | Sort-Object Name |
    ForEach-Object { "  {0,-34} {1,12:N0} B" -f $_.Name, $_.Length }
Get-ChildItem $outDir -Filter 'gi_*_meta.txt' | Sort-Object Name |
    ForEach-Object { "  {0,-34} {1}" -f $_.Name, ([System.IO.File]::ReadAllText($_.FullName) -replace "`r?`n", ' | ').Trim() }
