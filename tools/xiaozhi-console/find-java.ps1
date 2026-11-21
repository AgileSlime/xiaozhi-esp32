# 查找本机可用的 Java 17+，输出 java.exe 的完整路径；找不到则不输出任何内容。
# 由 start.bat 调用。输出只有路径（ASCII），避免编码问题。
#
# 两个坑，改这个脚本时别再踩：
#   1. 不能盲目相信 JAVA_HOME —— 很多机器上它指向老版本（如 Java 8），
#      而真正的新版装在别处。所以把所有候选都探一遍版本，取最高的。
#   2. 不能用 $ErrorActionPreference = 'SilentlyContinue' 全局静默 ——
#      java -version 的输出走 stderr，全局静默会把版本信息一起吞掉，
#      导致所有 Java 都被判成"版本未知"。改成逐条 -ErrorAction。

$cands = New-Object System.Collections.Generic.List[string]

function Try-Add([string]$p) {
    if ($p -and (Test-Path -LiteralPath $p -ErrorAction SilentlyContinue)) {
        [void]$cands.Add($p)
    }
}

# 1. JAVA_HOME（只是候选之一，不优先）
if ($env:JAVA_HOME) { Try-Add (Join-Path $env:JAVA_HOME 'bin\java.exe') }

# 2. 注册表里登记的 JDK/JRE
$regRoots = @(
    'HKLM:\SOFTWARE\JavaSoft\JDK',
    'HKLM:\SOFTWARE\JavaSoft\JRE',
    'HKLM:\SOFTWARE\JavaSoft\Java Development Kit',
    'HKLM:\SOFTWARE\JavaSoft\Java Runtime Environment',
    'HKLM:\SOFTWARE\Eclipse Adoptium\JDK',
    'HKLM:\SOFTWARE\Eclipse Adoptium\JRE',
    'HKLM:\SOFTWARE\Microsoft\JDK',
    'HKLM:\SOFTWARE\Amazon Corretto\JDK',
    'HKLM:\SOFTWARE\Azul Systems\Zulu'
)
foreach ($rk in $regRoots) {
    Get-ChildItem -Path $rk -ErrorAction SilentlyContinue | ForEach-Object {
        $jh = (Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue).JavaHome
        if ($jh) { Try-Add (Join-Path $jh 'bin\java.exe') }
    }
}

# 3. 常见安装目录（浅层递归，不做全盘扫描）
$roots = @(
    'C:\Program Files\Java',
    'C:\Program Files\Eclipse Adoptium',
    'C:\Program Files\Microsoft',
    'C:\Program Files\Amazon Corretto',
    'C:\Program Files\BellSoft',
    'C:\Program Files\Zulu',
    'C:\Program Files\Semeru',
    'C:\Program Files (x86)\Java',
    (Join-Path $env:LOCALAPPDATA 'Programs\Eclipse Adoptium')
)
foreach ($r in $roots) {
    Get-ChildItem -Path $r -Filter 'java.exe' -Recurse -Depth 4 -ErrorAction SilentlyContinue |
        ForEach-Object { Try-Add $_.FullName }
}

# 4. PATH 里的 java
$onPath = (Get-Command java -ErrorAction SilentlyContinue).Source
if ($onPath) { Try-Add $onPath }

# 5. 逐个探版本，选 >=17 里版本号最高的
$best = $null
$bestMajor = 0
$bestMinor = 0
foreach ($j in ($cands | Select-Object -Unique)) {
    $lines = $null
    try {
        # 逐行转字符串：java -version 走 stderr，直接 Out-String 会是空的
        $lines = & $j -version 2>&1 | ForEach-Object { "$_" }
    } catch {
        continue
    }
    if (-not $lines) { continue }
    $txt = $lines -join "`n"
    if ($txt -match 'version "(\d+)(?:\.(\d+))?') {
        $major = [int]$Matches[1]
        $minor = 0
        if ($Matches[2]) { $minor = [int]$Matches[2] }
        # 老式版本号 1.8.0_261 -> 真实主版本是 8
        if ($major -eq 1) { $major = $minor; $minor = 0 }
        if ($major -ge 17) {
            if ($major -gt $bestMajor -or ($major -eq $bestMajor -and $minor -gt $bestMinor)) {
                $best = $j
                $bestMajor = $major
                $bestMinor = $minor
            }
        }
    }
}

if ($best) { Write-Output $best }
