/*
 * 240×240 播放器界面，使用现有 RGB565 帧缓冲和差分 SPI 提交。
 * 设计参考 Astra UI 的圆角反白选择器、列表层次和缓动体验：
 * https://github.com/AstraThreshold/oled-ui-astra
 * 本文件为适配当前 C/ESP-IDF 工程的独立实现，不依赖 Astra C++/HAL 或 LVGL。
 * 运行期仅 tft_display 任务访问这里的动画状态；不修改音频、按键或语音业务状态。
 * 所有布局均按 240×240 可见像素定义，外部仍由 BOOT 和矩阵键盘操作，无触摸按钮。
 */

#include "player_ui_fonts.inc"

#define UI_BG       RGB565(9, 12, 17)
#define UI_SURFACE  RGB565(21, 26, 34)
#define UI_BORDER   RGB565(44, 52, 63)
#define UI_INK      RGB565(241, 245, 249)
#define UI_MUTED    RGB565(146, 158, 173)
#define UI_ACCENT   RGB565(103, 229, 211)
#define UI_DARK     RGB565(12, 18, 24)
#define UI_ANIMATION_US 180000LL /* 180 ms 页面入场/选择器缓动，不阻塞音频任务 */

/* 动画只保留显示坐标，不参与模式切换、选曲或音量计算。 */
static int s_ui_page = -1;
static int s_ui_offset_y = 0;
static int64_t s_ui_enter_us = 0;
static bool s_ui_enter_active = false;
static int s_ui_selector_y = 68;
static int s_ui_selector_from = 68;
static int s_ui_selector_target = 68;
static int64_t s_ui_selector_us = 0;
static bool s_ui_selector_active = false;

/** 以 Q10 返回 ease-out cubic 进度，0~1024；按实际微秒计时，不受帧率改变影响。 */
static int ui_ease(int64_t elapsed_us)
{
    if (elapsed_us <= 0) { return 0; }
    if (elapsed_us >= UI_ANIMATION_US) { return 1024; }
    const int64_t remaining = 1024 - elapsed_us * 1024 / UI_ANIMATION_US;
    return 1024 - (int)(remaining * remaining * remaining / (1024LL * 1024LL));
}

/** 判断是否还需要定时绘制动画；最后一帧绘制完才清零，保证最终坐标精确落到目标。 */
static bool ui_animation_pending(void)
{
    return s_ui_enter_active || s_ui_selector_active;
}

/** 更新显示动画，page 编号按设置/电子琴/加载/音乐区分，状态来自已复制的业务快照。 */
static void ui_prepare(const display_update_t *update)
{
    const int page = update->mode == PLAYER_MODE_HOME ? 4 :
        (update->mode == PLAYER_MODE_TRANSFER && !update->sd_loading ? 5 :
        (update->settings_open ? 0 :
        (update->mode == PLAYER_MODE_PIANO ? 1 : (update->sd_loading ? 2 : 3))));
    const int64_t now = esp_timer_get_time();
    const int target = update->settings_target == PLAYER_MODE_PIANO ? 128 : 68;
    if (page != s_ui_page) {
        s_ui_page = page;
        s_ui_enter_us = now;
        s_ui_enter_active = true;
        s_ui_selector_y = s_ui_selector_from = s_ui_selector_target = target;
        s_ui_selector_active = false;
    }
    if (page == 0 && target != s_ui_selector_target) {
        s_ui_selector_from = s_ui_selector_y; /* 连续改选时从当前显示位置继续移动。 */
        s_ui_selector_target = target;
        s_ui_selector_us = now;
        s_ui_selector_active = true;
    }
    if (s_ui_enter_active) {
        const int progress = ui_ease(now - s_ui_enter_us);
        s_ui_offset_y = (12 * (1024 - progress) + 512) / 1024;
        if (progress == 1024) { s_ui_enter_active = false; }
    } else { s_ui_offset_y = 0; }
    if (s_ui_selector_active) {
        const int progress = ui_ease(now - s_ui_selector_us);
        s_ui_selector_y = s_ui_selector_from +
            (s_ui_selector_target - s_ui_selector_from) * progress / 1024;
        if (progress == 1024) {
            s_ui_selector_y = s_ui_selector_target;
            s_ui_selector_active = false;
        }
    }
}

