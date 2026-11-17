# 抓取 ESP32 开发板串口日志并落盘
#
# 不依赖 ESP-IDF 环境（不需要 idf.py / 那串 PATH），直接用 .NET 的串口类读。
# 适合：开机日志、运行中报错、偶发重启的现场，以及需要留档给别人的场景。
#
# 用法（在项目根目录）：
#   .\tools\capture-log.ps1                 # 自动找板子，日志存 logs\serial-*.log
#   .\tools\capture-log.ps1 -List           # 只列出串口，不抓
#   .\tools\capture-log.ps1 -Port COM3      # 指定串口
#   .\tools\capture-log.ps1 -Reset          # 先复位板子，从第一条 boot 日志开始抓
#   .\tools\capture-log.ps1 -Seconds 30     # 抓 30 秒自动停
#   .\tools\capture-log.ps1 -Timestamp      # 每行加时间戳（排查时序问题很有用）
#
# 停止：Ctrl+C。文件是逐块 flush 的，Ctrl+C 不会丢已抓到的内容。

param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [string]$Out = "",
    [switch]$Reset,
    [switch]$Timestamp,
    [int]$Seconds = 0,
    [switch]$List
)

$ErrorActionPreference = "Continue"

# ESP32-S3 常用 USB 转串口芯片，用于自动识别
$knownChips = @("CH343", "CH340", "CH9102", "CP210", "FTDI", "Silicon Labs", "USB-SERIAL", "USB JTAG", "USB 串行")

function Get-SerialPorts {
    $result = @()
    try {
        $devs = Get-CimInstance Win32_PnPEntity -ErrorAction Stop | Where-Object { $_.Name -match '\(COM(\d+)\)' }
        foreach ($d in $devs) {
            if ($d.Name -match '\(COM(\d+)\)') {
                $result += [pscustomobject]@{ Port = "COM$($Matches[1])"; Desc = $d.Name }
            }
        }
    } catch {
        foreach ($p in [System.IO.Ports.SerialPort]::GetPortNames()) {
            $result += [pscustomobject]@{ Port = $p; Desc = "(无描述)" }
        }
    }
    return $result
}

$ports = @(Get-SerialPorts)

if ($List) {
    if ($ports.Count -eq 0) {
        Write-Host "没有找到任何串口。检查 USB 线（要能传数据，不是纯充电线）和 CH343 驱动。" -ForegroundColor Yellow
    } else {
        Write-Host "本机串口：" -ForegroundColor Cyan
        foreach ($p in $ports) { Write-Host ("  {0,-6} {1}" -f $p.Port, $p.Desc) }
    }
    exit 0
}

# ---- 选串口 ----
if ([string]::IsNullOrWhiteSpace($Port)) {
    $cands = @($ports | Where-Object { $desc = $_.Desc; ($knownChips | Where-Object { $desc -match $_ }).Count -gt 0 })
    if ($cands.Count -eq 1) {
        $Port = $cands[0].Port
        Write-Host "自动选中：$Port  ($($cands[0].Desc))" -ForegroundColor DarkGray
    } elseif ($cands.Count -gt 1) {
        Write-Host "找到多个疑似开发板的串口，请用 -Port 指定：" -ForegroundColor Yellow
        foreach ($p in $cands) { Write-Host ("  {0,-6} {1}" -f $p.Port, $p.Desc) }
        exit 1
    } else {
        Write-Host "没能自动识别开发板。现有串口：" -ForegroundColor Yellow
        foreach ($p in $ports) { Write-Host ("  {0,-6} {1}" -f $p.Port, $p.Desc) }
        Write-Host "请用 -Port COMx 指定。" -ForegroundColor Yellow
        exit 1
    }
}

