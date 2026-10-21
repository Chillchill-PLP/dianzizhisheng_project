"""将桌面的南邮动画转换为固件可直接解码的 240×240 RGB565 帧。

运行示例（需要 Pillow）：
  python generate_njupt_splash.py --source preview.gif --output njupt_splash_frames.inc

GIF 是用户透明 SVG 在黑色背景上的预览；ST7789 没有 alpha 通道，
所以透明区域保留为黑色，最终校徽的原白色区域仍保持镂空。
共用 32 色表 + PackBits 游程压缩，避免把数 MB 的原始帧放进应用分区。
"""

import argparse
import hashlib
import json
import re
from pathlib import Path

from PIL import Image, ImageDraw

WIDTH = HEIGHT = 240
COLORS = 32
MIN_FRAME_MS = 33
FINAL_HOLD_MS = 500
LOGO_ZOOM = 2.0  # 统一裁去画布四周留白，校徽宽高扩大两倍，动画各帧使用同一窗口。


def rgb565(rgb):
    red, green, blue = rgb
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def encode_packbits(values):
    """每包 1~128 像素；bit7=1 是重复包，bit7=0 是逐像素索引包。"""
    result = bytearray()
    position = 0
    while position < len(values):
        run = 1
        while (position + run < len(values) and run < 128
               and values[position + run] == values[position]):
            run += 1
        if run >= 3:
            result.extend((0x80 | (run - 1), values[position]))
            position += run
            continue
        start = position
        # 每次只扩展一个像素，保证跨短游程边界时也不会产生 129 像素的包。
        while position < len(values) and position - start < 128:
            if (position + 2 < len(values)
                and values[position] == values[position + 1] == values[position + 2]):
                break
            position += 1
        count = position - start
        assert 1 <= count <= 128
        result.append(count - 1)
        result.extend(values[start:position])
    return bytes(result)


def format_values(values, width, formatter):
    return ",\n".join("    " + ", ".join(formatter(value) for value in values[start:start + width])
                     for start in range(0, len(values), width))


def decode_packbits(data):
    output = bytearray()
    position = 0
    while position < len(data):
        control = data[position]
        position += 1
        count = (control & 127) + 1
        if control & 128:
            output.extend([data[position]] * count)
            position += 1
        else:
            output.extend(data[position:position + count])
            position += count
    if len(output) != WIDTH * HEIGHT:
        raise ValueError("invalid reference frame")
    return bytes(output)


