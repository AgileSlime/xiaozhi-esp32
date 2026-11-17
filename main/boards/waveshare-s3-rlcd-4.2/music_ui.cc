// 播放器页 UI（400×300 单色 RLCD）—— 与备忘录页同一套视觉语言
//
//   ┌──────────────────────────────────────────────┐
//   │ ╭──────────────────────────────────────────╮ │  白色胶囊头：时间 / 标题 / 电量
//   │ │ 18:07 ▂▃      播放器          86% ▮▮    │ │
//   │ ╰──────────────────────────────────────────╯ │
//   │ ┌──────────────────────────────────────────┐ │
//   │ │    ●●●●●●●    夜航 Nocturne              │ │  白卡：黑唱片 + 曲目 + 歌词
//   │ │   ●  ◜◝◜◝  ●   LUMEN FIELD / 2026        │ │
//   │ │  ●  ( 黑胶 ) ●  ─────────────────────     │ │
//   │ │   ●  ◟◞◟◞  ●   把耳朵交给夜晚            │ │  16px
//   │ │    ●●●●●●●    █ 让风替你翻页 █          │ │  当前句反白 20px
//   │ │                 所有声音都慢了下来        │ │  16px
//   │ └──────────────────────────────────────────┘ │
//   │        |◀      ( ❚❚ )      ▶|                │ 控制（纯状态指示）
//   │  ━━━━━━━━━━━●──────────────────────────      │ 进度 1px 轨道 + 4px 已播
//   │  01:24                              04:32    │
//   └──────────────────────────────────────────────┘
//
// 风格与备忘录页对齐（改版式时请一并遵守）：
//   1. 黑底 + 白色圆角卡。层级靠"整块反白"表达，不靠灰度——单色屏没有灰。
//   2. 顶部是同一条 376×30 白胶囊：左时间、中间标题、右电量，三页位置一致。
//   3. 当前句用"黑底白字"反白条，和备忘录页首行反白是同一个语言：
//      反白 = 此刻正在发生的那一条。
//   4. 屏幕外的控制/进度用白色线条画在黑底上；白卡内的文字是黑字。
//   5. 字号只有三档：20px（标题/当前句）、16px（次级）、14px（状态栏/时间）。
//      不用 text_opa：50% 黑正好等于 flush 阈值 0x7FFF，会被判成白色而消失。
//   6. 唱片是黑底白纹 + 3 个白色指示点公转（同心圆是旋转对称的，光转看不出来）。
//
// 本机没有触摸，控制区只是状态指示，操作全部靠语音。

#include "custom_lcd_display.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>

// 字体：14px 走 1bpp 字库（小字更脆），16px 走 4bpp（有抗锯齿，笔画更稳）
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_puhui_16_4);
// 20px 1bpp 子集，用于曲名与当前句歌词（缺字自动 fallback 到 16px 全量）
LV_FONT_DECLARE(font_puhui_20_1);

LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "MusicUI";

