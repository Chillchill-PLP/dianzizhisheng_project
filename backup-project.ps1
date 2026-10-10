param([string]$DestinationRoot = 'D:\备份')
$ErrorActionPreference = 'Stop'

$projectDirectory = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$projectName = Split-Path -Leaf $projectDirectory
$projectBackupRoot = Join-Path $DestinationRoot $projectName
New-Item -ItemType Directory -Path $projectBackupRoot -Force | Out-Null

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$destination = Join-Path $projectBackupRoot $timestamp
$suffix = 2
while (Test-Path -LiteralPath $destination) {
    $destination = Join-Path $projectBackupRoot ("$timestamp-$suffix")
    $suffix++
}
New-Item -ItemType Directory -Path $destination | Out-Null

# 只保存不可重新生成的源码、配置和说明；build 与 managed_components 不复制。
$itemsToBackUp = @(
    '.clangd', '.gitignore', '.devcontainer', '.vscode',
    'CMakeLists.txt', 'main', 'partitions.csv', 'sdkconfig',
    'sdkconfig.defaults', 'dependencies.lock', 'README_ESP_SR.md',
    'build.ps1', 'verify-speech-build.ps1', 'backup-project.ps1'
)
foreach ($relativeItem in $itemsToBackUp) {
    $source = Join-Path $projectDirectory $relativeItem
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $destination -Recurse
    }
}
Write-Output "Project backup created: $destination"
