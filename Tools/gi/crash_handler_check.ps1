# ============================================================
# Crash-handler / exit-crash check (docs section 10.2 task 2, defect family "D1")
#
# PART A -- the crash handler self-test must produce a usable report.
#   `HE_CRASH_TEST=1` makes 06.GILab dereference a null pointer on frame 3.
#   Before this was fixed the process *silently hung*: no report, no minidump, and the
#   WER-only fingerprint was "faulting module unknown, offset 0" -- i.e. no evidence at all.
#   Now it must: exit within the timeout, write a minidump, and write a staged report with
#   the exception code, the null target address, and at least one SYMBOLICATED frame
#   (`06.GILab!... [....cpp:NNN]`) -- that last one is what makes the report worth having.
#   The report carries ASCII phase tags (`[crash phase N/4]`) exactly so that tooling can
#   check completeness without depending on the console code page (PowerShell 5.1 reads
#   this .ps1 as ANSI, so a Chinese regex here would silently never match).
#
# PART B -- the exit-crash regression (task 2 root cause).
#   The Debug unit-test executable used to crash at exit (0xC0000005, RIP=0) whenever NOT all
#   test cases ran (161/161 single-case runs, and `--no-run`), while a full run exited cleanly.
#   Root cause: `he::physics::s_World` is a process-lifetime static holding a Jolt
#   `PhysicsSystem`; Jolt's global allocator/factory were only registered lazily inside
#   `PhysicsWorld::Initialize()`, so a run that never initialised physics called a NULL
#   `JPH::Free` from the static destructor (`delete[]` -> jump to address 0). Fixed by
#   registering in a base class (`JoltRuntimeGuard`) so registration always precedes any Jolt
#   member construction. PART B asserts the regression stays fixed: every run shape must exit
#   0 and must not record a crash (the crash log itself is always created at install time,
#   so the assertion is on its CONTENT -- no 0xC0000005 -- not on its existence).
#
# NOTE: ASCII-only on purpose -- Windows PowerShell 5.1 reads .ps1 as ANSI.
#
# Usage: powershell -File Tools/gi/crash_handler_check.ps1 [-TimeoutSec 60] [-Config Release]
# Exit code: 0 = all assertions pass, 1 = at least one failed.
# ============================================================
param(
    [int]$TimeoutSec = 60,
    [string]$Config = 'Release',
    [string]$TestsExe = ''
)
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe     = Join-Path $root "Build\bin\$Config\06.GILab.exe"
$outDir  = Join-Path $root 'Build\verify'
$baseCfg = Join-Path $root 'Content\Config\06_GILab.cfg'
$dump    = Join-Path $root "Build\bin\$Config\06.GILab_crash.dmp"
$report  = Join-Path $root 'Content\Config\06_GILab_crash.log'
if ($TestsExe -eq '') { $TestsExe = Join-Path $root 'build\bin\Debug\HugEngineTests.exe' }
$testsCrashLog = Join-Path $root 'Content\Config\HugEngineTests_crash.log'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not (Test-Path $exe))     { throw "missing exe: $exe" }
if (-not (Test-Path $baseCfg)) { throw "missing baseline config: $baseCfg" }

$failed = 0
function Assert-That([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Output "  [PASS] $name -- $detail" }
    else     { Write-Output "  [FAIL] $name -- $detail"; $script:failed++ }
}

# ---------------- PART A: crash-handler self-test ----------------
Write-Output "=== PART A: HE_CRASH_TEST self-test (06.GILab, $Config) ==="

# private cfg copy: the sample rewrites the file it is pointed at
$cfg = Join-Path $outDir 'chk_crash_selftest.cfg'
Copy-Item $baseCfg $cfg -Force
Remove-Item $dump, $report -Force -ErrorAction SilentlyContinue

