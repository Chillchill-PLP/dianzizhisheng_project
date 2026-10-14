# 源码备份脚本：将脚本所在工程复制到 DestinationRoot/工程名/时间戳。
# 默认目标 D:\备份；只在明确运行此脚本时创建备份，不参与固件构建。
# 不移动/删除源工程；目录重名时追加编号，避免覆盖已有同秒备份。
# itemsToBackUp 是显式清单，只复制其中存在的条目；新增重要文件需维护清单。
# build 和 managed_components 可重建，不复制；包含 main 的校标/字体等源码素材。
# 使用 LiteralPath 避免中文路径或方括号被 PowerShell 当成通配符。
param([string]$DestinationRoot = 'D:\备份')
$ErrorActionPreference = 'Stop'

# 以脚本所在目录为备份源，先解析为绝对路径，不受调用者当前目录影响。
$projectDirectory = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$projectName = Split-Path -Leaf $projectDirectory
$projectBackupRoot = Join-Path $DestinationRoot $projectName
New-Item -ItemType Directory -Path $projectBackupRoot -Force | Out-Null

# 时间戳按秒命名；同一秒重复运行时下面的循环寻找尚不存在的目录。
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
    'README.md', 'CHANGELOG.md', 'tools', 'assets', 'send_mp3.py',
    'build.ps1', 'verify-speech-build.ps1', 'backup-project.ps1'
)
# 列表中的目录递归复制，缺失条目跳过；不会包含清单外的文件。
foreach ($relativeItem in $itemsToBackUp) {
    $source = Join-Path $projectDirectory $relativeItem
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $destination -Recurse
    }
}
Write-Output "Project backup created: $destination"
