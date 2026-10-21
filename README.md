# ESP32-S3 智能语音识别播放器与电子琴

基于 ESP32-S3 N16R8、ESP-IDF 5.5.5 和 ESP-SR 2.5.5 的离线语音播放器。启动时播放南京邮电大学校徽描边、渐显动画，再进入加载页和歌曲界面；支持 microSD WAV 播放、语音控制、4×4 矩阵键盘电子琴和 WS2812 RGB 灯效。

## 当前进度

截至 2026-10-14，最新固件已在 ESP32-S3 开发板上编译、烧录并检查启动日志。详细记录见 [更新日志](CHANGELOG.md)。

- **开屏动画**：37 帧，最多 10 帧/秒，单次约 4.3 秒；校徽相对最初动画宽高放大约 2 倍，屏幕上约 166×173 像素，居中且保留原白色区域的镂空。播放结束进入“加载中”，SD 重试不重复动画。
- **显示优化**：ST7789 使用 20 MHz SPI、16 行 DMA 缓冲及变化区域刷新；跳过重复状态、合并最新消息。运行界面的局部更新上限为 50 次/秒，这不是整屏帧率或面板扫描帧率。动画写屏过慢时跳过过期帧，最终校徽始终显示。
- **RGB 灯效**：音乐播放时按固定周期彩色呼吸；电子琴按住单键时显示对应音名的颜色，松开熄灭。当前灯效根据播放状态运行，不进行音频节拍分析。
- **模式切换恢复**：电子琴切回播放器时检查 SD 卡；读卡超时后自动重新挂载并降至 4/2 MHz 重试，期间显示“加载中”。
- **代码说明**：为任务分工、音频通路、按键消抖、离线语音、SD 恢复及显示传输增加了详细中文注释。



## 播放和语音

将 WAV 文件放入 FAT/FAT32 格式的 microSD 卡。固件启动时自动扫描并播放，支持 PCM 8/16/24/32 位和 Float32、单/双声道、8–96 kHz。文件使用 UTF-8 长文件名打开，屏幕标题转换为 ASCII；无内置歌曲，也未集成 MP3 解码。

音乐模式下先说 **“你好小智”** 唤醒，再在 6 秒内说以下任一句。每条命令前需要重新唤醒。

| 功能 | 提示词 |
| --- | --- |
| 切歌 | 下一首、上一首 |
| 播放控制 | 暂停、继续 |
| 选曲 | 播放第一首、播放第二首 |
| 音量 | 增大音量、减小音量、静音、取消静音 |

“第一首/第二首”对应启动扫描时建立的播放列表顺序。电子琴模式暂停麦克风采集和语音识别，切回播放器后恢复。

## 按键与灯效

短按并松开 BOOT 切换播放器/电子琴，返回播放器后从当前歌曲开头播放。长按 BOOT 约 1 秒进入设置，短按退出；R1C1 选择音乐音量，R1C2 选择电子琴音量，R1C3 减小，R1C4 增大。音量范围 0–100%，步进 1%，长按加减可连续调整；重启恢复代码初始值。

| 矩阵行 | C1 | C2 | C3 | C4 |
| --- | --- | --- | --- | --- |
| R1 | C4 | D4 | E4 | F4 |
| R2 | G4 | A4 | B4 | C5 |
| R3 | D5 | E5 | F5 | G5 |
| R4 | A5 | B5 | C6 | D6 |

电子琴为单音：按住发声，松开停止；多键冲突时静音。C=红、D=橙、E=黄、F=绿、G=青、A=蓝、B=紫，同名音不同八度同色。暂停、语音监听、无歌曲、多键冲突及电子琴设置中熄灭 RGB。

当前 `main/main.c` 的音量参数如下，修改后需重新编译烧录：

| 参数 | 当前值 | 用途 |
| --- | --- | --- |
| `PLAYER_VOLUME_PERCENT` | `1U` | 音乐上电初始音量 1% |
| `PIANO_VOLUME_PERCENT` | `1U` | 电子琴上电初始音量 1% |
| `AUDIO_OUTPUT_GAIN` | `1U` | 两种模式共用输出增益，超出 int16 时限幅 |
| `PIANO_VOLUME_DIVISOR` | `300U` | 电子琴波形的额外数字衰减 |