/** 以 0~3 的 alpha 混合两个原生 RGB565 色值，逐通道计算，避免通道进位相互污染。 */
static uint16_t ui_blend(uint16_t background, uint16_t foreground, unsigned alpha)
{
    const unsigned inverse = 3U - alpha;
    const unsigned red = (((background >> 11) & 31U) * inverse +
                          ((foreground >> 11) & 31U) * alpha + 1U) / 3U;
    const unsigned green = (((background >> 5) & 63U) * inverse +
                            ((foreground >> 5) & 63U) * alpha + 1U) / 3U;
    const unsigned blue = ((background & 31U) * inverse +
                           (foreground & 31U) * alpha + 1U) / 3U;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

/** 裁剪后写单个逻辑像素；入场偏移统一在这里应用，字体透明位不会擦除背景。 */
static void ui_pixel(int x, int y, uint16_t color, unsigned alpha)
{
    y += s_ui_offset_y;
    if (x < 0 || y < 0 || x >= (int)TFT_WIDTH || y >= (int)TFT_HEIGHT || alpha == 0U) { return; }
    uint16_t *pixel = &s_tft_framebuffer[(size_t)y * TFT_WIDTH + (size_t)x];
    *pixel = alpha == 3U ? color : ui_blend(*pixel, color, alpha);
}

/** 小型圆角实心矩形，用整数圆方程逐行裁剪角，避免额外图形库或堆分配。 */
static void ui_round_rect(int x, int y, int width, int height, int radius, uint16_t color)
{
    if (width <= 0 || height <= 0) { return; }
    if (radius > width / 2) { radius = width / 2; }
    if (radius > height / 2) { radius = height / 2; }
    for (int row = 0; row < height; ++row) {
        const int dy = row < radius ? radius - row - 1 :
            (row >= height - radius ? row - (height - radius) : 0);
        int inset = 0;
        while (inset < radius && (radius - inset) * (radius - inset) + dy * dy > radius * radius) { ++inset; }
        tft_fill_rect(x + inset, y + row + s_ui_offset_y, width - inset * 2, 1, color);
    }
}

/** 圆角轮廓由外框和内底色组成；适合无渐变的 Astra 风格列表、琴键和状态标签。 */
static void ui_outline(int x, int y, int width, int height, int radius,
                       uint16_t border, uint16_t background)
{
    ui_round_rect(x, y, width, height, radius, border);
    ui_round_rect(x + 1, y + 1, width - 2, height - 2, radius > 0 ? radius - 1 : 0, background);
}

/** 绘制圆盘/圆环：inner=0 为实心圆，否则为内半径，外缘用一像素透明度过渡。 */
static void ui_circle(int cx, int cy, int outer, int inner, uint16_t color)
{
    const int outer2 = outer * outer;
    const int inner2 = inner * inner;
    for (int dy = -outer; dy <= outer; ++dy) {
        for (int dx = -outer; dx <= outer; ++dx) {
            const int distance = dx * dx + dy * dy;
            if (distance > outer2 || (inner > 0 && distance < inner2)) { continue; }
            const int edge = outer2 - distance;
            const unsigned alpha = edge < outer ? 1U : (edge < outer * 2 ? 2U : 3U);
            ui_pixel(cx + dx, cy + dy, color, alpha);
        }
    }
}

/** Bresenham 直线，供无需素材图片的音符/扬声器/状态图标使用。 */
static void ui_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        ui_pixel(x0, y0, color, 3U);
        if (x0 == x1 && y0 == y1) { break; }
        const int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

/** 安全读取一个 UTF-8 码点；非法编码只消耗一字节并显示缺字符号，绝不越过 NUL。 */
static uint32_t ui_utf8_next(const char **text)
{
    const uint8_t *p = (const uint8_t *)*text;
    if (*p == 0U) { return 0U; }
    unsigned count = 1U;
    uint32_t codepoint = p[0];
    if (p[0] >= 0xC2U && p[0] <= 0xDFU) { count = 2U; codepoint = p[0] & 0x1FU; }
    else if (p[0] >= 0xE0U && p[0] <= 0xEFU) { count = 3U; codepoint = p[0] & 0x0FU; }
    else if (p[0] >= 0xF0U && p[0] <= 0xF4U) { count = 4U; codepoint = p[0] & 7U; }
    else if (p[0] >= 0x80U) { ++*text; return 0x25A1U; }
    for (unsigned index = 1U; index < count; ++index) {
        if (p[index] == 0U || (p[index] & 0xC0U) != 0x80U) { ++*text; return 0x25A1U; }
        codepoint = (codepoint << 6) | (p[index] & 0x3FU);
    }
    if ((count == 3U && codepoint < 0x800U) || (count == 4U && codepoint < 0x10000U) ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU) || codepoint > 0x10FFFFU) {
        ++*text; return 0x25A1U;
    }
    *text += count;
    return codepoint;
}

