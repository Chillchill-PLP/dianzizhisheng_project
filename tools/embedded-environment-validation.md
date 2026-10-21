# D 盘嵌入式环境适配记录（2026-10-21）

当前工程：`C:\Users\Administrator\Desktop\dianzizhisheng_project`。本次适配开发入口、VS Code 配置和文档；工程源码继续使用原位置，工具复用 `D:\Embedded` 与既有 ESP-IDF 安装。

## 适配内容

- 根目录 `打开电子之声VSCode.cmd` / `tools/Open-Embedded.ps1` 打开本工程专用工作区，使用 D 盘独立用户数据与插件目录。
- `tools/Invoke-Project.ps1` 每次显式向共享脚本传入当前 Project 路径，不使用其自检工程默认值。
- Build 与 Flash 在编译后执行已有的 `verify-speech-build.ps1`。Flash 通过 IDF 完整清单烧录，包括语音模型；端口必须由使用者填写。
- 原 `build.ps1` 兼容入口和 `-SkipConfigure` 保留，后者仍直接运行本工程 Ninja 增量构建。
- 增加编译、配置、清理、烧录、监视、语音检查、串口枚举、10 秒采集和 OpenOCD 任务。
- 使用 ESP clangd、实际编译数据库、Xtensa GDB 和 ESP OpenOCD；调试类型按已安装 ESP-IDF 插件 2.3.0 的 `gdbtarget` 配置。调试连接不自动烧录，须连接原生 USB JTAG。
- 进程临时目录、构建缓存和操作日志使用 D 盘；日志位于 `D:\Embedded\logs\dianzizhisheng_project`。

## 验证结果

| 验证项 | 结果 |
| --- | --- |
| PowerShell 7 及共享环境 | 通过，入口 PowerShell 为 7.6.7，`idf.py --version` 为 ESP-IDF v5.5.5 |
| ESP 工具选择 | 命令解析到 `D:\Espressif\tools` 下的 Xtensa GCC/GDB、ESP OpenOCD 和 ESP clangd，而非 STM32 工具 |
| 配置文件与脚本 | JSON 可解析、PowerShell AST 无语法错误；调试类型和属性存在于已安装插件定义中 |
| 项目路由 | 所有任务明确传递当前 `${workspaceFolder}`，实际构建日志确认目标为音乐工程 |
| 完整构建 | 通过，共执行 972 个 Ninja 步骤，生成当前完整播放器镜像 |
| 语音产物检查 | 通过：麦克风启用，采集/识别/命令和模型关键符号存在，应用及模型镜像齐全 |
| 完整烧录清单 | 包含 bootloader、分区表、应用，以及 `0x310000` 处的 `srmodels/srmodels.bin` |
| 显式串口要求 | 未提供串口时，Flash 在调用烧录工具之前拒绝执行 |
| 串口枚举与日志 | 通过，仅枚举设备、不打开端口；结果与原始工具输出写入 D 盘日志。当前只列出蓝牙 COM3、COM4 |
| clangd 编辑器诊断 | 使用 LSP 初始化并打开真实 main.c，收到 0 个错误；6 条“头文件未直接引用”提示属于现有代码提示，未为此改动业务代码 |
| 应用及配置源码 | 主目录代码、GPIO 定义、sdkconfig.defaults、分区表及依赖锁定文件哈希保持不变 |

本次完整构建的应用镜像为 **3,120,688 字节**，3 MiB 应用分区剩余 **25,040 字节（约 24.5 KiB）**；模型镜像为 **2,494,988 字节**。当前可放入现有分区，构建工具会提示应用分区接近满载；后续增加功能时需重新评估容量。

没有执行 Flash、Monitor、串口采集、Clean、JTAG 实物连接或 VS Code GUI 启动验证；没有改变原生 USB 保留、UART0 控制台或现有外设接线。构建通过不等于实物验收通过。
