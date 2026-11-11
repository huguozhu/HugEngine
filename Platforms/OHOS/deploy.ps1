# ============================================================
#  deploy.ps1 —— 一键构建 hap、安装到真机、启动并抓日志
#
#  前置：
#    · DevEco Studio 已在该项目上配置**自动签名**（Project Structure → Signing Configs）
#    · 手机已连接并授权调试（hdc list targets 显示设备号）
#
#  用法：
#    pwsh -File deploy.ps1            # 构建 + 安装 + 启动
#    pwsh -File deploy.ps1 -BuildOnly # 只构建
#    pwsh -File deploy.ps1 -Log       # 装完并持续打印 HugEngine 日志
# ============================================================
param(
    [switch]$BuildOnly,
    [switch]$Log
)

$ErrorActionPreference = 'Stop'

$deveco  = 'C:\Program Files\Huawei\DevEco Studio'
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$hvigor  = Join-Path $deveco 'tools\hvigor\bin\hvigorw.bat'
$hdc     = Join-Path $deveco 'sdk\default\openharmony\toolchains\hdc.exe'
$bundle  = 'com.hugengine.sample02'
$ability = 'EntryAbility'

$env:DEVECO_SDK_HOME = Join-Path $deveco 'sdk'
$env:PATH = (Join-Path $deveco 'jbr\bin') + ';' + (Join-Path $deveco 'tools\node') + ';' + $env:PATH

# ---- 同步 rawfile 资产（Sponza 场景 + HDR 天空盒；体积大，不入库）----
$repo    = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $root))
$rawContent = Join-Path $root 'entry\src\main\resources\rawfile\content'
function Sync-Assets {
    $sponzaSrc = Join-Path $repo 'Content\gltf\Sponza'
    $sponzaDst = Join-Path $rawContent 'gltf\Sponza'
    if ((Test-Path $sponzaSrc) -and -not (Test-Path $sponzaDst)) {
        Write-Host '== 拷贝 Sponza 资产到 rawfile ==' -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sponzaDst) | Out-Null
        Copy-Item $sponzaSrc $sponzaDst -Recurse -Force
    }
    $hdrSrc = Join-Path $repo 'Content\Textures\skybox.hdr'
    $hdrDst = Join-Path $rawContent 'Textures\skybox.hdr'
    if ((Test-Path $hdrSrc) -and -not (Test-Path $hdrDst)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $hdrDst) | Out-Null
        Copy-Item $hdrSrc $hdrDst -Force
    }
}
Sync-Assets
Write-Host '== 构建 hap ==' -ForegroundColor Cyan
Push-Location $root
try {
    & $hvigor --mode module -p product=default -p module=entry@default assembleHap --no-daemon
    if ($LASTEXITCODE -ne 0) { throw "hvigor 构建失败（退出码 $LASTEXITCODE），详见 .hvigor\outputs\build-logs\build.log" }
} finally {
    Pop-Location
}

$outDir = Join-Path $root 'entry\build\default\outputs\default'
$hap = Get-ChildItem $outDir -Filter '*.hap' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $hap) { throw "没有找到 hap 产物：$outDir" }
Write-Host ("hap: {0} ({1:N1} MB)" -f $hap.Name, ($hap.Length / 1MB)) -ForegroundColor Green

if ($hap.Name -match 'unsigned') {
    Write-Host '⚠ 产物是未签名的：请先在 DevEco 里打开本项目并勾选自动签名，否则无法安装。' -ForegroundColor Yellow
    if ($BuildOnly) { return }
}

if ($BuildOnly) { return }

Write-Host '== 安装 ==' -ForegroundColor Cyan
& $hdc install -r $hap.FullName
if ($LASTEXITCODE -ne 0) { throw "安装失败（退出码 $LASTEXITCODE）" }

Write-Host '== 启动 ==' -ForegroundColor Cyan
& $hdc shell aa start -a $ability -b $bundle

if ($Log) {
    Write-Host '== 日志（Ctrl+C 结束）==' -ForegroundColor Cyan
    & $hdc shell hilog | Select-String -Pattern 'HugEngine|HugEngineNative|\bentry\b'
}

# T13：Nanite 离线产物（Content/Processed/*.nanite）
# 【为什么也同步】鸿蒙端一旦开启 nanite_offline，资产要从 rawfile 解压到沙箱；
# 没有这些文件就只能走运行时构建（日志会明确回退原因）。
$processedSrc = Join-Path $repo 'Content\Processed'
$processedDst = Join-Path $rawContent 'Processed'
if (Test-Path $processedSrc) {
    New-Item -ItemType Directory -Force -Path $processedDst | Out-Null
    Copy-Item (Join-Path $processedSrc '*.nanite') $processedDst -Force -ErrorAction SilentlyContinue
    Write-Host "已同步 Content/Processed/*.nanite"
} else {
    Write-Warning "缺少 Content/Processed（先跑 ProcessMeshes）；鸿蒙将回退运行时构建"
}