/** 在按码点排序的 Flash 字形表二分查找；大字库缺字可退回完整的小字库。 */
static const ui_glyph_t *ui_find_glyph(const ui_font_t **font, uint32_t codepoint)
{
    size_t first = 0U, last = (*font)->count;
    while (first < last) {
        const size_t middle = first + (last - first) / 2U;
        const ui_glyph_t *glyph = &(*font)->glyphs[middle];
        if (glyph->codepoint == codepoint) { return glyph; }
        if (glyph->codepoint < codepoint) { first = middle + 1U; } else { last = middle; }
    }
    if (*font != &UI_FONT_14) { *font = &UI_FONT_14; return ui_find_glyph(font, codepoint); }
    if (codepoint != 0x25A1U) { return ui_find_glyph(font, 0x25A1U); }
    return NULL;
}

/** 计算 UTF-8 字符串实际像素宽，支持比例字宽；不使用 strlen 作为汉字数量。 */
static int ui_text_width(const char *text, const ui_font_t *font)
{
    int width = 0;
    while (text != NULL && *text != '\0') {
        const uint32_t codepoint = ui_utf8_next(&text);
        const ui_font_t *actual_font = font;
        const ui_glyph_t *glyph = ui_find_glyph(&actual_font, codepoint);
        if (glyph != NULL) { width += glyph->advance; }
    }
    return width;
}

/** 绘制有灰度抗锯齿的 UTF-8 文本，可按整数倍放大并裁剪到指定水平区间。
 * alpha 读取本字形的连续像素索引（行末不另补齐），与 generate_fonts.py 一致。
 * 小字库回退时垂直居中到大字行高；未知字符显示方框，不丢弃后续 UTF-8 字节。
 */
static void ui_text_scaled(int x, int y, const char *text, const ui_font_t *font,
                           unsigned scale, uint16_t color, int clip_left, int clip_right)
{
    if (text == NULL || scale == 0U) { return; }
    while (*text != '\0' && x < clip_right) {
        const uint32_t codepoint = ui_utf8_next(&text);
        const ui_font_t *actual_font = font;
        const ui_glyph_t *glyph = ui_find_glyph(&actual_font, codepoint);
        if (glyph == NULL) { continue; }
        const int top = y + (font->height - actual_font->height) * (int)scale / 2;
        for (unsigned row = 0U; row < actual_font->height; ++row) {
            for (unsigned column = 0U; column < glyph->width; ++column) {
                const unsigned pixel = row * glyph->width + column;
                const unsigned alpha = (actual_font->data[glyph->offset + pixel / 4U] >>
                                         (6U - 2U * (pixel % 4U))) & 3U;
                for (unsigned sy = 0U; sy < scale; ++sy) {
                    for (unsigned sx = 0U; sx < scale; ++sx) {
                        const int px = x + (int)(column * scale + sx);
                        if (px >= clip_left && px < clip_right) {
                            ui_pixel(px, top + (int)(row * scale + sy), color, alpha);
                        }
                    }
                }
            }
        }
        x += glyph->advance * (int)scale;
    }
}

/** 常规文本和居中/右对齐包装，全部共享同一字形测量，避免百分比数字变位。 */
static void ui_text(int x, int y, const char *text, const ui_font_t *font, uint16_t color)
{
    ui_text_scaled(x, y, text, font, 1U, color, 0, TFT_WIDTH);
}

static void ui_center(int y, const char *text, const ui_font_t *font, uint16_t color)
{
    ui_text(((int)TFT_WIDTH - ui_text_width(text, font)) / 2, y, text, font, color);
}

static void ui_right(int right, int y, const char *text, const ui_font_t *font, uint16_t color)
{
    ui_text(right - ui_text_width(text, font), y, text, font, color);
}

/** 在指定宽度内显示一行文字；超长时按完整码点截断并附 …，不会横向挤压字号。 */
static void ui_ellipsize(const char *text, char *output, size_t capacity,
                         int width, const ui_font_t *font)
{
    if (capacity == 0U) { return; }
    output[0] = '\0';
    if (text == NULL) { return; }
    const bool needs_trim = ui_text_width(text, font) > width;
    const int reserve = needs_trim ? ui_text_width("…", font) : 0;
    const char *cursor = text;
    int used_width = 0;
    size_t used_bytes = 0U;
    while (*cursor != '\0') {
        const char *start = cursor;
        const uint32_t codepoint = ui_utf8_next(&cursor);
        const ui_font_t *actual_font = font;
        const ui_glyph_t *glyph = ui_find_glyph(&actual_font, codepoint);
        const int advance = glyph != NULL ? glyph->advance : 0;
        const size_t bytes = (size_t)(cursor - start);
        if (used_width + advance + reserve > width || used_bytes + bytes + 4U > capacity) { break; }
        memcpy(output + used_bytes, start, bytes);
        used_bytes += bytes;
        used_width += advance;
    }
    output[used_bytes] = '\0';
    if (*cursor != '\0' || needs_trim) {
        if (used_bytes + 4U <= capacity) { memcpy(output + used_bytes, "…", 4U); }
    }
}

