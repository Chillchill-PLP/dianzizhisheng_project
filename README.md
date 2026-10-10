# ESP32-S3 智能语音识别播放器与电子琴

基于 ESP32-S3 和 ESP-IDF 的离线智能语音识别播放器。设备启动时显示南京邮电大学校标，随后从 microSD 卡读取 WAV 音乐；支持离线语音控制、歌曲切换、音量控制和 4×4 矩阵键盘电子琴。

## 功能

- 南邮校标启动页与加载状态显示。
- 自动扫描 microSD 卡中的 WAV 文件并播放，支持 PCM 8/16/24/32 位和 Float32，单声道或双声道，采样率 8–96 kHz。
- 离线唤醒词：`你好小智`。
- 语音控制：`下一首`、`上一首`、`暂停`、`继续`、`播放第一首`、`播放第二首`、`增大音量`、`减小音量`、`静音`、`取消静音`。
- 短按 BOOT 在音乐播放和电子琴模式之间切换；长按约 1 秒进入音量设置。
- 电子琴使用 4×4 矩阵键盘，可演奏 C4 至 D6；其数字音量经额外衰减，可通过 `PIANO_VOLUME_DIVISOR` 调整。
- SD 卡初始化或重试期间显示“加载中”，避免反复刷新启动校标。

## 硬件连接

| 模块 | ESP32-S3 引脚 |
| --- | --- |
| MAX98357A I2S 功放 | BCLK GPIO15，LRC/WS GPIO16，DIN GPIO7 |
| ST7789 240×240 显示屏 | SCL GPIO42，SDA GPIO41，RES GPIO39，DC GPIO40，BLK GPIO38 |
| microSD 卡（SPI） | CS GPIO10，MOSI GPIO11，SCK GPIO12，MISO GPIO13 |
| INMP441 麦克风 | SCK GPIO5，WS GPIO4，SD GPIO6 |
| 矩阵键盘列 C1–C4 | GPIO9、GPIO8、GPIO2、GPIO1 |
| 矩阵键盘行 R1–R4 | GPIO14、GPIO17、GPIO21、GPIO18 |

USB 使用 GPIO19/20，BOOT 使用 GPIO0，应避免占用。

## 操作说明

音乐模式下，短按 BOOT 进入电子琴模式；再次短按返回音乐模式。按住 BOOT 约 1 秒进入设置界面：R1C1 选择音乐音量，R1C2 选择电子琴音量，R1C3 减小，R1C4 增大。

电子琴模式下语音识别会暂停，避免按键演奏声音被误识别为语音命令。

将 WAV 文件放入 FAT/FAT32 格式的 microSD 卡根目录。播放器会自动发现可播放的文件。

## 构建与烧录

需要 ESP-IDF 5.5（项目依赖 `espressif/esp-sr` 2.5.5）。在已配置 ESP-IDF 环境的 Windows PowerShell 中执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

仓库也提供 `build.ps1`，用于在本机已安装 ESP-IDF 的环境中配置并构建项目。

## 供电提示

MAX98357A 和扬声器在较高音量下会产生瞬时电流。请使用稳定的电源，并确保所有模块共地；扬声器负端应接功放的 `SPK-`，不要直接接地。
