#Requires -Version 7.0
<#
当前音乐工程的开发入口。始终显式传入 Project，不使用共享脚本的自检工程默认值。
Flash 会先构建并检查语音模型；只有显式选择 Flash 才访问烧录流程。
Monitor/Capture 均要求填写实际串口；Ports 只枚举设备，不打开串口。
#>
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Configure', 'Clean', 'Flash', 'Monitor', 'Ports', 'Capture', 'Verify', 'DebugServer')]
    [string]$Action = 'Build',
    [string]$ProjectDirectory = (Split-Path -Parent $PSScriptRoot),
    [string]$SerialPort,
    [ValidateRange(1, 3600)]
    [int]$Seconds = 10,
    [switch]$SkipConfigure
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$projectPath = (Resolve-Path -LiteralPath $ProjectDirectory).Path
$embeddedRoot = 'D:\Embedded'
$sharedRunner = Join-Path $embeddedRoot 'scripts\Invoke-ESP32.ps1'
$activator = Join-Path $embeddedRoot 'scripts\Activate-Embedded.ps1'
$serialTool = Join-Path $embeddedRoot 'scripts\serial_tool.py'
$python = 'D:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe'
$verification = Join-Path $projectPath 'verify-speech-build.ps1'
foreach ($required in @((Join-Path $projectPath 'CMakeLists.txt'), $sharedRunner, $activator, $serialTool, $python, $verification)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required project/environment file missing: $required"
    }
}
if ($Action -in @('Flash', 'Monitor', 'Capture')) {
    if ([string]::IsNullOrWhiteSpace($SerialPort) -or $SerialPort -notmatch '^COM[1-9][0-9]*$') {
        throw 'Provide the actual board port with -SerialPort COMx. Run -Action Ports first; do not select a Bluetooth port.'
    }
    $SerialPort = $SerialPort.ToUpperInvariant()
}
if ($SkipConfigure -and $Action -ne 'Build') {
    throw '-SkipConfigure is supported only with -Action Build.'
}

$env:PYTHONUTF8 = '1'
$logDirectory = Join-Path $embeddedRoot 'logs\dianzizhisheng_project'
$null = New-Item -ItemType Directory -Path $logDirectory -Force
$stamp = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('yyyyMMdd-HHmmss-fff')
$logPath = Join-Path $logDirectory ("{0}-{1}-{2}.log" -f $Action.ToLowerInvariant(), $stamp, $PID)
$null = Start-Transcript -LiteralPath $logPath
try {
    Write-Host "Project: $projectPath"
    Write-Host "Action: $Action"
    switch ($Action) {
        'Build' {
            if ($SkipConfigure) {
                . $activator -Platform ESP32
                $buildPath = Join-Path $projectPath 'build'
                if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'build.ninja'))) {
                    throw 'No configured build directory. Run Build without -SkipConfigure first.'
                }
                & 'D:\Espressif\tools\ninja\1.12.1\ninja.exe' -C $buildPath -j 4 | Out-Host
                if ($LASTEXITCODE -ne 0) { throw "Ninja failed with exit code $LASTEXITCODE" }
            } else {
                & $sharedRunner -Action Build -Project $projectPath | Out-Host
            }
            & $verification -ProjectDirectory $projectPath | Out-Host
        }
        'Flash' {
            & $sharedRunner -Action Build -Project $projectPath | Out-Host
            & $verification -ProjectDirectory $projectPath | Out-Host
            # 使用 IDF 生成的完整烧录清单，包含 bootloader、分区表、应用和 srmodels。
            & $sharedRunner -Action Flash -Project $projectPath -SerialPort $SerialPort | Out-Host
        }
        'Monitor' {
            & $sharedRunner -Action Monitor -Project $projectPath -SerialPort $SerialPort | Out-Host
        }
        'Capture' {
            $capturePath = Join-Path $logDirectory ("serial-{0}-{1}.log" -f $stamp, $PID)
            & $python $serialTool capture --port $SerialPort --baud 115200 --seconds $Seconds --output $capturePath | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "Serial capture failed with exit code $LASTEXITCODE" }
            Write-Host "Serial data: $capturePath"
        }
        'Ports' {
            & $python $serialTool list | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "Serial enumeration failed with exit code $LASTEXITCODE" }
        }
        'Verify' {
            & $verification -ProjectDirectory $projectPath | Out-Host
        }
        default {
            & $sharedRunner -Action $Action -Project $projectPath | Out-Host
        }
    }
} catch {
    Write-Host "FAILED: $($_.Exception.Message)"
    throw
} finally {
    $null = Stop-Transcript
    Write-Host "Log: $logPath"
}