/** 无图片素材的音符图标，重复用于播放页/设置页，尺寸约 16×18。 */
static void ui_music_icon(int x, int y, uint16_t color)
{
    ui_line(x + 5, y + 4, x + 5, y + 14, color);
    ui_line(x + 14, y + 1, x + 14, y + 11, color);
    ui_line(x + 5, y + 4, x + 14, y + 1, color);
    ui_line(x + 5, y + 5, x + 14, y + 2, color);
    ui_circle(x + 2, y + 14, 3, 0, color);
    ui_circle(x + 11, y + 11, 3, 0, color);
}

/** 迷你键盘图标，按比例画三个白键及两根短黑键的分隔线。 */
static void ui_piano_icon(int x, int y, uint16_t color, uint16_t background)
{
    ui_round_rect(x, y, 18, 15, 2, color);
    ui_line(x + 6, y + 1, x + 6, y + 13, background);
    ui_line(x + 12, y + 1, x + 12, y + 13, background);
    ui_round_rect(x + 4, y, 4, 8, 1, background);
    ui_round_rect(x + 10, y, 4, 8, 1, background);
}

/** 16 像素扬声器图标；音量为零时添加斜线，仅表达真实静音状态。 */
static void ui_volume_icon(int x, int y, uint16_t color, bool muted)
{
    ui_round_rect(x, y + 5, 4, 6, 1, color);
    ui_line(x + 3, y + 5, x + 8, y + 1, color);
    ui_line(x + 8, y + 1, x + 8, y + 15, color);
    ui_line(x + 8, y + 15, x + 3, y + 10, color);
    if (muted) { ui_line(x + 11, y + 3, x + 16, y + 13, color); }
    else {
        ui_line(x + 11, y + 5, x + 13, y + 8, color);
        ui_line(x + 13, y + 8, x + 11, y + 11, color);
        ui_line(x + 14, y + 2, x + 17, y + 8, color);
        ui_line(x + 17, y + 8, x + 14, y + 14, color);
    }
}

/** 页面底色和细分隔线，整屏在 RAM 清空一次，后续 SPI 仍只发送差分区域。 */
static void ui_base(void)
{
    tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, UI_BG);
    ui_round_rect(16, 42, 208, 1, 0, UI_BORDER);
}

/** 右上角状态标签；文字测量后再分配宽度，中文/数字均保持右边距 16 像素。 */
static void ui_badge(const char *label, uint16_t color)
{
    const int width = ui_text_width(label, &UI_FONT_14) + 16;
    ui_round_rect(224 - width, 10, width, 25, 12, UI_SURFACE);
    ui_text(232 - width, 13, label, &UI_FONT_14, color);
}

/** 通用底部操作说明，只描述现有物理按键；不绘制无法点击的触摸控制按钮。 */
static void ui_footer(void)
{
    ui_center(218, "长按 BOOT 返回开始界面", &UI_FONT_14, UI_MUTED);
}

/** 将播放器流进度格式化为 m:ss，不用运行时间猜测音频时长；暂停时进度自然冻结。 */
static void ui_format_time(uint32_t seconds, char *buffer, size_t capacity)
{
    (void)snprintf(buffer, capacity, "%lu:%02lu", (unsigned long)(seconds / 60U),
                   (unsigned long)(seconds % 60U));
}

/** 提取文件名中的歌曲/作者文本，只改显示副本，不改变用于 fopen 的路径。
 * 约定“作者-歌名.wav”可显示作者，其余文件名整段作为歌名；+/_ 显示为空格。
 * 任意长名称在后续按 UTF-8 完整码点做省略；没有元数据时不伪造专辑/作者。
 */
static void ui_song_labels(const display_update_t *update, char *title, size_t title_size,
                            char *artist, size_t artist_size)
{
    title[0] = artist[0] = '\0';
    if (update->song_index >= update->track_count) { return; }
    const char *path = update->track_path;
    const char *base = strrchr(path, '/');
    base = base == NULL ? path : base + 1;
    (void)snprintf(title, title_size, "%s", base);
    const size_t length = strlen(title);
    if (length >= 4U) { title[length - 4U] = '\0'; } /* 登记列表保证扩展名为 .wav。 */
    for (char *p = title; *p != '\0'; ++p) { if (*p == '+' || *p == '_') { *p = ' '; } }
    char *separator = strchr(title, '-');
    if (separator != NULL && separator != title && separator[1] != '\0' &&
        (size_t)(separator - title) < artist_size) {
        memcpy(artist, title, (size_t)(separator - title));
        artist[separator - title] = '\0';
        memmove(title, separator + 1, strlen(separator + 1) + 1U);
    }
}

