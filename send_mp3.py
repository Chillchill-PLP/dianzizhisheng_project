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

BLOCK_BYTES = 4096
MAX_MP3_BYTES = 128 * 1024 * 1024


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


def send_file(port_name, path, name):
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
        crc = 0
        while block := music.read(64 * 1024):
            crc = zlib.crc32(block, crc)
        music.seek(0)
        header = b"MP3!" + struct.pack("<II13s", size, crc, name.encode("ascii"))
        port = serial.Serial(port=None, baudrate=115200, timeout=1, write_timeout=10)
        # Avoid toggling reset/boot lines when opening a USB-UART adapter.
        port.dtr = False
        port.rts = False
        port.port = port_name
        try:
            port.open()
            port.reset_input_buffer()
            print(f"等待 SD 卡就绪：{port_name}；准备保存为 /sdcard/music/{name}")
            wait_reply(port, b"READY", 30)
            port.write(header)
            port.flush()
            wait_reply(port, b"SEND", 15)
            sent = 0
            last_percent = -1
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
            wait_reply(port, b"OK", max(60, size / (64 * 1024) + 30))
            print(f"成功：music/{name}，{size} 字节，CRC32={crc:08X}")
        finally:
            port.close()


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


if __name__ == "__main__":
    sys.exit(main())
