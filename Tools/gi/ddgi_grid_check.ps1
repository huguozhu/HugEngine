# ============================================================
# DDGI probe-grid check (docs section 9.2-K / task 14)
#
# Evidence for the two halves of task 14 and for the premise of task 17:
#
#   A) fixed   : auto-fit OFF -> the old fixed grid (8x4x8, cell 3) covers only
#                21x9x21 world units while the scene AABB is 3720.9x1555.9x2288.2.
#                With the probe-grid confidence bit ON, DDGI's screen contribution
#                must collapse to ~0 -- i.e. everything measured before came from
#                the border-clamp extrapolation (SampleDDGI clamps all 8 taps to one
#                border probe when the query is far outside the grid).
#   B) fit16   : auto-fit ON, 16 cells along the longest axis -> the grid must really
#                cover the scene, and DDGI's contribution must come back.
#   C) fit32   : same, 32 cells (8960 probes, cell 120). If the probe field carried
#                spatial information, the screen contribution would change. It does
#                not: in the current default path (RSM not in the diffuse stack) the
#                IBL fallback in DDGI.comp.slang samples u_IBLIrradiance by `dir` only
#                and never uses `samplePos`, so every probe gets the same SH.
#                => this case documents the premise of task 17 (DDGI ray march).
#
# Each case needs its own `none` baseline, so everything runs through a PRIVATE cfg
# copy (the sample rewrites HE_GILAB_CONFIG on exit) -- see docs 11.3.
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/ddgi_grid_check.ps1 [-Frame 120] [-Config Release]
# Exit code: 0 = all cases pass, 1 = at least one failed.
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
if (-not (Test-Path $exe)) { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$baseMap = [ordered]@{}
foreach ($l in [System.IO.File]::ReadAllLines($baseCfg)) {
    if ($l -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$') { $baseMap[$Matches[1]] = $Matches[2] }
}

# RSM must stay OUT of the diffuse stack: it would put DDGI on the useRSM path,
# where radiance does depend on the probe position, and then case C would not isolate
# the IBL-fallback question at all.
$cases = @(
    @{ tag = 'gridFixed'; ov = @{ ddgi_grid_auto = '0' } },
    @{ tag = 'gridFit8';  ov = @{ ddgi_grid_auto = '1'; ddgi_fit_cells = '8' } },
    @{ tag = 'gridFit16'; ov = @{ ddgi_grid_auto = '1'; ddgi_fit_cells = '16' } },
    @{ tag = 'gridFit32'; ov = @{ ddgi_grid_auto = '1'; ddgi_fit_cells = '32' } }
)

$failed = 0
foreach ($c in $cases) {
    foreach ($stack in @('none', 'ddgi')) {
        $m = [ordered]@{}
        foreach ($k in $baseMap.Keys) { $m[$k] = $baseMap[$k] }
        foreach ($k in $c.ov.Keys) { $m[$k] = $c.ov[$k] }
        $m['gi_blend_diffuse_w0']  = '0.000000'
        $ddgiWeight = '0.000000'
        if ($stack -eq 'ddgi') { $ddgiWeight = '1.000000' }
        $m['gi_blend_diffuse_w1']  = $ddgiWeight
        $m['gi_blend_diffuse_w2']  = '0.000000'
        $m['gi_blend_diffuse_w3']  = '0.000000'
        $m['gi_blend_diffuse_rsm'] = '0.000000'   # see note above
        $m['gi_solo']    = '0'                    # keep direct light (same as dump_gi)
        $m['ae_enabled'] = '0'
        $name = "grid_$($c.tag)_$stack"
        $cfg  = Join-Path $outDir "chk_$name.cfg"
        [System.IO.File]::WriteAllLines($cfg, @(foreach ($k in $m.Keys) { "$k=$($m[$k])" }))
        $log = Join-Path $outDir "chk_$name.log"
        Get-ChildItem $outDir -Filter "gi_$name*" -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue

        $env:HE_GILAB_CONFIG  = $cfg
        $env:HE_DUMP_GI       = $name
        $env:HE_DUMP_GI_FRAME = "$Frame"
        $t0 = Get-Date
        $p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                           -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Output "  !! $name timed out" }
        # stale-dump guard (docs 9.2-U / 11.3.1)
        $hdr = Join-Path $outDir "gi_${name}_hdr.f16"
        if (-not (Test-Path $hdr) -or (Get-Item $hdr).LastWriteTime -lt $t0) {
            Write-Output "  !! FAILED: $name produced no fresh dump"
            $failed++
        }
    }
}
Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

# --- report the fitted grid each case actually used ---
# Match on ASCII only: the log text is Chinese, and this script must stay ASCII (ANSI parsing).
Write-Output ""
Write-Output "=== fitted grid per case (from the run log; 'auto-fit OFF' = fixed 8x4x8, cell 3) ==="
foreach ($c in $cases) {
    $log  = Join-Path $outDir "chk_grid_$($c.tag)_ddgi.log"
    $line = (Select-String -Path $log -Pattern '(\d+x\d+x\d+)' -ErrorAction SilentlyContinue |
             Select-Object -First 1)
    $grid = 'auto-fit OFF (fixed 8x4x8, cell 3)'
    if ($line) { $grid = $line.Matches[0].Groups[1].Value }
    Write-Output ("{0,-10} probes {1}" -f $c.tag, $grid)
}

$py = Join-Path $PSScriptRoot 'ddgi_grid_check.py'
python $py $outDir 'gridFixed' 'gridFit8' 'gridFit16' 'gridFit32'
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all DDGI grid checks passed"
exit 0