$outLog = Join-Path $outDir 'chk_crash_selftest.out'
$errLog = Join-Path $outDir 'chk_crash_selftest.err'
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName               = $exe
$psi.WorkingDirectory       = $root
$psi.UseShellExecute        = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.EnvironmentVariables['HE_GILAB_CONFIG']       = $cfg
$psi.EnvironmentVariables['HE_CRASH_TEST']         = '1'
$psi.EnvironmentVariables['HE_CRASH_NO_WER']       = '1'   # bounded exit: WER alone takes ~62 s here
$psi.EnvironmentVariables['HE_CRASH_WATCHDOG_SEC'] = '30'
$t0 = Get-Date
$proc = [System.Diagnostics.Process]::Start($psi)
$so = $proc.StandardOutput.ReadToEndAsync()
$se = $proc.StandardError.ReadToEndAsync()
$done = $proc.WaitForExit($TimeoutSec * 1000)
$secs = [int]((Get-Date) - $t0).TotalSeconds
if (-not $done) { $proc.Kill() }
$exitCode = $null
if ($done) { $exitCode = $proc.ExitCode }
[System.IO.File]::WriteAllText($outLog, $so.Result)
[System.IO.File]::WriteAllText($errLog, $se.Result)

Assert-That "process terminates (no silent hang)" $done `
    "exited=$done after ${secs}s (timeout ${TimeoutSec}s)"
if ($done) {
    Assert-That "exit code signals a crash" ($exitCode -ne 0) "exit code=$exitCode (2 expected with HE_CRASH_NO_WER=1)"
}

$dmpOk = (Test-Path $dump) -and ((Get-Item $dump).LastWriteTime -ge $t0) -and ((Get-Item $dump).Length -ge 1MB)
if (Test-Path $dump) {
    Assert-That "minidump written and fresh" $dmpOk `
        "$dump ($([math]::Round((Get-Item $dump).Length/1MB,1)) MB)"
} else {
    Assert-That "minidump written and fresh" $false "$dump missing"
}

$txt = ''
if (Test-Path $report) { $txt = [System.IO.File]::ReadAllText($report) }
Assert-That "crash report written and fresh" `
    ((Test-Path $report) -and ((Get-Item $report).LastWriteTime -ge $t0)) `
    "$report ($($txt.Length) chars)"
$staged = ($txt -match 'crash phase 1/4') -and ($txt -match 'crash phase 2/4') -and `
          ($txt -match 'crash phase 3/4') -and ($txt -match 'crash phase 4/4')
Assert-That "report is staged (phases 1..4)" $staged "ASCII phase tags present"
Assert-That "report names the exception and the null target" `
    ($txt -match '0xC0000005' -and $txt -match '0x0000000000000000') `
    "0xC0000005 + target 0x0"
$sym = [regex]::Match($txt, '06\.GILab!\S+.*\.cpp:\d+')
Assert-That "report has a symbolicated frame with source line" $sym.Success `
    $(if ($sym.Success) { $sym.Value } else { "no '06.GILab!... [*.cpp:NNN]' frame found" })

# ---------------- PART B: exit-crash regression ----------------
Write-Output ""
Write-Output "=== PART B: exit-crash regression (Debug unit tests) ==="
if (-not (Test-Path $TestsExe)) {
    Write-Output "  [SKIP] $TestsExe not built -- PART B skipped"
} else {
    $cases = @(
        @{ name = 'no-run';   args = @('--no-run') },
        @{ name = 'one case'; args = @('--test-case=*FitRSMFrustumToBounds*') },
        @{ name = 'full run'; args = @() }
    )
    foreach ($c in $cases) {
        Remove-Item $testsCrashLog -Force -ErrorAction SilentlyContinue
        & $TestsExe @($c.args) *> $null
        $code = $LASTEXITCODE
        # 安装处理器时就会写一行"已安装"，所以判据是**内容里没有崩溃**，不是文件不存在
        $crashText = ''
        if (Test-Path $testsCrashLog) { $crashText = [System.IO.File]::ReadAllText($testsCrashLog) }
        $crashed = $crashText -match '0xC0000005'
        Assert-That "unit tests exit 0 ($($c.name))" ($code -eq 0) "exit code=$code"
        Assert-That "no crash recorded ($($c.name))" (-not $crashed) `
            $(if ($crashed) { "crash report recorded -> exit-time crash is back" } else { "clean teardown" })
    }
}

Write-Output ""
if ($failed -gt 0) { Write-Output "!! $failed check(s) FAILED"; exit 1 }
Write-Output "all crash-handler checks passed"
exit 0