/** 歌曲核心信息：高对比标题、独立作者/曲目行；过长文本省略，不挤占播放进度。 */
static void tft_draw_song_title(const display_update_t *update)
{
    char title[SD_PATH_BUFFER_BYTES], artist[SD_PATH_BUFFER_BYTES], fitted[96];
    ui_song_labels(update, title, sizeof(title), artist, sizeof(artist));
    ui_ellipsize(title, fitted, sizeof(fitted), 208, &UI_FONT_18);
    ui_center(120, fitted, &UI_FONT_18, UI_INK);
    char metadata[SD_PATH_BUFFER_BYTES + 32U];
    if (artist[0] != '\0') {
        (void)snprintf(metadata, sizeof(metadata), "%s  /  %u/%u", artist,
                       (unsigned)update->song_index + 1U, (unsigned)update->track_count);
    } else {
        (void)snprintf(metadata, sizeof(metadata), "WAV  /  %u/%u",
                       (unsigned)update->song_index + 1U, (unsigned)update->track_count);
    }
    ui_ellipsize(metadata, fitted, sizeof(fitted), 208, &UI_FONT_14);
    ui_center(145, fitted, &UI_FONT_14, UI_MUTED);
}

/** 记录图形只表达播放/暂停/监听状态，不伪装为实时频谱或不存在的专辑封面。 */
static void ui_record(const display_update_t *update)
{
    const uint16_t accent = update->paused ? UI_MUTED : UI_ACCENT;
    ui_circle(120, 83, 32, 30, UI_BORDER);
    ui_circle(120, 83, 24, 23, UI_BORDER);
    ui_circle(120, 83, 17, 16, UI_BORDER);
    ui_circle(120, 83, 10, 0, update->listening ? UI_INK : accent);
    ui_circle(120, 83, 3, 0, UI_BG);
    /* 两侧短线是唱片装饰，没有伪造随机振幅或音量数值。 */
    for (int line = 0; line < 3; ++line) {
        const int length = 14 + line * 6;
        ui_round_rect(64 - length, 75 + line * 8, length, 2, 1, UI_BORDER);
        ui_round_rect(176, 75 + line * 8, length, 2, 1, UI_BORDER);
    }
    if (update->paused) {
        ui_round_rect(115, 76, 3, 14, 1, UI_BG);
        ui_round_rect(122, 76, 3, 14, 1, UI_BG);
    }
}

/** 播放页：状态、唱片标记、UTF-8 歌名/作者、真实时间进度和唤醒提示。 */
static void ui_draw_music(const display_update_t *update)
{
    ui_base();
    ui_music_icon(17, 14, UI_ACCENT);
    ui_text(43, 11, "音乐", &UI_FONT_18, UI_INK);
    const bool empty = update->track_count == 0U;
    ui_badge(update->listening ? "聆听中" : (!update->player_started ? "准备中" :
              (empty ? "无歌曲" : (update->paused ? "已暂停" : "播放中"))), UI_ACCENT);
    if (empty) {
        ui_outline(94, 64, 52, 43, 9, UI_BORDER, UI_SURFACE);
        ui_music_icon(112, 76, UI_MUTED);
        ui_center(123, "未找到歌曲", &UI_FONT_18, UI_INK);
        ui_center(154, "将 WAV 文件放入存储卡", &UI_FONT_14, UI_MUTED);
        ui_center(177, "也可从文件传输上传", &UI_FONT_14, UI_MUTED);
    } else {
        ui_record(update);
        tft_draw_song_title(update);
        const unsigned progress = update->track_progress_permille > 1000U ? 1000U : update->track_progress_permille;
        ui_round_rect(20, 173, 200, 3, 1, UI_BORDER);
        if (progress > 0U) { ui_round_rect(20, 173, (int)(progress * 200U / 1000U), 3, 1, UI_ACCENT); }
        ui_circle(20 + (int)(progress * 200U / 1000U), 174, 3, 0, UI_ACCENT);
        char time[24];
        ui_format_time(update->track_elapsed_seconds, time, sizeof(time));
        ui_text(20, 179, time, &UI_FONT_14, UI_MUTED);
        ui_format_time(update->track_duration_seconds, time, sizeof(time));
        ui_right(220, 179, time, &UI_FONT_14, UI_MUTED);
    }
    if (!empty) {
        ui_volume_icon(20, 199, UI_MUTED, update->volume_percent == 0U);
        char volume[12];
        (void)snprintf(volume, sizeof(volume), "%u%%", (unsigned)update->volume_percent);
        ui_text(43, 198, volume, &UI_FONT_14, UI_INK);
        ui_right(220, 198, update->listening ? "请说指令" :
                 (update->voice_ready ? "你好小鑫唤醒" : "语音关闭"), &UI_FONT_14,
                 update->listening ? UI_ACCENT : UI_MUTED);
    }
    ui_center(218, "短按下一首 / 长按返回", &UI_FONT_14, UI_MUTED);
}

