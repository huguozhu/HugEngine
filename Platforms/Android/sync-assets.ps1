# ============================================================
#  Platforms/Android/sync-assets.ps1
#
#  把仓库 Content/ 里移动端样例需要的资产同步到 APK 的 assets/content。
#
#  为什么必须同步：
#    引擎的 VFS 是"文件路径"语义（glTF 相对路径、stb 读整文件），APK 里的 assets
#    不能当普通目录随机访问，所以 NativeEntry 会把 assets/content 解压到沙箱。
#
#  01/02：天空盒 6 张 PNG（约 4 MB）
#  03/04：另外需要 Sponza glTF（约 51 MB）与 skybox.hdr（约 1.5 MB）
#
#  用法：powershell -ExecutionPolicy Bypass -File Platforms/Android/sync-assets.ps1
# ============================================================

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # Platforms/Android → 仓库根
$contentSrc = Join-Path $repoRoot 'Content'
$assetsDst  = Join-Path $PSScriptRoot 'app\src\main\assets\content'

# ---- 01/02：天空盒 6 面 PNG ----
$files = @(
    'Textures\skybox\daylight0.png',
    'Textures\skybox\daylight1.png',
    'Textures\skybox\daylight2.png',
    'Textures\skybox\daylight3.png',
    'Textures\skybox\daylight4.png',
    'Textures\skybox\daylight5.png',
    # 03/04：HDR 全景（03/04 的天空盒来源）
    'Textures\skybox.hdr'
)

# ---- 03/04：Sponza glTF（整目录，含贴图）----
$dirs = @(
    'gltf\Sponza'
)

foreach ($rel in $files) {
    $src = Join-Path $contentSrc $rel
    if (-not (Test-Path $src)) { Write-Warning "缺少资产: $rel"; continue }
    $dst = Join-Path $assetsDst $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item $src $dst -Force
    Write-Host "同步 $rel"
}

foreach ($rel in $dirs) {
    $src = Join-Path $contentSrc $rel
    if (-not (Test-Path $src)) { Write-Warning "缺少资产目录: $rel"; continue }
    $dst = Join-Path $assetsDst $rel
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Copy-Item (Join-Path $src '*') $dst -Recurse -Force
    Write-Host "同步 $rel\（递归）"
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

# ---- 生成资产清单 ----
# AAssetManager 的目录枚举只返回文件、不返回子目录，递归解包会在嵌套目录
# （gltf/Sponza/glTF/...）处断掉。这里在构建期生成清单，运行期按清单逐个拷贝。
$assetsRoot   = Join-Path $PSScriptRoot 'app\src\main\assets'
$contentRoot  = Join-Path $assetsRoot 'content'
$manifestPath = Join-Path $assetsRoot 'asset_manifest.txt'
$lines = @()
foreach ($f in (Get-ChildItem $contentRoot -Recurse -File)) {
    $rel = $f.FullName.Substring($contentRoot.Length + 1)
    $rel = $rel -replace '\\', '/'
    $lines += ('content/' + $rel)
}
[System.IO.File]::WriteAllLines($manifestPath, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host $("资产清单: {0} 个文件" -f $lines.Count)
