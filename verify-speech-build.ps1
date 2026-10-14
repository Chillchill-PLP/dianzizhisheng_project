# 编译产物的离线检查脚本，请在成功构建后用 PowerShell 7 执行。
# ProjectDirectory 默认脚本目录；只读取配置和固件，不连接开发板。
# 检查麦克风启用、ELF/BIN/模型存在、关键语音函数符号及 3 MiB 应用容量。
# 这是构建完整性检查，不能代替真实麦克风唤醒、播放或模式切换测试。
# 输出 PASS 仅表示以下静态条件通过，不表示旧产物一定与当前源码一致，故应先构建。
param([string]$ProjectDirectory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$ProjectDirectory = (Resolve-Path -LiteralPath $ProjectDirectory).Path
$buildDirectory = Join-Path $ProjectDirectory 'build'
# 检查实际 sdkconfig，不只检查默认配置，保证麦克风分支参与构建。
$configLines = Get-Content -LiteralPath (Join-Path $ProjectDirectory 'sdkconfig')
if (-not ($configLines -contains 'CONFIG_PLAYER_MIC_CONNECTED=y')) {
    throw 'Microphone support is not enabled in sdkconfig.'
}
$elfPath = Join-Path $buildDirectory 'dianzizhisheng_project.elf'
$binPath = Join-Path $buildDirectory 'dianzizhisheng_project.bin'
$modelPath = Join-Path $buildDirectory 'srmodels\srmodels.bin'
# 应用与模型分开生成，三个产物都必须存在才能进行完整语音烧录。
foreach ($requiredFile in @($elfPath, $binPath, $modelPath)) {
    if (-not (Test-Path -LiteralPath $requiredFile)) { throw "Missing build output: $requiredFile" }
}
# 用 ESP32-S3 交叉工具链 nm 检查 ELF 符号，不执行固件。
$symbols = & 'D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-nm.exe' $elfPath
if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect firmware symbols.' }
# 确认采集/检测、命令表和模型加载路径已链接入应用。
foreach ($function in @('speech_feed_task', 'speech_detect_task', 'esp_mn_commands_alloc', 'esp_srmodel_init')) {
    if (-not ($symbols -match ("\s" + $function + '$'))) { throw "Missing speech symbol: $function" }
}
# 与 partitions.csv 的 factory 大小 0x300000 对应，超出即无法放入应用分区。
if ((Get-Item -LiteralPath $binPath).Length -gt 0x300000) {
    throw 'Firmware exceeds the 3 MB factory partition.'
}
Write-Output ("PASS: ESP-SR enabled; SCK=GPIO5, WS=GPIO4, SD=GPIO6; firmware={0} bytes; models={1} bytes." -f (Get-Item -LiteralPath $binPath).Length, (Get-Item -LiteralPath $modelPath).Length)