/** 琴键音名颜色与 RGB 的 C 红/D 橙/E 黄/F 绿/G 青/A 蓝/B 紫一致，屏幕上略降低饱和度。 */
static uint16_t ui_note_color(unsigned key)
{
    switch (PIANO_NOTE_NAMES[key][0]) {
        case 'C': return RGB565(255, 126, 150);
        case 'D': return RGB565(255, 183, 112);
        case 'E': return RGB565(245, 215, 113);
        case 'F': return RGB565(124, 224, 158);
        case 'G': return RGB565(103, 229, 211);
        case 'A': return RGB565(133, 172, 255);
        default: return RGB565(201, 156, 255);
    }
}

/** 电子琴页面：保留物理 4×4 布局，唯一按下键反白，顶部大音名与灯色对应。 */
static void tft_draw_piano(const display_update_t *update)
{
    const int active = piano_key_index(update->piano_keys);
    const bool sounding = active >= 0 && !update->paused && !update->listening && update->keypad_ready;
    const uint16_t accent = sounding ? ui_note_color((unsigned)active) : UI_MUTED;
    ui_base();
    ui_piano_icon(17, 16, UI_ACCENT, UI_BG);
    ui_text(43, 11, "电子琴", &UI_FONT_18, UI_INK);
    char volume[12];
    (void)snprintf(volume, sizeof(volume), "%u%%", (unsigned)update->volume_percent);
    ui_badge(volume, UI_INK);
    ui_text_scaled(18, 48, sounding ? PIANO_NOTE_NAMES[active] : "--", &UI_FONT_32,
                    1U, accent, 0, 100);
    const char *hint = !update->keypad_ready ? "请检查键盘" : (update->paused ? "已暂停" :
        (update->piano_keys != 0U && active < 0 ? "仅支持单键" : "按住琴键演奏"));
    ui_right(224, 59, hint, &UI_FONT_14, UI_MUTED);
    ui_right(224, 79, "C4 - D6", &UI_FONT_14, UI_BORDER);
    for (unsigned key = 0U; key < 16U; ++key) {
        const int x = 14 + (int)(key % 4U) * 54;
        const int y = 103 + (int)(key / 4U) * 28;
        const bool pressed = sounding && (int)key == active;
        const uint16_t tint = ui_note_color(key);
        ui_outline(x, y, 50, 24, 7, pressed ? tint : UI_BORDER, pressed ? tint : UI_SURFACE);
        const int label_width = ui_text_width(PIANO_NOTE_NAMES[key], &UI_FONT_14);
        ui_text(x + (50 - label_width) / 2, y + 3, PIANO_NOTE_NAMES[key], &UI_FONT_14,
                 pressed ? UI_DARK : UI_INK);
        if (!pressed) { ui_round_rect(x + 20, y + 21, 10, 1, 0, tint); }
    }
    ui_footer();
    const esp_err_t error = tft_write_framebuffer();
    if (error != ESP_OK) { ESP_LOGW(TAG, "Piano TFT refresh: %s", esp_err_to_name(error)); }
}

/** 设置页的滑动白色选择器；动画只改变坐标，实际音量命令立即生效。 */
static void tft_draw_settings(const display_update_t *update)
{
    ui_base();
    ui_outline(17, 15, 17, 17, 5, UI_MUTED, UI_BG);
    ui_circle(25, 23, 3, 1, UI_ACCENT);
    ui_text(43, 11, "音量设置", &UI_FONT_18, UI_INK);
    ui_text(17, 46, "长按 BOOT 返回开始界面", &UI_FONT_14, UI_MUTED);
    ui_round_rect(14, 68, 212, 50, 11, UI_SURFACE);
    ui_round_rect(14, 128, 212, 50, 11, UI_SURFACE);
    ui_round_rect(14, s_ui_selector_y, 212, 50, 11, UI_INK);
    for (unsigned item = 0U; item < 2U; ++item) {
        const int y = 68 + (int)item * 60;
        /* 移动中按实际选择框覆盖位置反色，避免旧行文字在白框上消失。 */
        const bool covered = y + 22 >= s_ui_selector_y && y + 22 < s_ui_selector_y + 50;
        const uint16_t foreground = covered ? UI_DARK : UI_INK;
        const uint16_t background = covered ? UI_INK : UI_SURFACE;
        const unsigned volume = item == 0U ? update->music_volume_percent : update->piano_volume_percent;
        if (item == 0U) { ui_music_icon(27, y + 10, foreground); }
        else { ui_piano_icon(25, y + 13, foreground, background); }
        ui_text(54, y + 7, item == 0U ? "音乐" : "电子琴", &UI_FONT_18, foreground);
        char value[12];
        (void)snprintf(value, sizeof(value), "%u%%", volume);
        ui_right(212, y + 7, value, &UI_FONT_18, foreground);
        ui_round_rect(54, y + 35, 158, 3, 1, covered ? RGB565(193, 202, 209) : UI_BORDER);
        if (volume > 0U) {
            ui_round_rect(54, y + 35, (int)(volume * 158U / 100U), 3, 1, covered ? UI_DARK : UI_ACCENT);
        }
    }
    static const char *const keys[] = {"音乐", "琴键", "-", "+"};
    for (unsigned key = 0U; key < ARRAY_SIZE(keys); ++key) {
        const int x = 14 + (int)key * 54;
        ui_outline(x, 188, 50, 25, 7, UI_BORDER, UI_BG);
        ui_text(x + (50 - ui_text_width(keys[key], &UI_FONT_14)) / 2, 191,
                 keys[key], &UI_FONT_14, UI_MUTED);
    }
    ui_center(218, "第一行选项 / 长按加减", &UI_FONT_14, UI_MUTED);
    const esp_err_t error = tft_write_framebuffer();
    if (error != ESP_OK) { ESP_LOGW(TAG, "Settings TFT refresh: %s", esp_err_to_name(error)); }
}

