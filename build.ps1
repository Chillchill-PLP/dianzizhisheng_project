# 本机工程构建辅助脚本，请用 PowerShell 7（pwsh）执行。
# 参数 ProjectDirectory：待构建工程目录，默认脚本所在目录。
# 参数 SkipConfigure：已有有效 build 配置时跳过显式 CMake，直接执行 Ninja 增量构建。
# 示例：./build.ps1 -ProjectDirectory 'C:\Users\Administrator\Desktop\dianzizhisheng_project'
# 本脚本只配置和编译，不访问串口、不烧录；工具绝对路径与当前电脑安装位置绑定。
# CMake/Ninja 的退出码向外传递，便于自动化判断失败，不能只看是否生成过旧 BIN。
param([string]$ProjectDirectory = $PSScriptRoot, [switch]$SkipConfigure)
# PowerShell 命令错误立即中止；外部工具的非零退出码仍需显式检查。
$ErrorActionPreference = 'Stop'
# 为当前脚本进程和子进程配置 IDF/Python/ROM 符号环境，不修改系统永久环境。
$env:IDF_PATH = 'D:\esp\v5.5.5\esp-idf'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\tools\python\v5.5.5\venv'
$env:IDF_PYTHON_CHECK_CONSTRAINTS = 'no'
$env:PYTHONUTF8 = '1'
$env:ESP_ROM_ELF_DIR = 'D:\Espressif\tools\esp-rom-elfs\20241011'
# 将此工程所需工具放在 PATH 前面，避免误用另一套 Python/CMake/交叉编译器。
$env:PATH = 'D:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;D:\Espressif\tools\python\v5.5.5\venv\Scripts;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;' + $env:PATH
# Ninja 工程和全部生成产物放在工程 build 子目录。
$buildDirectory = Join-Path $ProjectDirectory 'build'
# 不跳过配置时，用 Ninja 生成器指定 esp32s3 目标和本机 IDF Python。
if (-not $SkipConfigure) {
    & 'D:\Espressif\tools\cmake\3.30.2\bin\cmake.exe' -S $ProjectDirectory -B $buildDirectory -G Ninja '-DIDF_TARGET=esp32s3' '-DPYTHON=D:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
# 四个并行编译作业；Ninja 只重建依赖变更的目标。
& 'D:\Espressif\tools\ninja\1.12.1\ninja.exe' -C $buildDirectory -j 4
exit $LASTEXITCODE