// ===== 布局常量（改版式只需要动这里）=====
namespace player_layout {
constexpr int kScreenW = 400;
constexpr int kScreenH = 300;

// 顶栏：和备忘录页同一条白胶囊
constexpr int kHeadX = 12;
constexpr int kHeadY = 4;
constexpr int kHeadW = 376;
constexpr int kHeadH = 30;
constexpr int kHeadRadius = 13;
constexpr int kBatScale = 213;    // 24px 电池图标 → 20px

// 白卡：装唱片 + 曲目 + 歌词
constexpr int kCardX = 12;
constexpr int kCardY = 40;
constexpr int kCardW = 376;
constexpr int kCardH = 176;
constexpr int kCardRadius = 12;

// 唱片（白卡内，相对卡片坐标）
constexpr int kDiscX = 14;
constexpr int kDiscY = 22;
constexpr int kDiscD = 132;
constexpr int kDiscR = kDiscD / 2;             // 66
constexpr int kLabelD = 52;
constexpr int kPipD = 6;
constexpr int kPipR = 56;                      // 56 + 3 < 66，留在盘内
constexpr int kGrooveFirst = 34;
constexpr int kGrooveStep = 5;
constexpr int kGrooveCount = 7;                // 34,39,...,64（4px 太密，1-bit 上会变斑马纹）

// 右栏（白卡内）
constexpr int kColX = 156;
constexpr int kColW = 208;
constexpr int kTitleY = 12;
constexpr int kArtistY = 38;
constexpr int kRuleY = 62;
constexpr int kLyricX = 152;      // 歌词容器左边（比 kColX 少 4，留给反白条的内缩）
constexpr int kLyricY = 70;
constexpr int kLyricLineH = 28;
constexpr int kLyricTextX = 8;    // 文字相对容器左边距
constexpr int kLyricMaxUnits = 20;

// 控制（黑底上的白线，纯状态指示）
constexpr int kCtrlY = 222;
constexpr int kPlayD = 34;
constexpr int kPlayCx = 200;
constexpr int kPrevCx = 150;
constexpr int kNextCx = 250;
constexpr int kIconW = 20;
constexpr int kIconH = 18;

// 进度
constexpr int kBarX = 24;
constexpr int kBarRight = 376;
constexpr int kBarY = 266;
constexpr int kBarW = kBarRight - kBarX;   // 352
constexpr int kTimesY = 274;
}  // namespace player_layout

using namespace player_layout;

namespace {

// ---------- 画布图标（ARGB8888：像素自带颜色，不依赖 recolor）----------

uint8_t *AllocIconBuf(int w, int h) {
    return static_cast<uint8_t *>(heap_caps_malloc(w * h * 4, MALLOC_CAP_8BIT));
}

lv_obj_t *CreateIcon(lv_obj_t *parent, int w, int h) {
    uint8_t *buf = AllocIconBuf(w, h);
    if (buf == nullptr) {
        ESP_LOGE(TAG, "图标缓冲分配失败 %dx%d", w, h);
        return nullptr;
    }
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_SCROLLABLE);
    return cv;
}

void ClearIcon(lv_obj_t *cv) {
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
}

void FillRectPx(lv_obj_t *cv, int x0, int y0, int w, int h, lv_color_t color) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            lv_canvas_set_px(cv, x, y, color, LV_OPA_COVER);
        }
    }
}

// 实心三角，尖端朝右
void FillTriRight(lv_obj_t *cv, int x0, int y0, int w, int h, lv_color_t color) {
    const int half = (h - 1) / 2;
    for (int i = 0; i < h; i++) {
        const int dist = (i <= half) ? i : (h - 1 - i);
        int len = (dist + 1) * w / (half + 1);
        if (len > w) len = w;
        if (len < 1) len = 1;
        for (int k = 0; k < len; k++) {
            lv_canvas_set_px(cv, x0 + k, y0 + i, color, LV_OPA_COVER);
        }
    }
}

// 实心三角，尖端朝左
void FillTriLeft(lv_obj_t *cv, int x0, int y0, int w, int h, lv_color_t color) {
    const int half = (h - 1) / 2;
    for (int i = 0; i < h; i++) {
        const int dist = (i <= half) ? i : (h - 1 - i);
        int len = (dist + 1) * w / (half + 1);
        if (len > w) len = w;
        if (len < 1) len = 1;
        for (int k = 0; k < len; k++) {
            lv_canvas_set_px(cv, x0 + w - 1 - k, y0 + i, color, LV_OPA_COVER);
        }
    }
}

// UTF-8 安全截断，按"显示宽度"算而不是字符数：
// 汉字算 2 个单位（约 20px），ASCII 算 1 个（约 10px）。
// 这样中文歌词不会挤爆、英文歌词也不会被砍掉一半。
std::string ClampUtf8(const std::string &text, int max_units) {
    size_t i = 0;
    int used = 0;
    size_t cut = std::string::npos;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t step = 1;
        int units = 1;
        if ((c & 0x80) == 0) {
            step = 1;
            units = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
            units = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
            units = 2;      // 中日韩汉字（3 字节）按全角算
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
            units = 2;
        }
        if (used + units > max_units) {
            cut = i;
            break;
        }
        used += units;
        i += step;
    }
    if (cut == std::string::npos) {
        return text;
    }
    return text.substr(0, cut) + "…";
}

