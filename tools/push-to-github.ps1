# 把本地仓库推到 GitHub（在你自己的终端里跑，凭据不经过任何第三方）
#
# 用法（PowerShell，在项目根目录）：
#   .\tools\push-to-github.ps1 AgileSlime
#   .\tools\push-to-github.ps1 -User AgileSlime
#   .\tools\push-to-github.ps1 -User AgileSlime -Repo my-xiaozhi -Branch main
#   .\tools\push-to-github.ps1 -User AgileSlime -Proxy http://127.0.0.1:7890
#   .\tools\push-to-github.ps1 -User AgileSlime -NoProxy      # 强制直连
#   .\tools\push-to-github.ps1 -User AgileSlime -DryRun       # 只配远端，不推
#
# 注意：用户名前面要有 -User，或者直接裸写（位置参数）。写成 -AgileSlime 会报
#       “找不到与参数名称匹配的参数”。
#
# 代理：git 不读 Windows 系统代理，只认 HTTP_PROXY/HTTPS_PROXY 环境变量或
#       git config http.proxy。本脚本按 参数 → 环境变量 → 系统代理 → 常见端口
#       依次探测，并且真的拿每个候选去 CONNECT 一次 github.com——
#       端口开着但返回 502/407 的假代理会被剔除。
#       选定后只以 -c 参数临时传给 git，不写进任何配置。
#
# 前置：先在网页上建好仓库（或 fork 78/xiaozhi-esp32），空仓库即可，不要勾 README。
# 第一次 push 会弹 GitHub 登录窗；如果没有弹窗，会要求输入用户名和密码——
# 此时密码位置要填 Personal Access Token（GitHub 早已不支持账号密码推送）。

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$User,
    [string]$Repo = "xiaozhi-esp32",
    [string]$Branch = "waveshare-rlcd-4.2",
    [string]$Proxy = "",
    [switch]$NoProxy,
    [switch]$Force,
    [switch]$DryRun
)

# git 往 stderr 打日志是常态（"No such remote"、"Everything up-to-date" 都走 stderr），
# 别让 Stop 把它们当成致命错误中断脚本。
$ErrorActionPreference = "Continue"

# PowerShell 5.1 默认可能只开 TLS 1.0，连 github 会握手失败
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }

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

git rev-parse --verify --quiet refs/heads/main | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "本地没有 main 分支，当前分支：" -ForegroundColor Red
    git branch --show-current
    exit 1
}

# ---------------- 代理探测 ----------------
function Test-PortOpen {
    param([string]$Url)
    try {
        $u = [Uri]$Url
        $c = New-Object System.Net.Sockets.TcpClient
        $iar = $c.BeginConnect($u.Host, $u.Port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(500)) { $c.Close(); return $false }
        $c.EndConnect($iar)
        $c.Close()
        return $true
    } catch { return $false }
}

# 真的走一次 CONNECT，返回是否可用（502/407 这类会被判死）
function Test-ProxyWorks {
    param([string]$Url)
    try {
        $req = [System.Net.HttpWebRequest]::Create("https://github.com")
        $req.Proxy = New-Object System.Net.WebProxy(([Uri]$Url), $false)
        $req.Method = "HEAD"
        # 慢代理走 CONNECT 可能要 10 秒以上，给足 20 秒，否则会把好代理误判成坏的
        $req.Timeout = 20000
        $req.ReadWriteTimeout = 20000
        $req.AllowAutoRedirect = $false
        $req.UserAgent = "git/2.20.1"
        $resp = $req.GetResponse()
        $code = [int]$resp.StatusCode
        $resp.Close()
        return ($code -lt 500)
    } catch [System.Net.WebException] {
        $r = $_.Exception.Response
        if ($null -ne $r) { return ([int]$r.StatusCode -lt 500) }
        return $false
    } catch { return $false }
}

function Get-ProxyCandidates {
    $list = New-Object System.Collections.ArrayList

    if (-not [string]::IsNullOrWhiteSpace($Proxy)) {
        [void]$list.Add(@{ Url = $Proxy; Src = "参数 -Proxy"; Must = $true })
    }
    # 系统代理优先：Windows 上 Clash / v2rayN 之类都是注册在这，比环境变量可信
    try {
        $reg = Get-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings" -ErrorAction Stop
        if ($reg.ProxyEnable -eq 1 -and -not [string]::IsNullOrWhiteSpace($reg.ProxyServer)) {
            $srv = $reg.ProxyServer
            if ($srv -match '=') {
                $kv = @{}
                foreach ($pair in $srv.Split(';')) {
                    $p2 = $pair.Split('=', 2)
                    if ($p2.Count -eq 2) { $kv[$p2[0]] = $p2[1] }
                }
                if ($kv.ContainsKey("https")) { $srv = $kv["https"] }
                elseif ($kv.ContainsKey("http")) { $srv = $kv["http"] }
            }
            if ($srv -notmatch '^[a-zA-Z]+://') { $srv = "http://$srv" }
            [void]$list.Add(@{ Url = $srv; Src = "系统代理"; Must = $false })
        }
    } catch { }
    foreach ($v in @($env:HTTPS_PROXY, $env:https_proxy, $env:HTTP_PROXY, $env:http_proxy)) {
        if (-not [string]::IsNullOrWhiteSpace($v)) { [void]$list.Add(@{ Url = $v; Src = "环境变量"; Must = $false }) }
    }
    # 兜底：常见代理客户端默认端口
    foreach ($p3 in 7890, 7897, 10809, 1080, 2080, 8889, 7891, 20171) {
        [void]$list.Add(@{ Url = "http://127.0.0.1:$p3"; Src = "端口探测"; Must = $false })
    }
    return $list
}

