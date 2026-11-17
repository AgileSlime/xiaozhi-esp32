# 把本地仓库推到 GitHub（在你自己的终端里跑，凭据不经过任何第三方）
#
# 用法（PowerShell，在项目根目录）：
#   .\tools\push-to-github.ps1 AgileSlime
#   .\tools\push-to-github.ps1 -User AgileSlime
#   .\tools\push-to-github.ps1 -User AgileSlime -Repo my-xiaozhi -Branch main
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
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not (Test-Path ".git")) {
    Write-Host "当前目录不是 git 仓库：$root" -ForegroundColor Red
    exit 1
}

$url = "https://github.com/$User/$Repo.git"
Write-Host "目标仓库：$url" -ForegroundColor Cyan
Write-Host "推送分支：main -> $Branch" -ForegroundColor Cyan

git remote remove origin 2>$null
git remote add origin $url

Write-Host "`n开始推送（首次会要求登录）..." -ForegroundColor Yellow
if ($Force) {
    git push -u origin "main:refs/heads/$Branch" --force
} else {
    git push -u origin "main:refs/heads/$Branch"
}

if ($LASTEXITCODE -eq 0) {
    Write-Host "`n完成：https://github.com/$User/$Repo/tree/$Branch" -ForegroundColor Green
} else {
    Write-Host "`n推送失败。常见原因：" -ForegroundColor Red
    Write-Host "  1. 仓库不存在 —— 先去网页建一个空仓库（不要勾 README/license）"
    Write-Host "  2. 认证失败 —— 密码处填 Personal Access Token，不是账号密码"
    Write-Host "  3. 网络不通 —— 需要能访问 github.com"
    Write-Host "  4. 分支已存在且内容不同 —— 加 -Force 覆盖"
}
