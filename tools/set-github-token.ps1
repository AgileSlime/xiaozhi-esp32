# 把 GitHub Token 存进 Windows 凭据管理器（wincred），供 push-to-github.ps1 使用
#
# 为什么需要这个：git 自己有密码提示框，但那是底层 read，某些 Windows 终端会把粘贴吞掉，
# 而且输入过程完全不回显，很容易以为没粘上。这个脚本用 PowerShell 的输入框，
# 粘贴稳定（显示为圆点），并且会把 token 直接写进凭据管理器，之后 push 不再问。
#
# 用法（在项目根目录）：
#   .\tools\set-github-token.ps1                     # 交互式粘贴（显示圆点）
#   .\tools\set-github-token.ps1 -FromClipboard      # 直接从剪贴板读（先在浏览器复制好）
#   .\tools\set-github-token.ps1 -User AgileSlime -Repo xiaozhi-esp32
#
# Token 在此生成：https://github.com/settings/tokens  → Generate new token (classic)
#   勾选 repo 权限，过期时间随意，生成后立刻复制（关掉页面就看不到了）。

param(
    [string]$User = "AgileSlime",
    [string]$Repo = "xiaozhi-esp32",
    [string]$Branch = "waveshare-rlcd-4.2",
    [switch]$FromClipboard,
    [switch]$NoVerify
)

$ErrorActionPreference = "Continue"
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }

$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root ".git"))) {
    Write-Host "当前目录不是 git 仓库：$root" -ForegroundColor Red
    exit 1
}
Set-Location $root

# ---- 与 push 脚本保持一致：绕开老版 GCM，凭据走 wincred ----
$env:GIT_CONFIG_NOSYSTEM = "1"
$helpers = @(& git config --local --get-all credential.helper 2>$null)
if (-not ($helpers -contains "wincred")) {
    & git config --local --add credential.helper wincred 2>&1 | Out-Null
}

Write-Host ""
Write-Host "准备为 $User@github.com 写入凭据" -ForegroundColor Cyan

# ---- 取 token ----
$pat = ""
if ($FromClipboard) {
    try { $pat = (Get-Clipboard -Raw -ErrorAction Stop) } catch { $pat = "" }
    if ([string]::IsNullOrWhiteSpace($pat)) {
        Write-Host "剪贴板是空的。先在浏览器里复制 token，再重跑本脚本。" -ForegroundColor Red
        exit 1
    }
    Write-Host "已从剪贴板读取（长度 $($pat.Trim().Length)）" -ForegroundColor DarkGray
} else {
    Write-Host "粘贴 token 后回车（输入时只显示圆点，这是正常的）" -ForegroundColor Yellow
    $secure = Read-Host "Token" -AsSecureString
    $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        $pat = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
}

# 粘贴常带换行或行尾空格，全部去掉
$pat = ($pat -replace '\s', '')
if ([string]::IsNullOrWhiteSpace($pat)) {
    Write-Host "没有读到内容，已退出。" -ForegroundColor Red
    exit 1
}

if ($pat.Length -lt 30) {
    Write-Host "长度只有 $($pat.Length)，不像是完整 token（经典 token 是 ghp_ 开头，40 位）。" -ForegroundColor Red
    Write-Host "确认复制完整后重试。" -ForegroundColor DarkGray
    exit 1
}
if ($pat -match '^(ghp_|github_pat_|gho_|ghu_|ghs_)') {
    Write-Host "格式看起来正常（$($pat.Substring(0,4))...，共 $($pat.Length) 位）" -ForegroundColor DarkGray
} else {
    Write-Host "注意：不以 ghp_ / github_pat_ 开头，请确认不是粘到了别的东西" -ForegroundColor Yellow
}

# ---- 写入凭据管理器 ----
# 走 git 自己的 credential approve，它会调用 wincred 存进 Windows 凭据管理器
$payload = @("protocol=https", "host=github.com", "username=$User", "password=$pat", "") -join "`n"
$payload | & git credential approve 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "写入凭据失败（退出码 $LASTEXITCODE）。" -ForegroundColor Red
    exit 1
}
$pat = ""
Write-Host "已写入 Windows 凭据管理器。" -ForegroundColor Green

# ---- 验证 ----
if ($NoVerify) {
    Write-Host ""
    Write-Host "已跳过验证。现在可以跑：.\tools\push-to-github.ps1 -User $User" -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "验证：用这份凭据做一次推送握手（不写远端）..." -ForegroundColor Yellow
$pushScript = Join-Path $PSScriptRoot "push-to-github.ps1"
& $pushScript -User $User -Repo $Repo -Branch $Branch -DryRun
$code = $LASTEXITCODE

Write-Host ""
if ($code -eq 0) {
    Write-Host "凭据可用。现在真推：" -ForegroundColor Green
    Write-Host "  .\tools\push-to-github.ps1 -User $User" -ForegroundColor Green
} else {
    Write-Host "验证没通过。若提示 401/403，说明 token 权限不足或已过期；" -ForegroundColor Red
    Write-Host "重新生成时务必勾选 repo 权限。" -ForegroundColor Red
}
exit $code
