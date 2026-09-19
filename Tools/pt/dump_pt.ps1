# ============================================================
# Tools/pt/dump_pt.ps1 — PT 参考图落盘 + 可复现性读数（PT 任务 5）
#
# 一条命令跑完全流程：
#   1. 每次运行使用**独立的 cfg 私有副本**（示例退出会回写 cfg，禁止复用同一文件）
#   2. 设置 HE_DUMP_PT / HE_DUMP_PT_FRAME / HE_CFG 启动 05.Sponza-PathTracing
#   3. stale-dump 守卫：运行前删除旧的 pt_<tag>_*.f16，运行后要求文件存在且比启动时间新
#   4. 采样帧到达后示例自行落盘并请求退出（脚本无需超时等待，超时仅作兜底）
#   5. 全部运行结束后调用 analyze_pt.py 出读数与离散度判据
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File Tools/pt/dump_pt.ps1                 # 默认 3 次、第 60 帧
#   powershell -ExecutionPolicy Bypass -File Tools/pt/dump_pt.ps1 -Tag base -Runs 4 -Frame 120
#   powershell -ExecutionPolicy Bypass -File Tools/pt/dump_pt.ps1 -Tag a -Runs 1 -SkipAnalyze
#   powershell -ExecutionPolicy Bypass -File Tools/pt/dump_pt.ps1 -Tag pt -Runs 1 -Mode pt      # 参考渲染器
#   powershell -ExecutionPolicy Bypass -File Tools/pt/dump_pt.ps1 -Tag df -Runs 1 -Mode deferred  # GI 层栈（对照）
#   powershell -ExecutionPolicy Bypass -File Tools/pt/dump_pt.ps1 -Tag spp4 -Runs 1 -Frame 120 -Spp 4   # SPP 维收敛
#
# 输出（build/verify/）：
#   pt_<tag>_runN_{hdr,depth,normal,albedo}.f16 + pt_<tag>_runN_meta.txt + _camera.txt
#   pt_<tag>_runN.log（示例日志）
# ============================================================
param(
    [string]$Tag = 'repro',
    [int]$Runs = 3,
    [int]$Frame = 60,
    [string]$Config = 'Release',
    [ValidateSet('pt','deferred')][string]$Mode = 'pt',   # pt=参考渲染器；deferred=GI 层栈（对照）
    [int]$Spp = 0,                 # >0 时把私有 cfg 副本的 pt_spp 改成该值（SPP 维收敛实验）
    [string[]]$Set = @(),          # 额外覆盖的 cfg 键，形如 'pt_bounces=8'
    [int]$TimeoutSec = 900,
    [switch]$SkipAnalyze
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # Tools/pt -> 仓库根
Set-Location $root

$exe = Join-Path $root "build\bin\$Config\05.Sponza-PathTracing.exe"
if (-not (Test-Path $exe)) { throw "缺少可执行文件: $exe（先构建 05.Sponza-PathTracing）" }

$out = Join-Path $root 'build\verify'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$baseCfg = Join-Path $root 'Content\Config\05_Sponza-PathTracing.cfg'

$tags = @()
for ($i = 1; $i -le $Runs; $i++) {
    $runTag = "${Tag}_run$i"
    $tags += $runTag

    # ── 每次运行一份私有 cfg 副本（示例退出会回写它）──
    $runCfg = Join-Path $out "pt_cfg_$runTag.cfg"
    if (Test-Path $baseCfg) {
        Copy-Item -Force $baseCfg $runCfg
    } else {
        Remove-Item -Force $runCfg -ErrorAction SilentlyContinue   # 没有基准则用示例默认值
    }
    # 实验参数只改进程的私有副本：SPP 维收敛实验与任意键覆盖（经 set_cfg.py）
    if (Test-Path $runCfg) {
        $overrides = @()
        if ($Spp -gt 0) { $overrides += "pt_spp=$Spp" }
        $overrides += $Set
        if ($overrides.Count -gt 0) {
            & python (Join-Path $root 'Tools\pt\set_cfg.py') $runCfg @overrides | Out-Null
        }
    }

    # ── stale-dump 守卫（1/2）：先清掉该 tag 的旧产物 ──
    Get-ChildItem -Path $out -Filter "pt_${runTag}_*" -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    $start = Get-Date
    $env:HE_DUMP_PT       = $runTag
    $env:HE_DUMP_PT_FRAME = "$Frame"
    $env:HE_CFG           = $runCfg
    $env:HE_DUMP_MODE     = $Mode

    $log = Join-Path $out "pt_${runTag}.log"
    $err = Join-Path $out "pt_${runTag}.err.log"
    Write-Output "[dump_pt] run $i/$Runs  tag=$runTag  frame=$Frame  mode=$Mode  spp=$Spp"
    $proc = Start-Process -FilePath $exe -RedirectStandardOutput $log -RedirectStandardError $err -PassThru
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        $proc.Kill()
        Write-Warning "[dump_pt] run $i 超时 ${TimeoutSec}s，已终止"
    }

    Remove-Item Env:HE_DUMP_PT, Env:HE_DUMP_PT_FRAME, Env:HE_CFG, Env:HE_DUMP_MODE -ErrorAction SilentlyContinue

    # ── stale-dump 守卫（2/2）：产物必须存在且比本次启动新 ──
    $dump = Join-Path $out "pt_${runTag}_hdr.f16"
    if (-not (Test-Path $dump)) { throw "[dump_pt] run $i 没有落盘: $dump（看日志 $log）" }
    if ((Get-Item $dump).LastWriteTime -lt $start) {
        throw "[dump_pt] run $i 的落盘文件是旧的（stale）：$dump —— 本次运行没跑到采样帧"
    }
    Write-Output "[dump_pt] run $i ok -> $dump"
}

if (-not $SkipAnalyze) {
    Write-Output ''
    Write-Output '[dump_pt] 可复现性读数（离散度判据）：'
    & python (Join-Path $root 'Tools\pt\analyze_pt.py') @tags
    if ($LASTEXITCODE -ne 0) { throw "[dump_pt] 读数未通过阈值，见上方输出" }
}

Write-Output ''
Write-Output "[dump_pt] 完成：$Runs 次运行，tag 前缀 '$Tag'"
Write-Output "[dump_pt] 两版对照：python Tools/pt/analyze_pt.py --diff <tagA> <tagB>"
