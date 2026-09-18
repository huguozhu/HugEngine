# ============================================================
# 06.GILab launch soak -- root-cause work for D1 "intermittent crash"
# (GI doc section 10.1, task 2)
#
# Purpose: turn "intermittent" into a measurable rate, and keep every crash
# report so call stacks can be compared across runs.
#
# NOTE: this file is intentionally ASCII-only. Windows PowerShell 5.1 reads
# .ps1 as ANSI unless the file carries a UTF-8 BOM, so non-ASCII content in the
# scripts under Tools/ breaks parsing (the existing dump_gi.ps1 is ASCII-only
# for the same reason). Keep it ASCII.
#
# Usage:
#   powershell -File Tools/gi/soak_launch.ps1 -Runs 30 -Tag baseline
#
# Output:
#   Build/verify/soak_<Tag>_summary.txt     per-run verdicts + totals
#   Build/verify/soak_<Tag>_run<N>.log      full log of that launch
#   Build/verify/soak_<Tag>_crash<N>.txt    the crash report block (call stack)
#
# Verdicts (detected through ASCII markers only):
#   OK      -- reached the sampling dump ("gi_<DumpTag>_hdr.f16") and exited
#   CRASH   -- "EXCEPTION_ACCESS_VIOLATION" present in the log
#   TIMEOUT -- did not exit within TimeoutSec (killed)
#   NODUMP  -- exited without a dump and without a crash report
# ============================================================
param(
    [int]$Runs = 30,
    [int]$Frame = 60,
    [int]$TimeoutSec = 120,
    [string]$Tag = 'soak',
    [string]$DumpTag = 'none',
    [string]$Config = 'Release',
    [string]$ConfigFile = ''
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe    = Join-Path $root "build\bin\$Config\06.GILab.exe"
$outDir = Join-Path $root 'Build\verify'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not (Test-Path $exe)) { throw "missing exe: $exe" }
if ([string]::IsNullOrEmpty($ConfigFile)) { $ConfigFile = Join-Path $outDir "dump_$DumpTag.cfg" }
if (-not (Test-Path $ConfigFile)) { throw "missing config: $ConfigFile" }

$crashMark = 'EXCEPTION_ACCESS_VIOLATION'
$dumpMark  = "gi_${DumpTag}_hdr.f16"

$summary = Join-Path $outDir "soak_${Tag}_summary.txt"
$rows = New-Object System.Collections.Generic.List[string]
$ok = 0; $crash = 0; $timeout = 0; $nodump = 0

for ($i = 1; $i -le $Runs; $i++) {
    $log = Join-Path $outDir "soak_${Tag}_run$i.log"
    $err = Join-Path $outDir "soak_${Tag}_run$i.err.log"
    if (Test-Path $log) { Remove-Item $log -Force }
    if (Test-Path $err) { Remove-Item $err -Force }

    $env:HE_GILAB_CONFIG  = $ConfigFile
    $env:HE_DUMP_GI       = $DumpTag
    $env:HE_DUMP_GI_FRAME = "$Frame"

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p  = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru -NoNewWindow `
                        -RedirectStandardOutput $log -RedirectStandardError $err
    $finished = $p.WaitForExit($TimeoutSec * 1000)
    if (-not $finished) { $p.Kill() }
    $secs = [int]$sw.Elapsed.TotalSeconds

    Remove-Item Env:\HE_GILAB_CONFIG, Env:\HE_DUMP_GI, Env:\HE_DUMP_GI_FRAME -ErrorAction SilentlyContinue

    $hasCrash = (Select-String -Path $log -Pattern $crashMark -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0
    $hasDump  = (Select-String -Path $log -Pattern $dumpMark  -ErrorAction SilentlyContinue | Measure-Object).Count -gt 0

    $verdict = 'NODUMP'
    if ($hasCrash)        { $verdict = 'CRASH';   $crash++ }
    elseif (-not $finished) { $verdict = 'TIMEOUT'; $timeout++ }
    elseif ($hasDump)     { $verdict = 'OK';      $ok++ }
    else                  { $nodump++ }

    if ($hasCrash) {
        $report = Join-Path $outDir "soak_${Tag}_crash$i.txt"
        $lines = Get-Content $log -Encoding UTF8
        $hit = $lines | Select-String -Pattern $crashMark | Select-Object -First 1
        if ($hit) {
            $idx = $hit.LineNumber
            $lines[([Math]::Max(0,$idx-8))..([Math]::Min($lines.Count-1,$idx+12))] | Set-Content $report -Encoding UTF8
        }
    }

    $line = "run {0,3}  {1,-8} {2,4}s" -f $i, $verdict, $secs
    $rows.Add($line)
    Write-Output $line
}

$head = @(
    "soak tag       : $Tag",
    "runs           : $Runs",
    "OK             : $ok",
    "CRASH          : $crash",
    "TIMEOUT        : $timeout",
    "NODUMP         : $nodump",
    "exe            : $exe",
    "config         : $ConfigFile",
    ""
)
($head + $rows) | Set-Content $summary -Encoding UTF8
Write-Output ""
Write-Output "summary -> $summary"
