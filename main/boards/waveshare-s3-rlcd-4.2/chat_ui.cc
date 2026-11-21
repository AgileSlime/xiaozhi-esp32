// 对话显示（400×300 单色 RLCD）
//
// 分两层，一层一层来：
//
//  ① 每页一张"小对话卡"（长在本页版式里，不是覆盖全屏）
//     ┌ 天气页 ─────────────────────────────┐   ┌ 备忘录页 ──────────────────────────┐
//     │ 23.5°C            [wifi][bat] 86%  │   │ 18:07   备忘录        86% ▮▮        │
//     │ ┌───────────┬────────────────────┐ │   │ ╭────────────────────────────────╮ │
//     │ │           │ 11:42              │ │   │ │ ●聆听中                对 话     │ │
//     │ │   照片     ├────────────────────┤ │   │ │ ▛ 放首晴天              ▛      │ │
//     │ │ 214×252   │ ●聆听中     00:06  │ │   │ │   好的，正在播放《晴天》        │ │
//     │ │           │ ▛ 放首晴天       ▛  │ │   │ │ ─────────────────────────────  │ │
//     │ │           │   好的，正在播放…  │ │   │ │ ▛ 明早七点叫我          ▛      │ │
//     │ └───────────┴────────────────────┘ │   │ │   好的，七点整叫你起床          │ │
//     └────────────────────────────────────┘   │ ╰────────────────────────────────╯ │
//      沿用原来的 voice_card_（158×142）        └────────────────────────────────────┘
//                                               盖住闹钟列表，说完自动还回来
//     ┌ 播放器页 ──────────────────────────────────────────────────┐
//     │ 18:07      播放器          86% ▮▮                          │
//     │ ┌───────────────────────┬──────────────────────────────┐   │
//     │ │      ●●●●●●●          │ ●回答中              对 话   │   │
//     │ │     ● 黑胶 ●          │ ▛ 现在放的是什么        ▛    │   │
//     │ │      ●●●●●●●          │   正在放《晴天》— 周杰伦     │   │
//     │ └───────────────────────┴──────────────────────────────┘   │
//     └────────────────────────────────────────────────────────────┘
//      左唱片照转，右栏的曲名/歌词临时让给对话
//
//  三页的小卡都是"白底不透明 + 后创建"，盖住就等于藏起来，
//  所以不需要挨个去隐藏本页控件，也没有半透明（1-bit 屏没有灰）。
//
//  什么时候出现：连接中 / 聆听中 / 回答中。空闲立刻收回，页面恢复原样。
//  工具跳页（放歌、翻目录、换图）那一轮不出现，把结果页完整让出来。
//
//  ② 放大的对话记录层（全屏）
//     用户说「放大对话列表」才铺满一屏，显示最近 3 轮；说「收起」或空闲 12 秒自动收回。
//     平时绝不自己冒出来——这是这一版和上一版最大的区别。
//
//  数据只有一份：chat_turns_（最多 3 轮）。两层的刷新都从这里取。
//  同一条回复可能分多次发过来（sentence_start 逐句），按轮拼接。

#include "custom_lcd_display.h"
#include "device_state.h"

#include <cstdio>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);

static const char* TAG = "ChatUI";

namespace chat_layout {
constexpr int kScreenW = 400;
constexpr int kScreenH = 300;

// ① 备忘录页小卡：就放在列表卡那一格（列表卡 12,40 376×214）
constexpr int kMemoChatX = 12;
constexpr int kMemoChatY = 40;
constexpr int kMemoChatW = 376;
constexpr int kMemoChatH = 214;

// ② 播放器页小卡：白卡右栏（白卡 12,40 376×176，右栏卡内 x=156 起）
constexpr int kMusicChatX = 168;
constexpr int kMusicChatY = 48;
constexpr int kMusicChatW = 208;
constexpr int kMusicChatH = 160;

// ③ 放大的全屏层
constexpr int kHeadX = 12;
constexpr int kHeadY = 4;
constexpr int kHeadW = 376;
constexpr int kHeadH = 30;

constexpr int kCardX = 12;
constexpr int kCardY = 40;
constexpr int kCardW = 376;
constexpr int kCardH = 248;

constexpr int kGroupH = 80;      // 放大层一轮占多高（用户行 + AI 两行）
constexpr int kUserH = 26;
constexpr int kAiH = 40;
constexpr int kTurnCount = 3;    // 一屏最多三轮
}  // namespace chat_layout