/** SD 初始化/重挂载共用的加载页；尝试序号只表示次数，不伪造加载百分比。 */
static esp_err_t tft_show_loading(uint32_t attempt, uint32_t total_attempts)
{
    if (!s_tft_ready || s_tft_framebuffer == NULL) { return ESP_ERR_INVALID_STATE; }
    ui_base();
    ui_text(17, 11, "电子之声", &UI_FONT_18, UI_INK);
    ui_badge("SD", UI_ACCENT);
    ui_outline(99, 64, 42, 49, 8, UI_BORDER, UI_SURFACE);
    ui_round_rect(106, 71, 28, 5, 1, UI_ACCENT);
    for (int pin = 0; pin < 4; ++pin) { ui_round_rect(106 + pin * 8, 85, 4, 13, 1, UI_MUTED); }
    ui_center(125, "加载中", &UI_FONT_18, UI_INK);
    ui_center(156, "正在读取存储卡", &UI_FONT_14, UI_MUTED);
    /* 常规总次数是 6；若以后改大，最多绘 12 点并保持居中。 */
    const unsigned dots = total_attempts > 12U ? 12U : total_attempts;
    const int start_x = 120 - (int)(dots > 0U ? dots - 1U : 0U) * 8;
    for (unsigned dot = 0U; dot < dots; ++dot) {
        ui_circle(start_x + (int)dot * 16, 190, 3, 0,
                   dot + 1U == attempt ? UI_ACCENT : (dot < attempt ? UI_INK : UI_BORDER));
    }
    char progress[32];
    if (attempt == 0U) { (void)snprintf(progress, sizeof(progress), "请稍候"); }
    else { (void)snprintf(progress, sizeof(progress), "重试 %u / %u", (unsigned)attempt, (unsigned)total_attempts); }
    ui_center(213, progress, &UI_FONT_14, UI_MUTED);
    return tft_write_framebuffer();
}

/** 四宫格开始页，选中项使用双像素青色边框，短按 BOOT 按行循环选择。 */
static void ui_transfer_icon(int x, int y, uint16_t color)
{
    ui_line(x, y + 5, x + 22, y + 5, color);
    ui_line(x + 22, y + 5, x + 17, y, color);
    ui_line(x + 22, y + 5, x + 17, y + 10, color);
    ui_line(x + 22, y + 17, x, y + 17, color);
    ui_line(x, y + 17, x + 5, y + 12, color);
    ui_line(x, y + 17, x + 5, y + 22, color);
}

static void tft_draw_home(const display_update_t *update)
{
    static const char *const labels[] = {"音乐播放器", "电子琴", "文件传输", "设置"};
    ui_base();
    ui_text(16, 11, "电子之声", &UI_FONT_18, UI_INK);
    ui_badge("开始", UI_ACCENT);
    for (unsigned item = 0U; item < 4U; ++item) {
        const int x = 14 + (int)(item % 2U) * 112;
        const int y = 52 + (int)(item / 2U) * 82;
        const bool selected = item == update->home_selection;
        ui_round_rect(x, y, 100, 74, 12, selected ? UI_ACCENT : UI_BORDER);
        ui_round_rect(x + 2, y + 2, 96, 70, 10, UI_SURFACE);
        const uint16_t color = selected ? UI_ACCENT : UI_MUTED;
        if (item == 0U) { ui_music_icon(x + 41, y + 12, color); }
        else if (item == 1U) { ui_piano_icon(x + 37, y + 15, color, UI_SURFACE); }
        else if (item == 2U) { ui_transfer_icon(x + 39, y + 10, color); }
        else { ui_circle(x + 50, y + 21, 11, 8, color); ui_circle(x + 50, y + 21, 3, 0, color); }
        ui_text(x + (100 - ui_text_width(labels[item], &UI_FONT_18)) / 2,
                y + 44, labels[item], &UI_FONT_18, selected ? UI_INK : UI_MUTED);
    }
    ui_center(218, "短按选择 / 长按进入", &UI_FONT_14, UI_MUTED);
    const esp_err_t error = tft_write_framebuffer();
    if (error != ESP_OK) { ESP_LOGW(TAG, "Home TFT: %s", esp_err_to_name(error)); }
}

