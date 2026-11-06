<#
.SYNOPSIS
    HugEngine 门禁脚本：配置 → 构建 → shader 绑定契约对账 → 单元测试。

.DESCRIPTION
    本地和 CI 走同一条命令，避免"本地绿、CI 红"：

        pwsh Tools/ci/ci.ps1                  # Debug，全流程
        pwsh Tools/ci/ci.ps1 -Config Release  # Release（含被 Debug 断言跳过的用例）
        pwsh Tools/ci/ci.ps1 -SkipAudit       # 只构建 + 单测

    任一步失败即非 0 退出。

    【为什么单测要分 Debug/Release 跑】
    Debug 下 HE_ASSERT 会 std::abort：`SurfaceCache: 非法迁移被拒绝且不改状态` 这类
    "验证非法输入被拒绝"的用例只能在 Release 里观测（用例已按 HE_BUILD_DEBUG 分档）。
    所以 CI 上两种配置都要跑一遍才有完整覆盖。

    【为什么审计目标不放进 ALL】
    它要对全部编译入口 × 3 个后端各跑一次 slangc（约 2–3 分钟），放进日常构建太贵；
    这里显式调用 `AuditShaderBindings`，失败（契约不一致 / 槽位冲突 / 超限）即门禁失败。
#>

param(
    [string]$BuildDir = "Build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")][string]$Config = "Debug",
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipAudit,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $repo
try {
    $step = 0
    function Invoke-Step {
        param([string]$Name, [scriptblock]$Body)
        $script:step++
        Write-Host ""
        Write-Host "=== [$script:step] $Name ===" -ForegroundColor Cyan
        & $Body
        if ($LASTEXITCODE -ne 0) {
            Write-Host "!!! [$script:step] $Name 失败（exit=$LASTEXITCODE）" -ForegroundColor Red
            exit $LASTEXITCODE
        }
    }

    if (-not $SkipConfigure) {
        Invoke-Step "配置（tests=ON）" {
            cmake -S . -B $BuildDir -DHUGENGINE_BUILD_TESTS=ON
        }
    }

    if (-not $SkipBuild) {
        Invoke-Step "构建（$Config）" {
            cmake --build $BuildDir --config $Config -- /m
        }
    }

    if (-not $SkipAudit) {
        Invoke-Step "shader 绑定契约对账（Vulkan / DX12 / DX11）" {
            cmake --build $BuildDir --config $Config --target AuditShaderBindings
        }
    }

    if (-not $SkipTests) {
        $exe = Join-Path $BuildDir "bin/$Config/HugEngineTests.exe"
        if (-not (Test-Path $exe)) {
            Write-Host "!!! 找不到测试可执行文件：$exe（先配置时加 -DHUGENGINE_BUILD_TESTS=ON）" -ForegroundColor Red
            exit 1
        }
        Invoke-Step "单元测试（$Config）" {
            & $exe
        }
    }

    Write-Host ""
    Write-Host "=== 全部通过（$Config） ===" -ForegroundColor Green
}
finally {
    Pop-Location
}
