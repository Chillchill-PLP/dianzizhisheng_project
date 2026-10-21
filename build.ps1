#Requires -Version 7.0
# 兼容原有构建入口；统一复用 D:\Embedded 环境，构建成功后检查语音模型和容量。
# -SkipConfigure 保留原先直接使用 Ninja 增量构建的行为，要求已配置 build 目录。
[CmdletBinding()]
param([string]$ProjectDirectory = $PSScriptRoot, [switch]$SkipConfigure)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'tools\Invoke-Project.ps1') -Action Build -ProjectDirectory $ProjectDirectory -SkipConfigure:$SkipConfigure
