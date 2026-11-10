# ============================================================
#  Platforms/Android/sync-assets.ps1
#
#  把仓库 Content/ 里 01/02 需要的资产同步到 APK 的 assets/content。
#
#  为什么必须同步：
#    引擎的 VFS 是"文件路径"语义（glTF 相对路径、stb 读整文件），APK 里的 assets
#    不能当普通目录随机访问，所以 NativeEntry 会把 assets/content 解压到沙箱。
#
#  用法：pwsh -File Platforms/Android/sync-assets.ps1
# ============================================================

$ErrorActionPreference = 'Stop'

$repoRoot  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # Platforms/Android → 仓库根
$contentSrc = Join-Path $repoRoot 'Content'
$assetsDst  = Join-Path $PSScriptRoot 'app\src\main\assets\content'

# 01/02 实际用到的资产（Sponza 约 56MB，暂不随 APK）
$items = @(
    'Textures\skybox\daylight0.png',
    'Textures\skybox\daylight1.png',
    'Textures\skybox\daylight2.png',
    'Textures\skybox\daylight3.png',
    'Textures\skybox\daylight4.png',
    'Textures\skybox\daylight5.png'
)

foreach ($rel in $items) {
    $src = Join-Path $contentSrc $rel
    if (-not (Test-Path $src)) { Write-Warning "缺少资产: $rel"; continue }
    $dst = Join-Path $assetsDst $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item $src $dst -Force
    Write-Host "同步 $rel"
}

# 字体（可选）：随包思源黑体时一并拷进去；没有则用设备系统字体
$fontSrc = Join-Path $contentSrc 'font\NotoSansSC-Regular.otf'
if (Test-Path $fontSrc) {
    $fontDst = Join-Path $assetsDst 'font\NotoSansSC-Regular.otf'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $fontDst) | Out-Null
    Copy-Item $fontSrc $fontDst -Force
    Write-Host "同步 font/NotoSansSC-Regular.otf"
}

$total = (Get-ChildItem $assetsDst -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("assets/content 合计 {0:N1} MB" -f ($total / 1MB))