function Resolve-Proxy {
    $seen = @{}
    foreach ($c in (Get-ProxyCandidates)) {
        if ($seen.ContainsKey($c.Url)) { continue }
        $seen[$c.Url] = $true
        if (-not (Test-PortOpen $c.Url)) { continue }
        if (Test-ProxyWorks $c.Url) { return $c }
        Write-Host "  跳过 $($c.Url)（$($c.Src)）：端口开着但连不上 github" -ForegroundColor DarkGray
        if ($c.Must) { return $c }   # 用户明确指定的，仍然用它
    }
    return $null
}

$proxyUrl = ""
if (-not $NoProxy) {
    $found = Resolve-Proxy
    if ($null -ne $found) {
        $proxyUrl = $found.Url
        Write-Host "代理：$proxyUrl（来源：$($found.Src)）" -ForegroundColor DarkGray
    } else {
        Write-Host "代理：未找到可用的，将直连" -ForegroundColor DarkGray
    }
} else {
    Write-Host "代理：已按 -NoProxy 强制直连" -ForegroundColor DarkGray
}

# 把选定结果同步给子进程：环境变量里的旧代理会把我们的选择盖掉（即使 -c 已经设了）
foreach ($n in "HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy", "ALL_PROXY", "all_proxy") {
    if ($proxyUrl -ne "") {
        [Environment]::SetEnvironmentVariable($n, $proxyUrl, "Process")
    } else {
        [Environment]::SetEnvironmentVariable($n, $null, "Process")
    }
}

# 代理和超时都以 -c 形式临时传给 git，不落盘、不影响其他仓库
$gitOpts = @()
if ($proxyUrl -ne "") {
    $gitOpts += @("-c", "http.proxy=$proxyUrl", "-c", "https.proxy=$proxyUrl")
}
# 速度低于 1KB/s 持续 20 秒就断，避免网络半死时无限挂起
$gitOpts += @("-c", "http.lowSpeedLimit=1000", "-c", "http.lowSpeedTime=20")

$url = "https://github.com/$User/$Repo.git"
Write-Host ""
Write-Host "目标仓库：$url" -ForegroundColor Cyan
Write-Host "推送分支：本地 main -> 远端 $Branch" -ForegroundColor Cyan

# ---------------- 远端配置 ----------------
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

# ---------------- 预检：连不连得上、仓库在不在 ----------------
Write-Host ""
Write-Host "预检：读取远端分支列表..." -ForegroundColor Yellow
$heads = & git @gitOpts ls-remote --heads $url 2>&1
$lsCode = $LASTEXITCODE
$headText = ($heads | Out-String)

# 认证类错误说明"网络是通的，只是没登录"，不该拦下推送
$authIssue = $headText -match 'Authentication failed|could not read Username|terminal prompts disabled|returned error: 40[13]'

if ($lsCode -ne 0 -and -not $authIssue) {
    Write-Host "预检失败，推送已取消。git 输出：" -ForegroundColor Red
    $heads | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    Write-Host ""
    Write-Host "排查方向：" -ForegroundColor Yellow
    Write-Host "  1. 仓库不存在 —— 先去网页建空仓库，或 fork 78/xiaozhi-esp32"
    Write-Host "  2. 代理没生效 —— 加 -Proxy http://127.0.0.1:7890（换成你自己的端口）"
    Write-Host "  3. DNS 被污染的现象是长时间无响应，不是报错 —— 那就必须走代理"
    exit 1
}
if ($authIssue) {
    Write-Host "预检：网络可达，仓库需要登录（正常，push 时会弹登录窗）" -ForegroundColor DarkGray
} else {
    $cnt = @($heads | Where-Object { "$_".Trim() -ne "" }).Count
    Write-Host "预检通过：远端可访问，读到 $cnt 个分支" -ForegroundColor DarkGray
    if ($headText -match 'refs/heads/main') {
        Write-Host "  远端已有 main 分支（fork 的上游），本次推送不会动它" -ForegroundColor DarkGray
    }
}

if ($DryRun) {
    Write-Host ""
    Write-Host "DryRun：尚未推送。真实推送请去掉 -DryRun 重跑。" -ForegroundColor Yellow
    Write-Host "将执行：git push -u origin main:refs/heads/$Branch" -ForegroundColor DarkGray
    exit 0
}

# ---------------- 推送 ----------------
$refspec = "main:refs/heads/$Branch"
Write-Host ""
Write-Host "开始推送（首次会弹 GitHub 登录窗，密码栏填 Personal Access Token）..." -ForegroundColor Yellow
if ($Force) {
    & git @gitOpts push -u origin $refspec --force
} else {
    & git @gitOpts push -u origin $refspec
}
$code = $LASTEXITCODE

if ($code -eq 0) {
    Write-Host ""
    Write-Host "完成：https://github.com/$User/$Repo/tree/$Branch" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "推送失败（退出码 $code）。常见原因：" -ForegroundColor Red
    Write-Host "  1. 认证失败 —— 密码处填 Personal Access Token，不是账号密码"
    Write-Host "  2. 分支已存在且内容不同 —— 加 -Force 覆盖"
    Write-Host "  3. 网络不稳 —— 换代理：-Proxy http://127.0.0.1:端口"
    if ($headText -match 'terminal prompts disabled|could not read Username|Logon failed') {
        Write-Host ""
        Write-Host "  ↑ 这次是「登录窗弹不出来」。脚本必须在交互式 PowerShell 窗口里跑，" -ForegroundColor Yellow
        Write-Host "    在 IDE 内置终端 / 计划任务 / 自动化里运行都会被判成非交互。" -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "自检命令：git -c http.proxy=$proxyUrl ls-remote $url" -ForegroundColor DarkGray
}
