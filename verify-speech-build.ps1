param([string]$ProjectDirectory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$ProjectDirectory = (Resolve-Path -LiteralPath $ProjectDirectory).Path
$buildDirectory = Join-Path $ProjectDirectory 'build'
$configLines = Get-Content -LiteralPath (Join-Path $ProjectDirectory 'sdkconfig')
if (-not ($configLines -contains 'CONFIG_PLAYER_MIC_CONNECTED=y')) {
    throw 'Microphone support is not enabled in sdkconfig.'
}
$elfPath = Join-Path $buildDirectory 'dianzizhisheng_project.elf'
$binPath = Join-Path $buildDirectory 'dianzizhisheng_project.bin'
$modelPath = Join-Path $buildDirectory 'srmodels\srmodels.bin'
foreach ($requiredFile in @($elfPath, $binPath, $modelPath)) {
    if (-not (Test-Path -LiteralPath $requiredFile)) { throw "Missing build output: $requiredFile" }
}
$symbols = & 'D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe' $elfPath
if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect firmware symbols.' }
foreach ($function in @('speech_feed_task', 'speech_detect_task', 'esp_mn_commands_alloc', 'esp_srmodel_init')) {
    if (-not ($symbols -match ("\s" + $function + '$'))) { throw "Missing speech symbol: $function" }
}
if ((Get-Item -LiteralPath $binPath).Length -gt 0x300000) {
    throw 'Firmware exceeds the 3 MB factory partition.'
}
Write-Output ("PASS: ESP-SR enabled; SCK=GPIO5, WS=GPIO4, SD=GPIO6; firmware={0} bytes; models={1} bytes." -f (Get-Item -LiteralPath $binPath).Length, (Get-Item -LiteralPath $modelPath).Length)
