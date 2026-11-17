# 把本地仓库推到 GitHub（在你自己的终端里跑，凭据不经过任何第三方）
#
# 用法（PowerShell，在项目根目录）：
#   .\tools\push-to-github.ps1 AgileSlime
#   .\tools\push-to-github.ps1 -User AgileSlime
#   .\tools\push-to-github.ps1 -User AgileSlime -Repo my-xiaozhi -Branch main
#   .\tools\push-to-github.ps1 -User AgileSlime -DryRun     # 只配远端，不推
#
# 注意：用户名前面要有 -User，或者直接裸写（位置参数）。写成 -AgileSlime 会报
#       “找不到与参数名称匹配的参数”。
#
# 前置：先在网页上建好仓库（或 fork 78/xiaozhi-esp32），空仓库即可，不要勾 README。
# 第一次 push 会弹 GitHub 登录窗；如果没有弹窗，会要求输入用户名和密码——
# 此时密码位置要填 Personal Access Token（GitHub 早已不支持账号密码推送）。

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$User,
    [string]$Repo = "xiaozhi-esp32",
    [string]$Branch = "waveshare-rlcd-4.2",
    [switch]$Force,
    [switch]$DryRun
)

# git 往 stderr 打日志是常态（"No such remote"、"Everything up-to-date" 都走 stderr），
# 别让 Stop 把它们当成致命错误中断脚本。
$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path ".git")) {
    Write-Host "当前目录不是 git 仓库：$root" -ForegroundColor Red
    exit 1
}

$User = $User.Trim().Trim('/')
if ([string]::IsNullOrWhiteSpace($User) -or $User -match '[\s/]') {
    Write-Host "用户名不合法：'$User'" -ForegroundColor Red
    exit 1
}

# 本地必须有 main 分支
git rev-parse --verify --quiet refs/heads/main | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "本地没有 main 分支，当前分支：" -ForegroundColor Red
    git branch --show-current
    exit 1
}

$url = "https://github.com/$User/$Repo.git"
Write-Host ""
Write-Host "目标仓库：$url" -ForegroundColor Cyan
Write-Host "推送分支：本地 main -> 远端 $Branch" -ForegroundColor Cyan
Write-Host ""

# ---- 远端配置：有 origin 就改地址，没有就新增（无脑 remove 会 fatal）----
$remotes = @(git remote 2>$null)
if ($remotes -contains "origin") {
    git remote set-url origin $url 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        git remote remove origin 2>&1 | Out-Null
        git remote add origin $url 2>&1 | Out-Null
    }
} else {
    git remote add origin $url 2>&1 | Out-Null
}

if ($LASTEXITCODE -ne 0) {
    Write-Host "配置远端失败，当前远端列表：" -ForegroundColor Red
    git remote -v
    exit 1
}
Write-Host "远端已配置：origin -> $url" -ForegroundColor DarkGray

if ($DryRun) {
    Write-Host ""
    Write-Host "DryRun：尚未推送。真实推送请去掉 -DryRun 重跑。" -ForegroundColor Yellow
    Write-Host "将执行：git push -u origin main:refs/heads/$Branch" -ForegroundColor DarkGray
    exit 0
}

# ---- 推送 ----
$refspec = "main:refs/heads/$Branch"
Write-Host ""
Write-Host "开始推送（首次会要求登录）..." -ForegroundColor Yellow
if ($Force) {
    git push -u origin $refspec --force
} else {
    git push -u origin $refspec
}
$code = $LASTEXITCODE

if ($code -eq 0) {
    Write-Host ""
    Write-Host "完成：https://github.com/$User/$Repo/tree/$Branch" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "推送失败（退出码 $code）。常见原因：" -ForegroundColor Red
    Write-Host "  1. 仓库不存在 —— 先去网页建一个空仓库（不要勾 README/license）"
    Write-Host "  2. 认证失败 —— 密码处填 Personal Access Token，不是账号密码"
    Write-Host "  3. 网络不通 —— 需要能访问 github.com，必要时先设 https_proxy"
    Write-Host "  4. 分支已存在且内容不同 —— 加 -Force 覆盖"
    Write-Host ""
    Write-Host "自检命令：git ls-remote $url" -ForegroundColor DarkGray
}
