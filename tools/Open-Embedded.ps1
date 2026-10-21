#Requires -Version 7.0
# 以 D 盘独立编辑器数据及插件目录打开当前工程，避免打开默认自检工程。
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$projectPath = Split-Path -Parent $PSScriptRoot
$workspace = Join-Path $projectPath 'dianzizhisheng.code-workspace'
$activator = 'D:\Embedded\scripts\Activate-Embedded.ps1'
$editor = 'D:\Users\Administrator\AppData\Local\Programs\Microsoft VS Code\Code.exe'
foreach ($required in @($workspace, $activator, $editor)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing file: $required" }
}
. $activator -Platform ESP32
& $editor --new-window --user-data-dir 'D:\Embedded\vscode-data' --extensions-dir 'D:\Embedded\vscode-extensions' $workspace
if ($LASTEXITCODE -ne 0) { throw "VS Code failed with exit code $LASTEXITCODE" }