// UTF-8 安全截断（保留尾部，前面加省略号）。空间固定，长句子必须截。
static std::string ChatTail(const std::string& text, size_t max_chars) {
    if (text.empty() || max_chars == 0) {
        return text;
    }
    size_t i = 0, count = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0x80) == 0) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        count++;
    }
    if (count <= max_chars) {
        return text;
    }

    const size_t skip = count - max_chars;
    i = 0;
    size_t seen = 0;
    while (i < text.size() && seen < skip) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0x80) == 0) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        seen++;
    }
    return "…" + text.substr(i);
}

namespace {

void PlainBox(lv_obj_t* obj, lv_color_t bg, lv_opa_t opa, int radius) {
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

// ============================================================
// ① 每页的小对话卡
// ============================================================

void CustomLcdDisplay::SetupPageChatWindow(PageChatWindow& win, lv_obj_t* parent,
                                           int x, int y, int width, int height,
                                           int badge_w, int turn_y0, int turn_gap,
                                           int user_h, int ai_h, bool with_tip) {
    if (parent == nullptr) {
        return;
    }

    const lv_font_t* font_sm = &font_puhui_14_1;    // 徽标 / 右上"对 话" / 底部提示
    const lv_font_t* font_body = &font_puhui_16_4;  // 对话正文

    // ---- 白纸片：不透明白底，直接盖住下面的内容 ----
    win.card = lv_obj_create(parent);
    lv_obj_set_pos(win.card, x, y);
    lv_obj_set_size(win.card, width, height);
    lv_obj_set_style_bg_color(win.card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(win.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win.card, 0, 0);
    lv_obj_set_style_radius(win.card, 12, 0);
    lv_obj_set_style_pad_all(win.card, 0, 0);
    lv_obj_set_style_clip_corner(win.card, true, 0);
    lv_obj_remove_flag(win.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(win.card, LV_OBJ_FLAG_HIDDEN);

    // ---- 顶部黑胶囊：状态 ----
    win.badge = lv_obj_create(win.card);
    lv_obj_set_pos(win.badge, 10, 7);
    lv_obj_set_size(win.badge, badge_w, 22);
    PlainBox(win.badge, lv_color_black(), LV_OPA_COVER, 11);

    lv_obj_t* dot = lv_obj_create(win.badge);
    lv_obj_set_size(dot, 6, 6);
    PlainBox(dot, lv_color_white(), LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 9, 0);

    win.badge_label = lv_label_create(win.badge);
    lv_obj_set_style_text_font(win.badge_label, font_sm, 0);
    lv_obj_set_style_text_color(win.badge_label, lv_color_white(), 0);
    lv_label_set_text(win.badge_label, "聆听中");
    lv_obj_align(win.badge_label, LV_ALIGN_LEFT_MID, 21, 0);

    // ---- 右上小字：这是对话，不是提醒列表 ----
    win.title = lv_label_create(win.card);
    lv_obj_set_style_text_font(win.title, font_sm, 0);
    lv_obj_set_style_text_color(win.title, lv_color_black(), 0);
    lv_label_set_text(win.title, "对  话");
    lv_obj_align(win.title, LV_ALIGN_TOP_RIGHT, -12, 10);

    // ---- 最近两轮：越往下越新 ----
    const int row_x = (width >= 300) ? 8 : 6;
    const int row_w = width - row_x * 2;
    const int ai_x = row_x + 4;
    const int ai_w = row_w - 8;

    for (int i = 0; i < 2; i++) {
        const int ty = turn_y0 + i * turn_gap;

        // 用户：反白块（黑底白字），和备忘录首行、歌词当前句同一套语言
        win.user_row[i] = lv_obj_create(win.card);
        lv_obj_set_pos(win.user_row[i], row_x, ty);
        lv_obj_set_size(win.user_row[i], row_w, user_h);
        PlainBox(win.user_row[i], lv_color_black(), LV_OPA_COVER, 7);
        lv_obj_set_style_pad_left(win.user_row[i], 8, 0);
        lv_obj_set_style_pad_right(win.user_row[i], 8, 0);
        lv_obj_add_flag(win.user_row[i], LV_OBJ_FLAG_HIDDEN);

        win.user_label[i] = lv_label_create(win.user_row[i]);
        lv_obj_set_style_text_font(win.user_label[i], font_body, 0);
        lv_obj_set_style_text_color(win.user_label[i], lv_color_white(), 0);
        lv_obj_set_width(win.user_label[i], row_w - 16);
        lv_label_set_long_mode(win.user_label[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(win.user_label[i], "");
        lv_obj_align(win.user_label[i], LV_ALIGN_LEFT_MID, 0, 0);

        // AI：黑字，最多两行（具体字数按卡宽截）
        win.ai_label[i] = lv_label_create(win.card);
        lv_obj_set_pos(win.ai_label[i], ai_x, ty + user_h + 3);
        lv_obj_set_size(win.ai_label[i], ai_w, ai_h);
        lv_obj_set_style_text_font(win.ai_label[i], font_body, 0);
        lv_obj_set_style_text_color(win.ai_label[i], lv_color_black(), 0);
        lv_obj_set_style_text_align(win.ai_label[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(win.ai_label[i], LV_LABEL_LONG_WRAP);
        lv_label_set_text(win.ai_label[i], "");
    }

    // 底部提示：告诉用户还有"放大"这一档
    if (with_tip) {
        lv_obj_t* tip = lv_label_create(win.card);
        lv_obj_set_style_text_font(tip, font_sm, 0);
        lv_obj_set_style_text_color(tip, lv_color_black(), 0);
        lv_label_set_text(tip, "说「放大对话列表」看完整记录");
        lv_obj_set_pos(tip, 12, height - 28);
    }

    // 按卡宽算截断长度：16px 汉字约 19px 宽
    win.max_user_chars = (width / 19 > 4) ? (width / 19) : 4;
    win.max_ai_chars = win.max_user_chars * 2 - 2;

    ESP_LOGI(TAG, "对话小卡已创建 %dx%d @ (%d,%d)", width, height, x, y);
}

void CustomLcdDisplay::SetupPageChatWindows() {
    DisplayLockGuard lock(this);
    using namespace chat_layout;

    // 备忘录页：直接占闹钟列表那一格（12,40 376×214）。
    // 两轮对话 + 底部一行"怎么放大"的提示。
    SetupPageChatWindow(memo_chat_, memo_page_,
                        kMemoChatX, kMemoChatY, kMemoChatW, kMemoChatH,
                        86, 34, 70, 24, 40, true);

    // 播放器页：占白卡右栏（曲名 / 歌手 / 歌词那一列），左边唱片继续转，
    // 下面的控制键和进度条也不动——"我还在放歌"这个信息不能丢。
    SetupPageChatWindow(music_chat_, music_page_,
                        kMusicChatX, kMusicChatY, kMusicChatW, kMusicChatH,
                        86, 32, 58, 22, 34, false);

    ESP_LOGI(TAG, "三页对话小卡已就绪（天气页沿用右下角语音卡）");
}

// 需已持锁
void CustomLcdDisplay::RefreshPageChatWindowsInternal() {
    const int total = static_cast<int>(chat_turns_.size());

    PageChatWindow* wins[2] = {&memo_chat_, &music_chat_};
    for (PageChatWindow* w : wins) {
        if (w->card == nullptr) {
            continue;
        }
        // 小卡永远显示"最近两轮"：索引越大越靠下，读起来像聊天记录
        for (int i = 0; i < 2; i++) {
            const int idx = total - 2 + i;
            const bool has = (idx >= 0 && idx < total);
            const std::string user = has ? ChatTail(chat_turns_[idx].user, w->max_user_chars) : "";
            const std::string ai = has ? ChatTail(chat_turns_[idx].ai, w->max_ai_chars) : "";

            if (w->user_row[i] != nullptr) {
                if (user.empty()) {
                    lv_obj_add_flag(w->user_row[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(w->user_row[i], LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(w->user_label[i], user.c_str());
                }
            }
            if (w->ai_label[i] != nullptr) {
                lv_label_set_text(w->ai_label[i], ai.c_str());
            }
        }

        // 一条都没有时给个引导，别让白纸片空着
        if (total == 0 && w->ai_label[0] != nullptr) {
            lv_label_set_text(w->ai_label[0], "说一句话，这里会显示你说过的和我的回答");
        }
    }
}

// 需已持锁
void CustomLcdDisplay::SetPageChatWindowsVisibleInternal(bool show) {
    if (page_chat_visible_ == show) {
        return;
    }
    PageChatWindow* wins[2] = {&memo_chat_, &music_chat_};
    for (PageChatWindow* w : wins) {
        if (w->card == nullptr) {
            continue;
        }
        if (show) {
            lv_obj_clear_flag(w->card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(w->card);   // 保证压在本页内容之上
        } else {
            lv_obj_add_flag(w->card, LV_OBJ_FLAG_HIDDEN);
        }
    }
    page_chat_visible_ = show;
    ESP_LOGI(TAG, "对话小卡%s", show ? "出现" : "收回");
}

void CustomLcdDisplay::UpdatePageChatWindows(int device_state) {
    DisplayLockGuard lock(this);

    const DeviceState ds = static_cast<DeviceState>(device_state);
    const bool voice_state = (ds == kDeviceStateConnecting ||
                              ds == kDeviceStateListening ||
                              ds == kDeviceStateSpeaking);

    // 工具刚把页面切走（放歌 / 翻目录 / 换图）：这一轮先把位置让给结果页，
    // 下次用户开口时 AppendChatTurnInternal("user") 会把标记清掉。
    const bool show = voice_state && !tool_jumped_this_turn_;

    // 状态点亮时补一次内容；平时消息一到就刷新，这里不用重复写
    if (show && !page_chat_visible_) {
        RefreshPageChatWindowsInternal();
    }
    SetPageChatWindowsVisibleInternal(show);

    // 徽标跟着设备状态走
    if (show) {
        const char* badge = "聆听中";
        if (ds == kDeviceStateConnecting) {
            badge = "连接中";
        } else if (ds == kDeviceStateSpeaking) {
            badge = "回答中";
        }
        PageChatWindow* wins[2] = {&memo_chat_, &music_chat_};
        for (PageChatWindow* w : wins) {
            if (w->badge_label != nullptr) {
                lv_label_set_text(w->badge_label, badge);
            }
        }
    }
}

// ============================================================
// ② 放大的对话记录层（全屏，只在用户要求时出现）
// ============================================================

void CustomLcdDisplay::SetupChatOverlay() {
    DisplayLockGuard lock(this);
    using namespace chat_layout;

    lv_obj_t* root = lv_screen_active();

    // 全屏黑底：直接把下面的页面盖住。1-bit 屏没有半透明，只能整块覆盖。
    chat_overlay_ = lv_obj_create(root);
    lv_obj_set_size(chat_overlay_, kScreenW, kScreenH);
    lv_obj_set_pos(chat_overlay_, 0, 0);
    lv_obj_set_style_bg_color(chat_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(chat_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chat_overlay_, 0, 0);
    lv_obj_set_style_radius(chat_overlay_, 0, 0);
    lv_obj_set_style_pad_all(chat_overlay_, 0, 0);
    lv_obj_remove_flag(chat_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chat_overlay_, LV_OBJ_FLAG_HIDDEN);

    const lv_font_t* font = &font_puhui_16_4;

    // ===== 顶栏白胶囊（和另外两页同一条语言）=====
    lv_obj_t* head = lv_obj_create(chat_overlay_);
    lv_obj_set_pos(head, kHeadX, kHeadY);
    lv_obj_set_size(head, kHeadW, kHeadH);
    lv_obj_set_style_bg_color(head, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_radius(head, kHeadH / 2, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* dot = lv_obj_create(head);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    chat_overlay_badge_ = lv_label_create(head);
    lv_obj_set_style_text_font(chat_overlay_badge_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(chat_overlay_badge_, lv_color_black(), 0);
    lv_label_set_text(chat_overlay_badge_, "聆听中");
    lv_obj_align(chat_overlay_badge_, LV_ALIGN_LEFT_MID, 22, 0);

    lv_obj_t* title = lv_label_create(head);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_label_set_text(title, "对 话 记 录");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    chat_overlay_time_ = lv_label_create(head);
    lv_obj_set_style_text_font(chat_overlay_time_, &font_puhui_14_1, 0);
    lv_obj_set_style_text_color(chat_overlay_time_, lv_color_black(), 0);
    lv_label_set_text(chat_overlay_time_, "--:--");
    lv_obj_align(chat_overlay_time_, LV_ALIGN_RIGHT_MID, -12, 0);

    // ===== 白卡：最近三轮 =====
    lv_obj_t* card = lv_obj_create(chat_overlay_);
    lv_obj_set_pos(card, kCardX, kCardY);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < kTurnCount; i++) {
        const int y = 6 + i * kGroupH;

        // 用户：反白块（黑底白字）
        lv_obj_t* user_row = lv_obj_create(card);
        lv_obj_set_pos(user_row, 6, y);
        lv_obj_set_size(user_row, kCardW - 12, kUserH);
        lv_obj_set_style_bg_color(user_row, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(user_row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(user_row, 0, 0);
        lv_obj_set_style_radius(user_row, 6, 0);
        lv_obj_set_style_pad_left(user_row, 8, 0);
        lv_obj_set_style_pad_right(user_row, 8, 0);
        lv_obj_remove_flag(user_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(user_row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* user_label = lv_label_create(user_row);
        lv_obj_set_style_text_font(user_label, font, 0);
        lv_obj_set_style_text_color(user_label, lv_color_white(), 0);
        lv_obj_set_width(user_label, kCardW - 28);
        lv_label_set_long_mode(user_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(user_label, "");
        lv_obj_align(user_label, LV_ALIGN_LEFT_MID, 0, 0);
        chat_user_row_[i] = user_label;

        // AI：黑字，最多两行
        lv_obj_t* ai_label = lv_label_create(card);
        lv_obj_set_pos(ai_label, 12, y + kUserH + 4);
        lv_obj_set_size(ai_label, kCardW - 24, kAiH);
        lv_obj_set_style_text_font(ai_label, font, 0);
        lv_obj_set_style_text_color(ai_label, lv_color_black(), 0);
        lv_obj_set_style_text_align(ai_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(ai_label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(ai_label, "");
        chat_ai_row_[i] = ai_label;

        // 每轮之间一条细分隔线（最后一轮不画）
        if (i < kTurnCount - 1) {
            lv_obj_t* sep = lv_obj_create(card);
            lv_obj_set_pos(sep, 12, y + kGroupH - 6);
            lv_obj_set_size(sep, kCardW - 24, 1);
            lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_30, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
            lv_obj_set_style_radius(sep, 0, 0);
            lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    RefreshChatOverlayInternal();

    ESP_LOGI(TAG, "放大的对话记录层已创建（默认隐藏，等用户开口）");
}

// 需已持锁
void CustomLcdDisplay::AppendChatTurnInternal(const char* role, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const bool is_user = (strcmp(role, "user") == 0);

    if (is_user) {
        // 新一轮开口 → 这一轮的"工具跳页"标记清零，小卡可以重新出现
        tool_jumped_this_turn_ = false;
        ChatTurn turn;
        turn.user = text;
        chat_turns_.push_back(turn);
        while (chat_turns_.size() > 3) {
            chat_turns_.erase(chat_turns_.begin());
        }
    } else {
        if (chat_turns_.empty()) {
            ChatTurn turn;
            turn.ai = text;
            chat_turns_.push_back(turn);
        } else {
            // 同一条回复可能分多次发过来（sentence_start 逐句），拼起来
            ChatTurn& last = chat_turns_.back();
            if (last.ai.empty()) {
                last.ai = text;
            } else if (last.ai != text) {
                last.ai += text;
            }
        }
    }

    // 两层一起更新：三页小卡是常驻入口，全屏层只是放大视图
    RefreshPageChatWindowsInternal();
    RefreshChatOverlayInternal();

    // 只有"用户明确要求放大"时才让全屏层自己冒出来。
    // 平时各页用自己的小卡，页面内容照旧可见。
    const bool blocked_by_tool_jump = (!is_user && tool_jumped_this_turn_);
    if (chat_overlay_pinned_ && !blocked_by_tool_jump) {
        SetChatOverlayVisibleInternal(true);
    }
}

// 需已持锁
void CustomLcdDisplay::RefreshChatOverlayInternal() {
    if (chat_overlay_ == nullptr) {
        return;
    }

    const int total = static_cast<int>(chat_turns_.size());
    const int first = (total > chat_layout::kTurnCount) ? total - chat_layout::kTurnCount : 0;

    for (int i = 0; i < chat_layout::kTurnCount; i++) {
        const int idx = first + i;
        const bool has = (idx < total);
        const std::string user = has ? ChatTail(chat_turns_[idx].user, 22) : "";
        const std::string ai = has ? ChatTail(chat_turns_[idx].ai, 52) : "";

        if (chat_user_row_[i] != nullptr) {
            if (user.empty()) {
                lv_obj_add_flag(lv_obj_get_parent(chat_user_row_[i]), LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(lv_obj_get_parent(chat_user_row_[i]), LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(chat_user_row_[i], user.c_str());
            }
        }
        if (chat_ai_row_[i] != nullptr) {
            lv_label_set_text(chat_ai_row_[i], ai.c_str());
        }
    }

    if (total == 0 && chat_ai_row_[0] != nullptr) {
        lv_label_set_text(chat_ai_row_[0], "说一句话，这里会显示你说过的和我的回答");
    }
}

// 需已持锁
void CustomLcdDisplay::SetChatOverlayVisibleInternal(bool show) {
    if (chat_overlay_ == nullptr) {
        return;
    }
    const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (show) {
        if (!chat_overlay_visible_) {
            lv_obj_clear_flag(chat_overlay_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(chat_overlay_);   // 保证盖在三页之上
            chat_overlay_visible_ = true;
            ESP_LOGI(TAG, "对话记录展开（放大）");
        }
        // 说话期间不断续期，停手后 12 秒自动收回
        chat_hide_at_ms_ = now_ms + 12000;
    } else {
        if (chat_overlay_visible_) {
            lv_obj_add_flag(chat_overlay_, LV_OBJ_FLAG_HIDDEN);
            chat_overlay_visible_ = false;
            chat_hide_at_ms_ = 0;
            ESP_LOGI(TAG, "对话记录收起");
        }
    }
}

void CustomLcdDisplay::SetChatOverlayVisible(bool show) {
    DisplayLockGuard lock(this);
    chat_overlay_pinned_ = show;
    if (show) {
        // 刚手动打开的，先别拿"上一轮的结束标记"把它立刻关掉
        close_overlay_on_idle_ = false;
        // 用户明确要看：清掉"工具跳页"标记，且把最新内容补进去
        tool_jumped_this_turn_ = false;
        RefreshChatOverlayInternal();
    }
    SetChatOverlayVisibleInternal(show);
}
