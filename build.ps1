param([string]$ProjectDirectory = $PSScriptRoot, [switch]$SkipConfigure)
$ErrorActionPreference = 'Stop'
$env:IDF_PATH = 'D:\esp\v5.5.5\esp-idf'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\tools\python\v5.5.5\venv'
$env:IDF_PYTHON_CHECK_CONSTRAINTS = 'no'
$env:PYTHONUTF8 = '1'
$env:ESP_ROM_ELF_DIR = 'D:\Espressif\tools\esp-rom-elfs\20241011'
$env:PATH = 'D:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;D:\Espressif\tools\python\v5.5.5\venv\Scripts;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;' + $env:PATH
$buildDirectory = Join-Path $ProjectDirectory 'build'
if (-not $SkipConfigure) {
    & 'D:\Espressif\tools\cmake\3.30.2\bin\cmake.exe' -S $ProjectDirectory -B $buildDirectory -G Ninja '-DIDF_TARGET=esp32s3' '-DPYTHON=D:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& 'D:\Espressif\tools\ninja\1.12.1\ninja.exe' -C $buildDirectory -j 4
exit $LASTEXITCODE