# ---- 输出文件 ----
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Out)) {
    $dir = Join-Path $root "logs"
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $Out = Join-Path $dir ("serial-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
} else {
    $dir = Split-Path -Parent $Out
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
}

# ---- 打开串口 ----
$sp = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$sp.Encoding = [System.Text.Encoding]::UTF8
$sp.ReadTimeout = 200
$sp.WriteTimeout = 500
# 打开前先拉低，避免 DTR/RTS 抖动把板子按进下载模式
$sp.DtrEnable = $false
$sp.RtsEnable = $false

try {
    $sp.Open()
} catch [System.UnauthorizedAccessException] {
    Write-Host "打不开 $Port：被别的程序占用了。" -ForegroundColor Red
    Write-Host "常见占用者：另一个 monitor 窗口、Arduino IDE 串口监视器、串口助手、上次没退干净的 idf.py。" -ForegroundColor Yellow
    Write-Host "查占用：Get-CimInstance Win32_SerialPort | ? DeviceID -eq '$Port'" -ForegroundColor DarkGray
    exit 1
} catch {
    Write-Host "打不开 $Port：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$writer = New-Object System.IO.StreamWriter($Out, $false, (New-Object System.Text.UTF8Encoding($false)))
$writer.AutoFlush = $true

Write-Host ""
Write-Host "开始抓日志：$Port @ $Baud" -ForegroundColor Cyan
Write-Host "  文件：$Out" -ForegroundColor DarkGray
Write-Host "  Ctrl+C 停止$(if ($Seconds -gt 0) { "，或 $Seconds 秒后自动停" })" -ForegroundColor DarkGray
Write-Host ""

# 复位：RTS 拉 EN，DTR 保持释放 IO0（esptool 的标准时序）
if ($Reset) {
    $sp.DtrEnable = $false
    $sp.RtsEnable = $true
    Start-Sleep -Milliseconds 120
    $sp.RtsEnable = $false
    Write-Host "（已发送复位）" -ForegroundColor DarkGray
}

$start = Get-Date
$bytesTotal = 0
$lines = 0
$partial = ""

try {
    while ($true) {
        if ($Seconds -gt 0 -and ((Get-Date) - $start).TotalSeconds -ge $Seconds) { break }

        $n = 0
        try { $n = $sp.BytesToRead } catch { break }
        if ($n -le 0) {
            Start-Sleep -Milliseconds 30
            continue
        }

        $buf = New-Object byte[] $n
        $read = $sp.Read($buf, 0, $n)
        if ($read -le 0) { Start-Sleep -Milliseconds 30; continue }
        $bytesTotal += $read

        $text = $sp.Encoding.GetString($buf, 0, $read)
        $writer.Write($text)

        if ($Timestamp) {
            # 按行拼装，避免半行被截断
            $partial += $text
            while ($partial.Contains("`n")) {
                $idx = $partial.IndexOf("`n")
                $line = $partial.Substring(0, $idx).TrimEnd("`r")
                $partial = $partial.Substring($idx + 1)
                $lines++
                Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $line)
            }
        } else {
            $lines += ([regex]::Matches($text, "`n")).Count
            Write-Host -NoNewline $text
        }
    }
} finally {
    if ($Timestamp -and $partial.Length -gt 0) { Write-Host ("[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $partial) }
    $writer.Flush()
    $writer.Close()
    if ($sp.IsOpen) { $sp.Close() }
    $sp.Dispose()
}

Write-Host ""
Write-Host ""
$size = (Get-Item $Out).Length
Write-Host ("结束：共 {0} 字节，{1} 行 → {2}" -f $bytesTotal, $lines, $Out) -ForegroundColor Green
if ($bytesTotal -eq 0) {
    Write-Host "一个字节都没收到。可能原因：" -ForegroundColor Yellow
    Write-Host "  1. 选错串口 —— 用 -List 看看哪个才是板子"
    Write-Host "  2. 波特率不对 —— 固件默认 115200，改过的话加 -Baud"
    Write-Host "  3. 板子没在输出 —— 说句话触发一次交互，日志才会出现"
    Write-Host "  4. 这根 USB 线只供电不传数据"
}