## 硬件连接

主控为 ESP32-S3 N16R8，16 MB Flash、8 MB 八线 PSRAM；显示驱动使用 ST7789，分辨率 240×240。矩阵键盘、SD 模块和扬声器的具体商品型号需以实物标识为准。

| 模块 | ESP32-S3 引脚 |
| --- | --- |
| MAX98357A I2S 功放 | BCLK GPIO15，LRC/WS GPIO16，DIN GPIO7 |
| ST7789 240×240 显示屏 | SCL GPIO42，SDA GPIO41，RES GPIO39，DC GPIO40，BLK GPIO38 |
| microSD 卡（SPI） | CS GPIO10，MOSI GPIO11，SCK GPIO12，MISO GPIO13 |
| INMP441 麦克风 | SCK GPIO5，WS GPIO4，SD GPIO6 |
| WS2812 RGB | DIN GPIO48 |
| 矩阵键盘列 C1–C4 | GPIO9、GPIO8、GPIO2、GPIO1 |
| 矩阵键盘行 R1–R4 | GPIO14、GPIO17、GPIO21、GPIO18 |

GPIO19/20 留给 USB，GPIO0 为 BOOT，GPIO35/36/37 由八线 PSRAM 占用。MAX98357A 使用稳定的 5V 电源，所有模块共地；扬声器负端接功放 `SPK-`。

## 构建与烧录

需要 ESP-IDF 5.5.x，当前验证版本为 **5.5.5**；组件管理器会下载锁定的 `espressif/esp-sr` **2.5.5**。在已配置 ESP-IDF 环境的 **PowerShell 7（pwsh）** 中执行：

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

首次烧录需包含分区表和语音模型。项目使用自定义分区：应用 3 MiB，模型 8 MiB。`sdkconfig.defaults` 提供首次配置；已有 `sdkconfig` 时以其为准。

仓库也提供本机辅助脚本，工具路径与开发电脑绑定，其他电脑需调整：

```powershell
./build.ps1
./verify-speech-build.ps1
./backup-project.ps1
```

`build.ps1` 只构建，`verify-speech-build.ps1` 检查麦克风配置、固件容量和模型/关键符号，`backup-project.ps1` 备份源码与配置。

## 动画资源

`main/njupt_splash_frames.inc` 是实际参与固件编译的 32 色 RGB565 游程压缩帧，运行时直接解码到 PSRAM 帧缓冲。备用静态校徽保留在 `main.c` 中。

`assets/njupt-splash-source.gif` 保存用户提供的透明版 SVG 在黑底上的预览源；ST7789 没有 alpha 通道，透明区域与黑底合成。安装 Pillow 后可重新生成固件资源，默认居中放大 2 倍：

```powershell
python -m pip install Pillow
python tools/generate_njupt_splash.py --source assets/njupt-splash-source.gif --output main/njupt_splash_frames.inc --zoom 2
```

动画时间轴、压缩格式、错误恢复和帧率控制的详细说明见 `main.c` 中的 `tft_decode_splash_frame`、`tft_splash_wait_until` 和 `tft_play_njupt_splash`。

## 验证记录

2026-10-13 在 ESP32-S3 开发板上烧录了当前放大动画版本。串口日志记录：

```text
NJUPT splash complete: rendered=37 skipped=0, elapsed=4373 ms, max_frame=64 ms, pixels=869891
```

37 帧全部提交成功，单帧最长 64 ms，低于 100 ms 帧间隔。随后 SD 扫描成功、ESP-SR 初始化完成，并开始播放 44.1 kHz 双声道 16 位 WAV。以上是固件解码/提交耗时，不是面板扫描帧率的测量。

此前主机模拟验证覆盖了逐帧解码、变化区域传输、慢速跳帧、SPI 失败后的整屏恢复及加载页切换；硬件上还验证过电子琴返回播放器的 SD 恢复流程。长期运行、不同 SD 卡和实际语音识别率仍需继续测试。

`send_mp3.py` 为旧串口接收协议的历史工具，当前固件不实现该协议。
