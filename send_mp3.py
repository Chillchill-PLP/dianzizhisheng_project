# 历史 UART1 MP3 文件上传工具，保留于工程目录，当前 main.c 不实现其接收协议。
# 当前播放器只播放 SD WAV，因此运行这个脚本不会把 MP3 变成当前固件可播放文件。
# 文档中的 GPIO4/GPIO6 是旧接收固件接线；当前它们被 INMP441 使用，不能并接适配器。
# 仅在配套旧接收固件及独立 3.3V USB-UART 接线下使用；本次只补充代码解释。
# 协议：READY -> MP3! 头 -> SEND -> 每块数据/NEXT -> 整体 CRC 校验后 OK。
# 头为 4 字节魔数 + <II13s（小端长度、CRC32、13 字节 8.3 文件名），总 25 字节。
"""Send existing MP3 files to the ESP32-S3 SD receiver over UART1.

Run with the ESP-IDF Python environment (pyserial is already installed).
This UART is a separate 3.3 V USB-UART adapter on GPIO4/GPIO6.
"""

import argparse
from pathlib import Path
import re
import struct
import sys
import time
import zlib

import serial

# 单次发送最多 4096 字节，每块收到 NEXT 确认后再发下一块。
BLOCK_BYTES = 4096
# 旧协议接受的文件上限 128 MiB，长度与 CRC 在协议头中用 uint32 保存。
MAX_MP3_BYTES = 128 * 1024 * 1024


# 初步检查 ID3 或 MPEG 音频帧同步/保留字段，只是签名筛查，不进行 MP3 解码。
def has_mp3_signature(data):
    if len(data) >= 10 and data[:3] == b"ID3":
        return True
    return (
        len(data) >= 4
        and data[0] == 0xFF
        and data[1] & 0xE0 == 0xE0
        and data[1] & 0x18 != 0x08
        and data[1] & 0x06 == 0x02
        and data[2] & 0xF0 not in (0, 0xF0)
        and data[2] & 0x0C != 0x0C
    )


# 等待一行精确协议响应；monotonic 不受系统时间调整影响，ERR 或异常行立即报错。
def wait_reply(port, expected, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().strip()
        if line.startswith(b"ERR"):
            raise RuntimeError(f"开发板返回：{line.decode('ascii', errors='replace')}")
        if line == expected:
            return
        if line and line != b"READY":
            raise RuntimeError(f"串口响应异常：{line!r}")
    raise TimeoutError(f"等待 {expected.decode()} 超时，请检查串口、接线和供电。")


# 上传一个文件：参数校验 -> 流式 CRC -> 串口握手 -> 逐块发送确认 -> 最终校验。
def send_file(port_name, path, name):
    # 旧接收协议使用 FAT 8.3 名称，最多 8 位主名，不支持本播放器使用的 UTF-8 长路径。
    name = name.upper()
    if re.fullmatch(r"[A-Z0-9_-]{1,8}\.MP3", name) is None:
        raise ValueError("SD 文件名须为 1~8 位英文字母/数字/下划线/连字符 + .MP3。")
    with path.open("rb") as music:
        size = music.seek(0, 2)
        if not 4 <= size <= MAX_MP3_BYTES:
            raise ValueError("文件大小须在 4 字节至 128 MiB 之间。")
        music.seek(0)
        if not has_mp3_signature(music.read(BLOCK_BYTES)):
            raise ValueError("文件没有识别到 MP3/ID3 文件头，请提供有效的现有 MP3 文件。")
        music.seek(0)
        # 先遍历一遍源文件生成整体 CRC32，再 seek 回开头发送，避免整首载入内存。
        crc = 0
        while block := music.read(64 * 1024):
            crc = zlib.crc32(block, crc)
        music.seek(0)
        # < 强制小端且不加对齐填充，13s 固定长度不足处补 NUL，保存 8.3 文件名。
        header = b"MP3!" + struct.pack("<II13s", size, crc, name.encode("ascii"))
        port = serial.Serial(port=None, baudrate=115200, timeout=1, write_timeout=10)
        # Avoid toggling reset/boot lines when opening a USB-UART adapter.
        # 打开端口前禁止 DTR/RTS 自动拉动复位/启动线，减少对旧板运行状态的干扰。
        port.dtr = False
        port.rts = False
        port.port = port_name
        try:
            port.open()
            # 清除接收缓冲中的旧字节，然后只按当前一轮握手解析 READY。
            port.reset_input_buffer()
            print(f"等待 SD 卡就绪：{port_name}；准备保存为 /sdcard/music/{name}")
            wait_reply(port, b"READY", 30)
            port.write(header)
            port.flush()
            wait_reply(port, b"SEND", 15)
            sent = 0
            last_percent = -1
            # 每个块都等接收端写卡完成；源文件在发送时缩水则中止，避免静默发送残缺内容。
            while sent < size:
                block = music.read(min(BLOCK_BYTES, size - sent))
                if not block:
                    raise RuntimeError("源文件在传输期间变短，传输已中止。")
                port.write(block)
                port.flush()
                wait_reply(port, b"NEXT", 15)
                sent += len(block)
                percent = sent * 100 // size
                if percent != last_percent:
                    print(f"\r上传 {percent:3d}% ({sent}/{size} 字节)", end="", flush=True)
                    last_percent = percent
            print("\n等待 SD 卡读回校验……")
            # Read-back of a large file is slower than a single UART block.
            # 最终 OK 在接收端整文件读回校验后出现，超时随文件大小增加。
            wait_reply(port, b"OK", max(60, size / (64 * 1024) + 30))
            print(f"成功：music/{name}，{size} 字节，CRC32={crc:08X}")
        # 失败/用户中止时也关闭本机串口；接收端是否清临时文件由旧固件负责。
        finally:
            port.close()


# 命令行入口；捕获预期操作错误输出中文原因，返回适合脚本调用的退出码。
def main():
    parser = argparse.ArgumentParser(description="通过 UART1 向 ESP32-S3 的 SD 卡存入 MP3。")
    parser.add_argument("--port", required=True, help="USB 转串口的端口，例如 COM6")
    parser.add_argument("--file", required=True, type=Path, help="电脑上的 MP3 文件路径")
    parser.add_argument("--name", default="SONG001.MP3", help="SD 文件名，例如 SONG002.MP3")
    args = parser.parse_args()
    try:
        send_file(args.port, args.file, args.name)
    except (OSError, ValueError, RuntimeError, TimeoutError, serial.SerialException) as error:
        print(f"失败：{error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n已中止；开发板超时后会清理未完成的临时文件。", file=sys.stderr)
        return 130
    return 0


# 直接执行才启动上传；作为模块导入时只定义函数，不访问串口。
if __name__ == "__main__":
    sys.exit(main())
