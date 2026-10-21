"""Generate compact antialiased bitmap fonts from the fonts on this computer.

Small font: ASCII and GB2312 Chinese for UI labels and SD filenames.
Large font: ASCII and UI/current-song characters. Each pixel uses two bits.
Run with Python 3 and Pillow on Windows. Writes main/player_ui_fonts.inc.
The bitmap is generated at development time; no font files are read by ESP32.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import json
import time

ROOT = Path(__file__).resolve().parent.parent / "main"
LABELS = "开始界面选择进入返回文件传输热点密码连接浏览器打开路由上传下载完成失败正在启动请重试可用 电子之声音乐播放器电子琴设置音量播放中已暂停正在聆听请说指令未找到歌曲将文件放入存储卡后重启加载中正在读取存储卡重试准备就绪按住琴键演奏请检查键盘仅支持单键长按设置短按切换退出选择减少增加音乐音量琴键音量唤醒你好小鑫松开后演奏扬声器静音语音关闭重新连接播放第一首第二首上一首下一首继续取消增大减小声道曲目没有文件宇多田光南邮校标当前"
small_chars = set(chr(i) for i in range(32, 127)) | set(LABELS) | {'…', '□'}
for first in range(0xA1, 0xF8):
    for second in range(0xA1, 0xFF):
        try:
            char = bytes([first, second]).decode('gb2312')
        except UnicodeDecodeError:
            continue
        if len(char) == 1:
            small_chars.add(char)
large_chars = set(chr(i) for i in range(32, 127)) | set(LABELS) | {'…', '□'}

lines = ["/* 自动生成的 2-bit 灰度点阵：每字节从高到低存 4 个像素，0 透明、3 不透明。",
         " * 小字库包含 ASCII、GB2312 字符及界面用字；大字库包含 ASCII、界面和当前歌曲常用字。",
         " * 仅在 Flash 保存常量，不在 MCU 启动时加载字体文件；字形按 Unicode 编码升序排列。",
         " * 修改界面文字后，可用 generate_fonts.py 重新生成；缺字回退为方框而不破坏 UTF-8。",
         " */", "#ifndef PLAYER_UI_FONTS_INC", "#define PLAYER_UI_FONTS_INC", "",
         "typedef struct {", "    uint32_t offset; /* 对应像素数据在本字体 data 数组中的字节偏移 */",
         "    uint16_t codepoint; /* BMP Unicode 码点，二分查找键 */",
         "    uint8_t width; /* 字形画布宽，单位像素 */",
         "    uint8_t advance; /* 光标前进像素数，含字间间距 */", "} ui_glyph_t;", "",
         "typedef struct {", "    const ui_glyph_t *glyphs;", "    const uint8_t *data;",
         "    size_t count;", "    uint8_t height;", "} ui_font_t;", ""]
stats = {}
for size, chars in [(14, small_chars), (18, large_chars), (32, set('ABCDEFG0123456789-'))]:
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', size)
    height = size + 3
    glyphs, data = [], bytearray()
    for char in sorted(chars, key=ord):
        advance = max(3, round(font.getlength(char)) + (0 if ord(char) > 127 else 1))
        width = max(advance, font.getbbox(char)[2])
        canvas = Image.new('L', (width, height), 0)
        ImageDraw.Draw(canvas).text((0, -2), char, font=font, fill=255)
        pixels = [(pixel * 3 + 127) // 255 for pixel in list(canvas.get_flattened_data() if hasattr(canvas, "get_flattened_data") else canvas.getdata())]
        glyphs.append((len(data), ord(char), width, advance))
        for offset in range(0, len(pixels), 4):
            group = pixels[offset:offset+4] + [0] * max(0, 4-len(pixels[offset:offset+4]))
            data.append((group[0] << 6) | (group[1] << 4) | (group[2] << 2) | group[3])
    lines.append(f'static const ui_glyph_t UI_GLYPHS_{size}[] = {{')
    lines.extend(f'    {{{offset}U, 0x{codepoint:04X}U, {width}U, {advance}U}},' for offset, codepoint, width, advance in glyphs)
    lines += ['};', f'static const uint8_t UI_GLYPH_DATA_{size}[] = {{']
    lines.extend('    ' + ','.join(f'0x{x:02X}' for x in data[start:start+24]) + ',' for start in range(0,len(data),24))
    lines += ['};', f'static const ui_font_t UI_FONT_{size} = {{',
              f'    UI_GLYPHS_{size}, UI_GLYPH_DATA_{size}, ARRAY_SIZE(UI_GLYPHS_{size}), {height}U,', '};', '']
    stats[str(size)] = {'glyphs': len(glyphs), 'pixels_bytes': len(data), 'height': height}
lines += ['#endif', '']
(ROOT / 'player_ui_fonts.inc').write_text('\n'.join(lines), encoding='utf-8')
print(stats, flush=True)