// 实心小条（信号条、分隔线、进度轨道通用）
void MakeBar(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t color) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
}

void StylePlainBox(lv_obj_t *obj, lv_color_t bg, lv_opa_t opa, int radius) {
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

// ============================================================
// 页面搭建
// ============================================================

void CustomLcdDisplay::SetupMusicUI() {
    DisplayLockGuard lock(this);

    lv_obj_t *root = lv_screen_active();

    const lv_font_t *font_lg = &font_puhui_20_1;  // 曲名 / 当前歌词 20px
    const lv_font_t *font_cn = &font_puhui_16_4;  // 次级中文 16px
    const lv_font_t *font_sm = &font_puhui_14_1;  // 状态栏 / 时间 14px

    // ===== 页面容器（黑底，和主页、备忘录页同一块底色）=====
    music_page_ = lv_obj_create(root);
    lv_obj_set_size(music_page_, kScreenW, kScreenH);
    lv_obj_set_pos(music_page_, 0, 0);
    lv_obj_set_style_bg_color(music_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(music_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_page_, 0, 0);
    lv_obj_set_style_radius(music_page_, 0, 0);
    lv_obj_set_style_pad_all(music_page_, 0, 0);
    lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *page = music_page_;

    // ================= 顶栏：白色胶囊 =================
    lv_obj_t *head = lv_obj_create(page);
    lv_obj_set_pos(head, kHeadX, kHeadY);
    lv_obj_set_size(head, kHeadW, kHeadH);
    StylePlainBox(head, lv_color_white(), LV_OPA_COVER, kHeadRadius);

    music_time_label_ = lv_label_create(head);
    lv_obj_set_style_text_font(music_time_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_time_label_, lv_color_black(), 0);
    lv_label_set_text(music_time_label_, "--:--");
    lv_obj_align(music_time_label_, LV_ALIGN_LEFT_MID, 11, 0);

    // 信号条（3 根递减短线，三页同一套画法）
    for (int i = 0; i < 3; i++) {
        MakeBar(head, 52, 10 + i * 4, 13 - i * 4, 2, lv_color_black());
    }

    lv_obj_t *title = lv_label_create(head);
    lv_obj_set_style_text_font(title, font_lg, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_label_set_text(title, "播放器");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    music_battery_pct_label_ = lv_label_create(head);
    lv_obj_set_style_text_font(music_battery_pct_label_, font_sm, 0);
    lv_obj_set_style_text_color(music_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(music_battery_pct_label_, "---%");
    lv_obj_align(music_battery_pct_label_, LV_ALIGN_RIGHT_MID, -34, 0);

    music_battery_icon_img_ = lv_image_create(head);
    lv_image_set_src(music_battery_icon_img_, &ui_img_battery_full);
    lv_image_set_scale(music_battery_icon_img_, kBatScale);
    lv_obj_align(music_battery_icon_img_, LV_ALIGN_RIGHT_MID, -10, 0);

    // ================= 白卡：唱片 + 曲目 + 歌词 =================
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_pos(card, kCardX, kCardY);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, kCardRadius, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 左侧：黑唱片（白卡上的黑圆，纹路是 1px 白线）----
    music_disc_ = lv_obj_create(card);
    lv_obj_set_pos(music_disc_, kDiscX, kDiscY);
    lv_obj_set_size(music_disc_, kDiscD, kDiscD);
    StylePlainBox(music_disc_, lv_color_black(), LV_OPA_COVER, LV_RADIUS_CIRCLE);

    for (int i = 0; i < kGrooveCount; i++) {
        const int r = kGrooveFirst + i * kGrooveStep;
        lv_obj_t *ring = lv_obj_create(music_disc_);
        lv_obj_set_size(ring, r * 2, r * 2);
        lv_obj_center(ring);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_white(), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(ring, 0, 0);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }

    // 中心白标
    lv_obj_t *label = lv_obj_create(music_disc_);
    lv_obj_set_size(label, kLabelD, kLabelD);
    lv_obj_center(label);
    StylePlainBox(label, lv_color_white(), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_style_clip_corner(label, true, 0);

    lv_obj_t *side = lv_label_create(label);
    lv_obj_set_style_text_font(side, font_sm, 0);
    lv_obj_set_style_text_color(side, lv_color_black(), 0);
    lv_obj_set_style_text_align(side, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(side, kLabelD);
    lv_obj_set_pos(side, 0, 11);
    lv_label_set_text(side, "SIDE A");

    // 中心轴孔
    lv_obj_t *hole = lv_obj_create(label);
    lv_obj_set_size(hole, 6, 6);
    lv_obj_set_pos(hole, kLabelD / 2 - 3, 31);
    StylePlainBox(hole, lv_color_black(), LV_OPA_COVER, LV_RADIUS_CIRCLE);

    // 旋转指示点（3 个，120° 均布）
    for (int i = 0; i < 3; i++) {
        lv_obj_t *pip = lv_obj_create(music_disc_);
        lv_obj_set_size(pip, kPipD, kPipD);
        StylePlainBox(pip, lv_color_white(), LV_OPA_COVER, LV_RADIUS_CIRCLE);
        // 1px 黑描边：白点跨在白色纹路上时，靠这圈黑边保住轮廓
        lv_obj_set_style_border_width(pip, 1, 0);
        lv_obj_set_style_border_color(pip, lv_color_black(), 0);
        lv_obj_set_style_border_opa(pip, LV_OPA_COVER, 0);
        music_pip_[i] = pip;
    }
    music_pip_step_ = 10;
    UpdateMusicPips();

    // ---- 右侧：曲名 / 歌手 / 分隔线 ----
    music_title_label_ = lv_label_create(card);
    lv_obj_set_style_text_font(music_title_label_, font_lg, 0);
    lv_obj_set_style_text_color(music_title_label_, lv_color_black(), 0);
    lv_obj_set_pos(music_title_label_, kColX, kTitleY);
    lv_obj_set_width(music_title_label_, kColW);
    lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_title_label_, "未播放");

    music_artist_label_ = lv_label_create(card);
    lv_obj_set_style_text_font(music_artist_label_, font_cn, 0);
    lv_obj_set_style_text_color(music_artist_label_, lv_color_black(), 0);
    lv_obj_set_pos(music_artist_label_, kColX, kArtistY);
    lv_obj_set_width(music_artist_label_, kColW);
    lv_label_set_long_mode(music_artist_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_artist_label_, "—");

    MakeBar(card, kColX, kRuleY, kColW, 1, lv_color_black());

    // ---- 歌词：3 行一起上下滚动（协议只给上/当前/下三句）----
    music_lyric_cont_ = lv_obj_create(card);
    lv_obj_set_pos(music_lyric_cont_, kLyricX, kLyricY);
    lv_obj_set_size(music_lyric_cont_, kColW + 4, kLyricLineH * 3);
    StylePlainBox(music_lyric_cont_, lv_color_black(), LV_OPA_TRANSP, 0);
    lv_obj_set_style_clip_corner(music_lyric_cont_, true, 0);

    // 当前句的反白条：和备忘录页首行反白是同一个语言
    music_lyric_hl_ = lv_obj_create(music_lyric_cont_);
    lv_obj_set_pos(music_lyric_hl_, 0, kLyricLineH);
    lv_obj_set_size(music_lyric_hl_, kColW + 4, kLyricLineH);
    StylePlainBox(music_lyric_hl_, lv_color_black(), LV_OPA_TRANSP, 8);
    lv_obj_add_flag(music_lyric_hl_, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *line = lv_label_create(music_lyric_cont_);
        const bool cur = (i == 1);
        lv_obj_set_style_text_font(line, cur ? font_lg : font_cn, 0);
        lv_obj_set_style_text_color(line, cur ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_pos(line, kLyricTextX, i * kLyricLineH + (cur ? 3 : 5));
        lv_obj_set_size(line, kColW + 4 - kLyricTextX, kLyricLineH - 2);
        lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
        lv_label_set_text(line, "");
        music_lyric_line_[i] = line;
    }

    // 旧版的"当前句圆点"已被反白条取代，对象保留但永远隐藏，
    // 免得基类或旧刷新逻辑拿到空指针。
    music_lyric_dot_ = lv_obj_create(music_lyric_cont_);
    lv_obj_set_size(music_lyric_dot_, 1, 1);
    StylePlainBox(music_lyric_dot_, lv_color_black(), LV_OPA_TRANSP, 0);
    lv_obj_add_flag(music_lyric_dot_, LV_OBJ_FLAG_HIDDEN);

    // ================= 控制区（黑底白线，纯状态指示）=================
    // 上一首 |◀
    music_prev_icon_ = CreateIcon(page, kIconW, kIconH);
    if (music_prev_icon_ != nullptr) {
        lv_obj_set_pos(music_prev_icon_, kPrevCx - kIconW / 2, kCtrlY + (kPlayD - kIconH) / 2);
        FillRectPx(music_prev_icon_, 0, 0, 2, kIconH, lv_color_white());
        FillTriLeft(music_prev_icon_, 4, 1, 16, 16, lv_color_white());
    }
    // 下一首 ▶|
    music_next_icon_ = CreateIcon(page, kIconW, kIconH);
    if (music_next_icon_ != nullptr) {
        lv_obj_set_pos(music_next_icon_, kNextCx - kIconW / 2, kCtrlY + (kPlayD - kIconH) / 2);
        FillTriRight(music_next_icon_, 0, 1, 16, 16, lv_color_white());
        FillRectPx(music_next_icon_, 18, 0, 2, kIconH, lv_color_white());
    }

    // 播放 / 暂停：白圆钮 + 黑图标
    lv_obj_t *play_btn = lv_obj_create(page);
    lv_obj_set_pos(play_btn, kPlayCx - kPlayD / 2, kCtrlY);
    lv_obj_set_size(play_btn, kPlayD, kPlayD);
    StylePlainBox(play_btn, lv_color_white(), LV_OPA_COVER, LV_RADIUS_CIRCLE);

    music_play_icon_ = CreateIcon(play_btn, 18, 18);
    if (music_play_icon_ != nullptr) {
        lv_obj_center(music_play_icon_);
        RedrawPlayIcon();
    }

    // ================= 进度（白线画在黑底上）=================
    MakeBar(page, kBarX, kBarY, kBarW, 1, lv_color_white());

    music_bar_fill_ = lv_obj_create(page);
    lv_obj_set_pos(music_bar_fill_, kBarX, kBarY - 2);
    lv_obj_set_size(music_bar_fill_, 0, 4);
    StylePlainBox(music_bar_fill_, lv_color_white(), LV_OPA_COVER, 0);

    music_bar_knob_ = lv_obj_create(page);
    lv_obj_set_pos(music_bar_knob_, kBarX - 4, kBarY - 4);
    lv_obj_set_size(music_bar_knob_, 9, 9);
    StylePlainBox(music_bar_knob_, lv_color_white(), LV_OPA_COVER, LV_RADIUS_CIRCLE);

    music_time_cur_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_time_cur_, font_sm, 0);
    lv_obj_set_style_text_color(music_time_cur_, lv_color_white(), 0);
    lv_obj_set_pos(music_time_cur_, kBarX, kTimesY);
    lv_label_set_text(music_time_cur_, "00:00");

    music_time_total_ = lv_label_create(page);
    lv_obj_set_style_text_font(music_time_total_, font_sm, 0);
    lv_obj_set_style_text_color(music_time_total_, lv_color_white(), 0);
    lv_obj_align(music_time_total_, LV_ALIGN_TOP_RIGHT, -(kScreenW - kBarRight), kTimesY);
    lv_label_set_text(music_time_total_, "--:--");

    // ================= 给基类留的隐藏占位 =================
    // SetChatMessage / SetEmotion / Alert 会写这些句柄，播放器页不显示它们，
    // 但必须是合法对象，否则基类写空指针会崩。
    music_chat_status_label_ = lv_label_create(page);
    lv_label_set_text(music_chat_status_label_, "");
    lv_obj_add_flag(music_chat_status_label_, LV_OBJ_FLAG_HIDDEN);

    music_emotion_label_ = lv_label_create(page);
    lv_label_set_text(music_emotion_label_, "");
    lv_obj_add_flag(music_emotion_label_, LV_OBJ_FLAG_HIDDEN);

    music_emotion_img_ = lv_image_create(page);
    lv_obj_add_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);

    // ================= 唱片转动定时器 =================
    lv_timer_t *pip_timer = lv_timer_create(MusicPipTimerCb, 150, this);
    if (pip_timer != nullptr) {
        lv_timer_set_repeat_count(pip_timer, -1);
    }

    ESP_LOGI(TAG, "播放器页创建完成（唱片 %d / 歌词 3 行 / 进度 %d）", kDiscD, kBarW);
}

// ============================================================
// 播放器页刷新
// ============================================================

void CustomLcdDisplay::UpdateMusicPips() {
    if (music_pip_[0] == nullptr) {
        return;
    }
    const float base = static_cast<float>(music_pip_angle_) * 3.14159265f / 180.0f;
    for (int i = 0; i < 3; i++) {
        const float a = base + static_cast<float>(i) * 2.0943951f;   // 120°
        const int px = static_cast<int>(kDiscR + kPipR * cosf(a) - kPipD / 2.0f);
        const int py = static_cast<int>(kDiscR + kPipR * sinf(a) - kPipD / 2.0f);
        lv_obj_set_pos(music_pip_[i], px, py);
    }
    // 三个点分散在圆周上，LVGL 会拆成 6 个脏矩形 → 6 次整帧 SPI。
    // 主动把整块唱片标脏，让它们合并成一次刷新，既省总线也不会在 DMA 传输中反复改同一块显存。
    lv_obj_invalidate(music_disc_);
}

void CustomLcdDisplay::MusicPipTimerCb(lv_timer_t *t) {
    auto *self = static_cast<CustomLcdDisplay *>(lv_timer_get_user_data(t));
    if (self == nullptr || self->music_disc_ == nullptr) {
        return;
    }
    // 只有"在放歌 + 播放器页可见"才转，别的时候一次都不要刷屏
    if (!self->music_is_playing_ || self->music_is_paused_) {
        return;
    }
    if (lv_obj_has_flag(self->music_page_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    self->music_pip_angle_ = (self->music_pip_angle_ + self->music_pip_step_) % 360;
    self->UpdateMusicPips();
}

void CustomLcdDisplay::RedrawPlayIcon() {
    if (music_play_icon_ == nullptr) {
        return;
    }
    ClearIcon(music_play_icon_);
    // 白圆钮内的图标必须是黑的，否则看不见
    if (music_is_playing_ && !music_is_paused_) {
        // 暂停符号：两根 4×16 竖条
        FillRectPx(music_play_icon_, 4, 1, 4, 16, lv_color_black());
        FillRectPx(music_play_icon_, 10, 1, 4, 16, lv_color_black());
    } else {
        // 播放符号：三角
        FillTriRight(music_play_icon_, 5, 1, 12, 16, lv_color_black());
    }
}

void CustomLcdDisplay::SetMusicPlayState(bool playing, bool paused) {
    DisplayLockGuard lock(this);
    if (music_is_playing_ == playing && music_is_paused_ == paused) {
        return;
    }
    music_is_playing_ = playing;
    music_is_paused_ = paused;
    RedrawPlayIcon();
}

void CustomLcdDisplay::SetMusicInfo(const char *title, const char *artist) {
    DisplayLockGuard lock(this);
    if (music_title_label_ == nullptr || music_artist_label_ == nullptr) {
        return;
    }
    lv_label_set_text(music_title_label_, (title != nullptr && title[0] != '\0') ? title : "未知歌曲");
    lv_label_set_text(music_artist_label_, (artist != nullptr && artist[0] != '\0') ? artist : "未知歌手");
}

void CustomLcdDisplay::SetMusicLyric(const char *lyric) {
    DisplayLockGuard lock(this);
    UpdateMusicLyricLines(lyric);
}

void CustomLcdDisplay::UpdateMusicLyricLines(const char *lyric) {
    if (music_lyric_line_[0] == nullptr) {
        return;
    }

    const std::string text(lyric != nullptr ? lyric : "");

    // 歌词只在整句切换时才该重画，进度刷新非常频繁，必须去重
    static std::string last_text;
    if (text == last_text) {
        return;
    }
    last_text = text;

    // 按行拆分（协议：上一句\n当前句\n下一句）
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size() && lines.size() < 3) {
        const size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }

    std::string slots[3];
    if (lines.size() == 1) {
        // 单行文本（"播放失败：…"这类提示）放进当前行
        slots[1] = lines[0];
    } else {
        for (size_t i = 0; i < lines.size() && i < 3; i++) {
            slots[i] = lines[i];
        }
    }

    for (int i = 0; i < 3; i++) {
        lv_label_set_text(music_lyric_line_[i], ClampUtf8(slots[i], kLyricMaxUnits).c_str());
    }

    // 反白条只在真有当前句时显示；空歌词时留白比一条黑杠干净
    if (slots[1].empty()) {
        lv_obj_add_flag(music_lyric_hl_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(music_lyric_hl_, LV_OBJ_FLAG_HIDDEN);
    }

    // 滚动：整块从下方一行的位置滑回来，配合"当前句固定在第 2 行"读起来就是歌词在往上走。
    // 注意歌词容器现在是白卡的子对象，坐标是卡片内相对坐标（不是屏幕坐标）。
    lv_anim_delete(music_lyric_cont_, nullptr);
    lv_obj_set_y(music_lyric_cont_, kLyricY + kLyricLineH);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, music_lyric_cont_);
    lv_anim_set_values(&a, kLyricY + kLyricLineH, kLyricY);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t value) {
        lv_obj_set_y(static_cast<lv_obj_t *>(obj), value);
    });
    lv_anim_start(&a);
}

void CustomLcdDisplay::SetMusicProgress(uint32_t current_ms, uint32_t total_ms) {
    DisplayLockGuard lock(this);
    if (music_bar_fill_ == nullptr || music_time_cur_ == nullptr) {
        return;
    }

    // 播放任务每解码一块（~90ms）就调一次；这里按"秒"去重，
    // 否则每秒会触发 10 多次整屏 SPI，1-bit 屏上看着就是闪。
    const int sec = static_cast<int>(current_ms / 1000);
    if (sec == music_last_sec_) {
        return;
    }
    music_last_sec_ = sec;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u",
             static_cast<unsigned>(sec / 60), static_cast<unsigned>(sec % 60));
    lv_label_set_text(music_time_cur_, buf);

    if (total_ms <= 1000) {
        // 拿不到总时长（网络流且没有歌词）：不画比例，只显示已播时间
        lv_label_set_text(music_time_total_, "--:--");
        lv_obj_set_width(music_bar_fill_, 0);
        lv_obj_set_pos(music_bar_knob_, kBarX - 4, kBarY - 4);
        return;
    }

    const unsigned total_sec = total_ms / 1000;
    snprintf(buf, sizeof(buf), "%02u:%02u", total_sec / 60, total_sec % 60);
    lv_label_set_text(music_time_total_, buf);

    int width = static_cast<int>(static_cast<uint64_t>(kBarW) * current_ms / total_ms);
    if (width < 0) width = 0;
    if (width > kBarW) width = kBarW;
    lv_obj_set_width(music_bar_fill_, width);
    lv_obj_set_pos(music_bar_knob_, kBarX + width - 4, kBarY - 4);
}