def convert(source_path, output_path, preview_dir=None, zoom=LOGO_ZOOM,
            frame_dir=None, reference_resource=None):
    """保留时序；可读取按原 SVG 重采样的 PNG+timings.json，不重复原 GIF 帧。"""
    if not 1.0 <= zoom <= 2.5:
        raise ValueError("zoom 必须在 1.0~2.5 之间")
    frames, durations = [], []
    fps = 10
    raw_frames = []
    if frame_dir is not None:
        manifest = json.loads((frame_dir / "timings.json").read_text(encoding="utf-8"))
        fps = manifest["fps"]
        for entry in manifest["frames"]:
            with Image.open(frame_dir / entry["file"]) as image:
                raw_frames.append((image.convert("RGBA"), entry["duration_ms"]))
    else:
        with Image.open(source_path) as source:
            for index in range(source.n_frames):
                source.seek(index)
                raw_frames.append((source.convert("RGBA"), int(source.info.get("duration", 100))))
    for index, (rgba, duration) in enumerate(raw_frames):
        if duration < MIN_FRAME_MS:
            raise ValueError("帧间隔不得小于 33 ms；生成精确时间轴后再转换")
        durations.append(duration)
        black = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
        frame = Image.alpha_composite(black, rgba).convert("RGB")
        crop_width, crop_height = round(frame.width / zoom), round(frame.height / zoom)
        left, top = (frame.width - crop_width) // 2, (frame.height - crop_height) // 2
        crop_box = (left, top, left + crop_width, top + crop_height)
        bounds = frame.getbbox()
        if bounds and (bounds[0] < crop_box[0] or bounds[1] < crop_box[1]
                       or bounds[2] > crop_box[2] or bounds[3] > crop_box[3]):
            raise ValueError(f"第 {index} 帧超出裁剪窗口，请减小 zoom")
        frames.append(frame.crop(crop_box).resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS))
    durations[-1] = FINAL_HOLD_MS
    reference_palette = reference_final = None
    if reference_resource is not None:
        reference = reference_resource.read_text(encoding="utf-8")
        colors = re.search(r"SPLASH_PALETTE\[[^]]+\] = \{(.*?)\};", reference, re.S)[1]
        reference_palette = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)U", colors)]
        count = int(re.search(r"#define SPLASH_FRAME_COUNT (\d+)U", reference)[1])
        final = re.search(r"SPLASH_FRAME_" + str(count - 1) + r"\[\d+\] = \{(.*?)\};", reference, re.S)[1]
        reference_final = decode_packbits(bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)U", final)))
    stacked = Image.new("RGB", (WIDTH, HEIGHT * len(frames)))
    for index, frame in enumerate(frames):
        stacked.paste(frame, (0, index * HEIGHT))
    if reference_palette is not None:
        raw_palette = []
        for color in reference_palette:
            raw_palette.extend((((color >> 11) & 31) * 255 // 31,
                                ((color >> 5) & 63) * 255 // 63, (color & 31) * 255 // 31))
        palette_image = Image.new("P", (1, 1))
        # Fill all 256 entries using real palette colors; no duplicate padding black indices.
        palette_image.putpalette((raw_palette * 8)[:768])
        palette = reference_palette
    else:
        palette_image = stacked.quantize(colors=COLORS, method=Image.Quantize.FASTOCTREE,
                                         dither=Image.Dither.NONE)
        raw_palette = palette_image.getpalette()[:COLORS * 3]
        palette = [rgb565(raw_palette[index:index + 3]) for index in range(0, len(raw_palette), 3)]
    indexed = [frame.quantize(palette=palette_image, dither=Image.Dither.NONE) for frame in frames]
    if reference_palette is not None:
        indexed = [frame.point(lambda index: index % len(palette)) for frame in indexed]
    if reference_final is not None:
        final_frame = Image.frombytes("P", (WIDTH, HEIGHT), reference_final)
        final_frame.putpalette(palette_image.getpalette())
        indexed[-1] = final_frame
    assert 0 in palette, "透明背景必须包含精确的 RGB565 黑色"
    encoded = [encode_packbits(frame.tobytes()) for frame in indexed]
    source_hash = hashlib.sha256(source_path.read_bytes()).hexdigest()
    lines = [
        "/* 南邮开屏动画资源：桌面“南邮校徽开屏动画（透明版）/preview.gif”。",
        " * 对应 SVG：双向盾形描边 -> 邮字描边/渐显 -> 完整蓝色校徽渐显。",
        " * 原白色区域及透明画布在最终画面中与黑底合成；临时浅色描边随后淡出。",
        f" * 240×240，{fps} fps，{len(indexed)} 帧；播放一次，最后停留 500 ms。",
        " * 30 fps 版本直接从原 SVG 按时间轴采样，保留原色表及最终帧。",
        f" * 校徽居中放大 {zoom:g} 倍（相对完整源画布缩放到屏幕），统一裁去外围留白。",
        " * 32 项 RGB565 色表，Flash 常量帧数据；解码使用现有 PSRAM 帧缓冲。",
        " * 每包首字节：低 7 位 + 1 为像素数（1~128）；bit7=1 后跟重复索引，",
        " * bit7=0 后跟对应数量的逐像素索引。每帧独立编码恰好 57600 像素，",
        " * 可以跳过过期帧；索引色不含 alpha，禁止直接作为 RGB565 发给显示屏。",
        " * 重新生成：tools/generate_njupt_splash.py --source preview.gif --output main/njupt_splash_frames.inc",
        f" * 源文件 SHA256: {source_hash}",
        " */",
        "#ifndef NJUPT_SPLASH_FRAMES_INC", "#define NJUPT_SPLASH_FRAMES_INC", "",
        f"#define SPLASH_WIDTH {WIDTH}U", f"#define SPLASH_HEIGHT {HEIGHT}U",
        f"#define SPLASH_FRAME_COUNT {len(encoded)}U",
        f"#define SPLASH_PALETTE_SIZE {len(palette)}U", f"#define SPLASH_TARGET_FPS {fps}U", "",
        "typedef struct {", "    const uint8_t *data;", "    size_t size;",
        "    uint16_t duration_ms;", "} splash_frame_t;", "",
        f"static const uint16_t SPLASH_PALETTE[{len(palette)}] = {{",
        format_values(palette, 8, lambda value: f"0x{value:04X}U"), "};", "",
    ]
    for index, data in enumerate(encoded):
        lines.extend([f"static const uint8_t SPLASH_FRAME_{index}[{len(data)}] = {{",
                      format_values(data, 20, lambda value: f"0x{value:02X}U"), "};", ""])
    lines.extend([f"static const splash_frame_t SPLASH_FRAMES[{len(encoded)}] = {{",
                  *[f"    {{SPLASH_FRAME_{index}, sizeof(SPLASH_FRAME_{index}), {durations[index]}U}},"
                    for index in range(len(encoded))], "};", "", "#endif", ""])
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    if preview_dir:
        preview_dir.mkdir(parents=True, exist_ok=True)
        previews = []
        for index, frame in enumerate(indexed):
            # 输出与屏幕 RGB565 量化后的预期颜色完全一致，供实际 C 解码器比较。
            rgb = bytearray()
            raw565 = bytearray()
            for color_index in frame.tobytes():
                color = palette[color_index]
                raw565.extend(color.to_bytes(2, "little"))
                rgb.extend((((color >> 11) & 31) * 255 // 31,
                            ((color >> 5) & 63) * 255 // 63, (color & 31) * 255 // 31))
            preview = Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(rgb))
            preview.save(preview_dir / f"{index:02d}.png")
            (preview_dir / f"{index:02d}.rgb565").write_bytes(raw565)
            previews.append(preview)
        gif_durations, timeline = [], 0
        for duration in durations:
            gif_durations.append(round((timeline + duration) / 10) * 10 - round(timeline / 10) * 10)
            timeline += duration
        previews[0].save(preview_dir / "firmware-preview.gif", save_all=True,
                         append_images=previews[1:], duration=gif_durations, loop=0)
        previews[0].save(preview_dir / "firmware-preview.png", save_all=True,
                         append_images=previews[1:], duration=durations, loop=0)
        selected = [round(i * (len(previews) - 1) / 6) for i in range(7)]
        sheet = Image.new("RGB", (WIDTH * len(selected), HEIGHT), "black")
        draw = ImageDraw.Draw(sheet)
        for column, index in enumerate(selected):
            sheet.paste(previews[index], (column * WIDTH, 0))
            draw.text((column * WIDTH + 5, 5), str(index), fill="white")
        sheet.save(preview_dir / "contact-sheet.png")
    print(f"frames={len(encoded)}, zoom={zoom:g}, duration_ms={sum(durations)}, "
          f"flash_data_bytes={sum(map(len, encoded)) + len(palette) * 2}, source_sha256={source_hash}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--preview-dir", type=Path)
    parser.add_argument("--zoom", type=float, default=LOGO_ZOOM,
                        help="校徽居中放大倍数，默认 2；允许 1~2.5，超出画面的帧会报错")
    parser.add_argument("--frame-dir", type=Path, help="SVG 采样得到的 PNG 与 timings.json 目录")
    parser.add_argument("--reference-resource", type=Path, help="保留旧资源的色表和最终帧")
    args = parser.parse_args()
    convert(args.source, args.output, args.preview_dir, args.zoom, args.frame_dir, args.reference_resource)
