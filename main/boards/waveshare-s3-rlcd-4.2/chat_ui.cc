// 对话浮层（400×300 单色 RLCD）
//
// 为什么做成"浮层"而不是把对话列表塞进每个页面：
//   天气页左边全是照片、右列挤着时间卡和提醒卡；备忘录页是 4 行列表；播放器页是
//   唱片 + 歌词 + 进度条。三套版式都没有第二块地方放对话记录。
//   而"说话的时候看不到自己说了什么、AI 答了什么"是最影响使用的问题。
//
// 做法：一层和屏幕等大的黑色底 + 顶栏胶囊 + 大白卡，创建在三页之后（z 序最高），
// 平时隐藏。于是它天然"覆盖在任意页面之上"——不管当前停在哪个页面，说话时都能看到
// 最近三轮对话。
//
// 什么时候出现：
//   1. 进入聆听 / 连接中        → 自动浮上来
//   2. AI 开始回答              → 除非这一轮有工具跳页，否则继续显示
//   3. 工具执行（放歌/翻目录）  → 立刻收起，把结果页让出来
//   4. 空闲 12 秒               → 自动收起，回到原来的页面
//   5. 说"看看聊天记录"        → 随时手动调出（self.chat.history）
//
// 版式：
//   ┌────────────────────────────────────────────┐
//   │ ●聆听中          对 话              20:33  │  白胶囊 376×30
//   ├────────────────────────────────────────────┤
//   │  ┌──────────────────────────────────────┐  │
//   │  │ 放首晴天                             │  │ 用户（反白块）
//   │  │ 好的，正在播放《晴天》— 周杰伦        │  │ AI（黑字，最多 2 行）
//   │  │ ...                                   │  │
//   │  └──────────────────────────────────────┘  │  白卡 376×248
//   └────────────────────────────────────────────┘

#include "custom_lcd_display.h"

#include <cstdio>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

LV_FONT_DECLARE(font_puhui_16_4);

static const char* TAG = "ChatOverlay";

namespace chat_layout {
constexpr int kScreenW = 400;
constexpr int kScreenH = 300;

constexpr int kHeadX = 12;
constexpr int kHeadY = 4;
constexpr int kHeadW = 376;
constexpr int kHeadH = 30;

constexpr int kCardX = 12;
constexpr int kCardY = 40;
constexpr int kCardW = 376;
constexpr int kCardH = 248;

constexpr int kGroupH = 80;      // 一轮占多高（用户行 + AI 两行）
constexpr int kUserH = 26;
constexpr int kAiH = 40;
constexpr int kTurnCount = 3;    // 屏上最多显示三轮
}  // namespace chat_layout

// UTF-8 安全截断（保留尾部，前面加省略号）。浮层空间固定，长句子必须截。
static std::string ChatTail(const std::string& text, size_t max_chars) {
    if (text.empty()) {
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

void CustomLcdDisplay::SetupChatOverlay() {
    DisplayLockGuard lock(this);
    using namespace chat_layout;

    lv_obj_t* root = lv_screen_active();

    // 全屏黑底：直接把下面的页面盖住。1-bit 屏没有半透明，所以只能用"整块覆盖"。
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
    lv_obj_set_style_text_font(chat_overlay_badge_, font, 0);
    lv_obj_set_style_text_color(chat_overlay_badge_, lv_color_black(), 0);
    lv_label_set_text(chat_overlay_badge_, "聆听中");
    lv_obj_align(chat_overlay_badge_, LV_ALIGN_LEFT_MID, 22, 0);

    lv_obj_t* title = lv_label_create(head);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_label_set_text(title, "对  话");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    chat_overlay_time_ = lv_label_create(head);
    lv_obj_set_style_text_font(chat_overlay_time_, font, 0);
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
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < kTurnCount; i++) {
        const int y = 6 + i * kGroupH;

        // 用户：反白块（黑底白字），和备忘录首行、歌词当前句同一套语言
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

    ESP_LOGI(TAG, "对话浮层已创建（覆盖三页，默认隐藏）");
}

// 需已持锁
void CustomLcdDisplay::AppendChatTurnInternal(const char* role, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const bool is_user = (strcmp(role, "user") == 0);

    if (is_user) {
        // 新一轮开口 → 这一轮的"工具跳页"标记清零
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

    RefreshChatOverlayInternal();

    // 工具已经把页面切走了就别再浮上来，否则用户看不到放歌/目录的结果页
    const bool blocked_by_tool_jump = (!is_user && tool_jumped_this_turn_);
    if (!blocked_by_tool_jump) {
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

    // 一条都没有时给个引导，总比整片空白好
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
            ESP_LOGI(TAG, "对话浮层显示");
        }
        // 说话/回答期间不断续期，停手后 12 秒自动收起
        chat_hide_at_ms_ = now_ms + 12000;
    } else {
        if (chat_overlay_visible_) {
            lv_obj_add_flag(chat_overlay_, LV_OBJ_FLAG_HIDDEN);
            chat_overlay_visible_ = false;
            chat_hide_at_ms_ = 0;
            ESP_LOGI(TAG, "对话浮层收起");
        }
    }
}

void CustomLcdDisplay::SetChatOverlayVisible(bool show) {
    DisplayLockGuard lock(this);
    if (show) {
        // 手动调出时把"工具跳页"标记清掉，否则下次回答会被它挡住
        tool_jumped_this_turn_ = false;
    }
    SetChatOverlayVisibleInternal(show);
}