static void tft_draw_transfer(const display_update_t *update)
{
    ui_base();
    ui_transfer_icon(17, 13, UI_ACCENT);
    ui_text(49, 11, "文件传输", &UI_FONT_18, UI_INK);
    ui_badge("Wi-Fi", update->wifi_status < 0 ? UI_MUTED : UI_ACCENT);
    char text[192], fitted[192];
    (void)snprintf(text, sizeof(text), "Wi-Fi: %s", CONFIG_PLAYER_WIFI_AP_SSID);
    ui_ellipsize(text, fitted, sizeof(fitted), 208, &UI_FONT_14);
    ui_text(16, 55, fitted, &UI_FONT_14, UI_INK);
    (void)snprintf(text, sizeof(text), "密码: %s", CONFIG_PLAYER_WIFI_AP_PASSWORD);
    ui_ellipsize(text, fitted, sizeof(fitted), 208, &UI_FONT_14);
    ui_text(16, 78, fitted, &UI_FONT_14, UI_MUTED);
    ui_text(16, 104, "http://192.168.4.1", &UI_FONT_18, UI_ACCENT);
    if (update->wifi_sta_ip != 0U) {
        const esp_ip4_addr_t ip = {.addr = update->wifi_sta_ip};
        (void)snprintf(text, sizeof(text), "路由器: " IPSTR, IP2STR(&ip));
    } else {
        (void)snprintf(text, sizeof(text), "%s", CONFIG_PLAYER_WIFI_STA_SSID[0] == '\0' ?
                       "连接热点后用浏览器打开" : "路由器连接中 / 热点可用");
    }
    ui_text(16, 134, text, &UI_FONT_14, UI_MUTED);
    const char *status = update->wifi_status < 0 ? "Wi-Fi 启动失败，请重试" :
        (update->wifi_status != 2 ? "正在启动 Wi-Fi" :
        (update->transfer_state == 1 ? "正在传输 WAV" :
        (update->transfer_state == 2 ? "传输完成" :
        (update->transfer_state < 0 ? "传输失败，请重试" : "请选择 WAV 上传或下载"))));
    ui_center(160, status, &UI_FONT_14, UI_INK);
    unsigned progress = update->transfer_total == 0U ? 0U :
        (unsigned)((uint64_t)update->transfer_bytes * 100U / update->transfer_total);
    if (progress > 100U) { progress = 100U; }
    ui_round_rect(16, 186, 208, 4, 2, UI_BORDER);
    if (progress > 0U) { ui_round_rect(16, 186, (int)(208U * progress / 100U), 4, 2, UI_ACCENT); }
    (void)snprintf(text, sizeof(text), "%u%%  %lu / %lu KiB", progress,
                   (unsigned long)(update->transfer_bytes / 1024U),
                   (unsigned long)(update->transfer_total / 1024U));
    ui_center(195, text, &UI_FONT_14, UI_MUTED);
    ui_footer();
    const esp_err_t error = tft_write_framebuffer();
    if (error != ESP_OK) { ESP_LOGW(TAG, "Transfer TFT: %s", esp_err_to_name(error)); }
}

/** 统一运行期入口：保持原来的设置 > 电子琴 > 加载 > 音乐优先级。 */
static void tft_draw_status(const display_update_t *update)
{
    if (!s_tft_ready || s_tft_framebuffer == NULL || update == NULL) { return; }
    ui_prepare(update);
    if (update->mode == PLAYER_MODE_HOME) { tft_draw_home(update); return; }
    if (update->settings_open) { tft_draw_settings(update); return; }
    if (update->mode == PLAYER_MODE_PIANO) { tft_draw_piano(update); return; }
    if (update->sd_loading) {
        const esp_err_t error = tft_show_loading(update->sd_mount_attempt, SD_MOUNT_RETRY_COUNT);
        if (error != ESP_OK) { ESP_LOGW(TAG, "SD recovery TFT: %s", esp_err_to_name(error)); }
        return;
    }
    if (update->mode == PLAYER_MODE_TRANSFER) { tft_draw_transfer(update); return; }
    ui_draw_music(update);
    const esp_err_t error = tft_write_framebuffer();
    if (error != ESP_OK) { ESP_LOGW(TAG, "TFT refresh failed: %s", esp_err_to_name(error)); }
}
