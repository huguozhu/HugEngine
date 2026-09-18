# ============================================================
# SSR check (docs section 9.2-W / task 25)
#
# Four cases, each with a private cfg copy (the sample rewrites HE_GILAB_CONFIG on exit):
#
#   hiz      : specular = { SSR }, ssr_use_hiz = 1   (the default path -- must produce hits)
#   lin      : specular = { SSR }, ssr_use_hiz = 0   (linear march -- the reference)
#   ibl      : specular = { IBL }                    (no SSR: the baseline picture)
#   ibl_ssr  : specular = { IBL, SSR }               (SSR must change that picture)
#
# What it asserts:
#   1. the Hi-Z path's validity fraction (alpha > 0 in the SSR output) is at least 5% --
#      before task 25 it was 0%: every pixel missed and the whole output was RGB=0 with
#      alpha = -1, so "SSR in the stack" and "no SSR" were pixel-identical;
#   2. Hi-Z finds at least half as many hits as the linear march (same order of magnitude);
#   3. adding SSR to a specular stack that already has IBL changes the HDR reading, i.e. the
#      source is no longer a no-op.
#
# The SSR output target is `prov4_spec_raw`: the dump names providers by registration order
# (0=AO, 1=IBL, 2=RSM, 3=SSGI, 4=SSR, 5=DDGI, then the four RT effects). The analyzer prints
# which file it used, so a re-ordering shows up immediately instead of silently reading
# another source's texture.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/ssr_check.ps1 [-Frame 120] [-Config Release]
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
    @{ tag = 'hiz';     spec = 'ssr';      useHiz = '1' },
    @{ tag = 'lin';     spec = 'ssr';      useHiz = '0' },
    @{ tag = 'ibl';     spec = 'ibl';      useHiz = '1' },
    @{ tag = 'ibl_ssr'; spec = 'ibl_ssr';  useHiz = '1' }
)

$failed = 0
foreach ($c in $cases) {
    $m = [ordered]@{}
    foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
    # 漫反射层栈留空：本检查只看镜面通道，空漫反射栈让 SSR 的贡献不被别的源分走
    foreach ($w in 0..3) { $m["gi_blend_diffuse_w$w"] = '0.000000' }
    $m['gi_blend_diffuse_rsm'] = '0.000000'
    # specular = { IBL } / { SSR } / { IBL, SSR }（槽位序：w0=IBL, w1=SSR, w2=RTReflection）
    $m['gi_blend_specular_w0'] = if ($c.spec -eq 'ssr') { '0.000000' } else { '1.000000' }
    $m['gi_blend_specular_w1'] = if ($c.spec -eq 'ibl') { '0.000000' } else { '1.000000' }
    $m['gi_blend_specular_w2'] = '0.000000'
    $m['gi_blend_specular_w3'] = '0.000000'
    $m['ssr_use_hiz'] = $c.useHiz
    $m['gi_solo']    = '0'
    $m['ae_enabled'] = '0'
    $name = "ssr_$($c.tag)"
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
        Write-Output "sampled $name (specular=$($c.spec), ssr_use_hiz=$($c.useHiz))"
    }
}
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

$py = Join-Path $PSScriptRoot 'ssr_check.py'
python $py $outDir
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all SSR checks passed"
exit 0
