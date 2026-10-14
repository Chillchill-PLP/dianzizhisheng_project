# 应用组件构建清单；本工程运行入口和应用功能都在 main.c。
# SRCS 是实际参与编译的源文件，INCLUDE_DIRS 允许包含本目录头文件。
# PRIV_REQUIRES 仅为本组件引入所需头文件/链接依赖，不向其他组件公开依赖。
# esp_driver_gpio：BOOT、矩阵行列、TFT 控制及 SD 引脚配置。
# esp_driver_i2s：I2S0 功放输出、I2S1 INMP441 输入。
# esp_driver_rmt：WS2812 的精确定时脉冲及 GRB 字节编码。
# esp_driver_spi：TFT SPI2 与 SD SPI3；esp_timer 提供微秒消抖/识别超时计时。
# espressif__esp-sr：AFE、WakeNet、MultiNet 和离线命令表。
# fatfs：SD FAT 文件系统及 VFS 挂载；esp_driver_sdspi：SPI 模式 SD 主机。
# sdmmc：卡初始化、状态检查以及卡信息对象，不代表接线使用 SDMMC 并行模式。
# main/njupt_splash_frames.inc 由 main.c include，提供实际参与固件的压缩动画帧。
# 静态备用校标直接内嵌于 main.c；oled_pixel_art.inc 为未参与当前构建的历史素材。
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_rmt
        esp_driver_spi
        esp_timer
        espressif__esp-sr
        fatfs
        esp_driver_sdspi
        sdmmc
)
