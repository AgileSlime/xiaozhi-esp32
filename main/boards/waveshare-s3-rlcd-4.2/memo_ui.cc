// 备忘录页（400×300 单色 RLCD）——本质就是"闹钟目录"
//
// 风格跟主页/播放器页保持一致：黑底、白色圆角卡、正文 16px、次级 14px。
//   ┌──────────────────────────────────────────────┐
//   │ ╭──────────────────────────────────────────╮ │  白色胶囊头：时间 / 标题 / 电量
//   │ │ 18:07 ▂▃   备忘录           86% ▮▮      │ │
//   │ ╰──────────────────────────────────────────╯ │
//   │ ┌──────────────────────────────────────────┐ │
//   │ │▛▛ 和设计组过一遍 UI 稿        ▛12:07▛▛│ │  首条整车反白 = 下一条要响的
//   │ │  回复微雪的技术邮件            15:40    │ │  时间是 24px 大号数字
//   │ │  给 RLCD 板子刷一版固件        18:20    │ │
//   │ │  每天 整理本周的排期           21:00    │ │
//   │ └──────────────────────────────────────────┘ │
//   │ 共 4 项待办                          01│03   │  页脚
//   └──────────────────────────────────────────────┘
//
// 数据源：ReminderManager::List()。它天然只含"还没提醒过"的条目——
//   - 单次提醒触发后直接被 erase（ReminderManager::FireDue）
//   - 每日重复的触发后滚到下一个周期，仍留在列表里
// 所以这里不需要再过滤，也不会出现"响过的闹钟还挂在目录里"。
//
// 为什么不用设计稿里的浅灰底 + 细表格线：
//   这块屏只有黑与白。浅灰只能靠抖动，抖动会吃掉 16px 汉字的笔画；
//   细表格线在 1-bit 上就是实黑线，会把列表切成"表格"而不是"卡片"。
//   所以层级改用"整块反白 + 黑底白卡"来表达——这也是主页提醒卡的语言。

#include "custom_lcd_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "managers/reminder_manager.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_1);
// 20px 1bpp 子集（GB2312 一级 3755 字 + ASCII + 标点），缺字自动 fallback 到 16px 全量
LV_FONT_DECLARE(font_puhui_20_1);
LV_FONT_DECLARE(alibaba_puhui_24);   // 行内时间用大号数字，和主页 48px 时钟同族

LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "MemoUI";

// ===== 布局常量（改版式只需要动这里）=====
namespace memo_layout {
constexpr int kScreenW = 400;
constexpr int kScreenH = 300;

// 顶栏：一条白色胶囊，把时间 / 标题 / 电量都装进去
constexpr int kHeadX = 12;
constexpr int kHeadY = 4;
constexpr int kHeadW = 376;
constexpr int kHeadH = 30;   // 20px 标题在 26px 里只剩 3px 呼吸，加高到 30
constexpr int kHeadRadius = 13;
constexpr int kBatScale = 213;       // 24px 图标 → 20px

// 列表卡
constexpr int kCardX = 12;
constexpr int kCardY = 40;
constexpr int kCardW = 376;
constexpr int kCardH = 214;
constexpr int kCardRadius = 12;
constexpr int kRowH = 53;
constexpr int kRowCount = 4;
constexpr int kRowInset = 4;      // 行条相对卡片内缩，反白时仍能看见白卡轮廓
constexpr int kRowRadius = 8;
constexpr int kRowPadL = 16;
constexpr int kRowPadR = 16;
constexpr int kTimeW = 72;
constexpr int kRowW = kCardW - kRowInset * 2;          // 368
constexpr int kRowInnerH = kRowH - kRowInset * 2;      // 45
constexpr int kTextW = 264;   // 行条内正文宽度（右侧留给 24px 时间，留 8px 间隙）

// 页脚
constexpr int kFootY = 262;
constexpr int kFootH = 22;
constexpr int kPageCellW = 34;

constexpr int kRowsPerPage = kRowCount;
}  // namespace memo_layout

using namespace memo_layout;

namespace {

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

void CustomLcdDisplay::SetupMemoUI() {
    DisplayLockGuard lock(this);

    const lv_font_t *font_cn = &font_puhui_20_1;   // 正文：20px 1bpp，单色屏上比 16px 4bpp 更锐
    const lv_font_t *font_sm = &font_puhui_14_1;   // 顶栏时间 / 页脚

    lv_obj_t *root = lv_screen_active();

    // ===== 页面容器（黑底，和主页同一块底色）=====
    memo_page_ = lv_obj_create(root);
    lv_obj_set_size(memo_page_, kScreenW, kScreenH);
    lv_obj_set_pos(memo_page_, 0, 0);
    lv_obj_set_style_bg_color(memo_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(memo_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(memo_page_, 0, 0);
    lv_obj_set_style_radius(memo_page_, 0, 0);
    lv_obj_set_style_pad_all(memo_page_, 0, 0);
    lv_obj_remove_flag(memo_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(memo_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *page = memo_page_;

    // ================= 顶栏：白色胶囊 =================
    lv_obj_t *head = lv_obj_create(page);
    lv_obj_set_pos(head, kHeadX, kHeadY);
    lv_obj_set_size(head, kHeadW, kHeadH);
    StylePlainBox(head, lv_color_white(), LV_OPA_COVER, kHeadRadius);

    memo_time_label_ = lv_label_create(head);
    lv_obj_set_style_text_font(memo_time_label_, font_sm, 0);
    lv_obj_set_style_text_color(memo_time_label_, lv_color_black(), 0);
    lv_label_set_text(memo_time_label_, "--:--");
    lv_obj_align(memo_time_label_, LV_ALIGN_LEFT_MID, 11, 0);

    // 信号条（3 根递减短线，和播放器页同一套画法）
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(head);
        lv_obj_set_pos(bar, 52, 10 + i * 4);
        lv_obj_set_size(bar, 13 - i * 4, 2);
        StylePlainBox(bar, lv_color_black(), LV_OPA_COVER, 0);
    }

    lv_obj_t *title = lv_label_create(head);
    lv_obj_set_style_text_font(title, font_cn, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_label_set_text(title, "备忘录");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    memo_battery_pct_label_ = lv_label_create(head);
    lv_obj_set_style_text_font(memo_battery_pct_label_, font_sm, 0);
    lv_obj_set_style_text_color(memo_battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(memo_battery_pct_label_, "---%");
    lv_obj_align(memo_battery_pct_label_, LV_ALIGN_RIGHT_MID, -34, 0);

    memo_battery_icon_img_ = lv_image_create(head);
    lv_image_set_src(memo_battery_icon_img_, &ui_img_battery_full);
    lv_image_set_scale(memo_battery_icon_img_, kBatScale);
    lv_obj_align(memo_battery_icon_img_, LV_ALIGN_RIGHT_MID, -10, 0);

    // ================= 列表卡（白卡，四行）=================
    memo_page_card_ = lv_obj_create(page);
    lv_obj_set_pos(memo_page_card_, kCardX, kCardY);
    lv_obj_set_size(memo_page_card_, kCardW, kCardH);
    lv_obj_set_style_bg_color(memo_page_card_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(memo_page_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(memo_page_card_, 0, 0);
    lv_obj_set_style_radius(memo_page_card_, kCardRadius, 0);
    lv_obj_set_style_pad_all(memo_page_card_, 0, 0);
    lv_obj_set_style_clip_corner(memo_page_card_, true, 0);
    lv_obj_remove_flag(memo_page_card_, LV_OBJ_FLAG_SCROLLABLE);

    // 行分隔线：只在"下面还有一条"的地方显示（刷新时决定显隐）
    for (int i = 0; i < kRowCount - 1; i++) {
        memo_page_sep_[i] = lv_obj_create(memo_page_card_);
        lv_obj_set_pos(memo_page_sep_[i], kRowPadL, (i + 1) * kRowH - 1);
        lv_obj_set_size(memo_page_sep_[i], kCardW - kRowPadL - kRowPadR, 1);
        StylePlainBox(memo_page_sep_[i], lv_color_black(), LV_OPA_COVER, 0);
        lv_obj_add_flag(memo_page_sep_[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < kRowCount; i++) {
        // 整行容器：反白时把它的底色刷黑，文字跟着反色
        memo_page_row_[i] = lv_obj_create(memo_page_card_);
        lv_obj_set_pos(memo_page_row_[i], kRowInset, i * kRowH + kRowInset);
        lv_obj_set_size(memo_page_row_[i], kRowW, kRowInnerH);
        StylePlainBox(memo_page_row_[i], lv_color_black(), LV_OPA_TRANSP, kRowRadius);

        memo_page_row_text_[i] = lv_label_create(memo_page_row_[i]);
        lv_obj_set_style_text_font(memo_page_row_text_[i], font_cn, 0);
        lv_obj_set_style_text_color(memo_page_row_text_[i], lv_color_black(), 0);
        lv_obj_set_pos(memo_page_row_text_[i], kRowPadL - kRowInset, (kRowInnerH - 21) / 2);
        lv_obj_set_width(memo_page_row_text_[i], kTextW);
        lv_label_set_long_mode(memo_page_row_text_[i], LV_LABEL_LONG_DOT);
        lv_label_set_text(memo_page_row_text_[i], "");

        // 时间用 24px 大号数字：和 20px 正文拉开一个层级，
        // 一屏里总得有个立得住的视觉锚点（主页靠 48px 时钟）
        memo_page_row_time_[i] = lv_label_create(memo_page_row_[i]);
        lv_obj_set_style_text_font(memo_page_row_time_[i], &alibaba_puhui_24, 0);
        lv_obj_set_style_text_color(memo_page_row_time_[i], lv_color_black(), 0);
        lv_obj_set_width(memo_page_row_time_[i], kTimeW);
        lv_obj_set_style_text_align(memo_page_row_time_[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(memo_page_row_time_[i], kRowW - kRowPadR + kRowInset - kTimeW, (kRowInnerH - 31) / 2);
        lv_label_set_text(memo_page_row_time_[i], "");
    }

    // 空列表提示（居中，两行）
    memo_page_empty_ = lv_label_create(memo_page_card_);
    lv_obj_set_style_text_font(memo_page_empty_, font_cn, 0);
    lv_obj_set_style_text_color(memo_page_empty_, lv_color_black(), 0);
    lv_obj_set_style_text_align(memo_page_empty_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(memo_page_empty_, 8, 0);
    lv_obj_set_width(memo_page_empty_, kCardW - 60);
    lv_label_set_long_mode(memo_page_empty_, LV_LABEL_LONG_WRAP);
    lv_obj_center(memo_page_empty_);
    lv_label_set_text(memo_page_empty_, "还没有闹钟\n说「七点叫我起床」试试");
    lv_obj_add_flag(memo_page_empty_, LV_OBJ_FLAG_HIDDEN);

    // ================= 页脚（黑底白字）=================
    memo_page_count_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(memo_page_count_label_, font_sm, 0);
    lv_obj_set_style_text_color(memo_page_count_label_, lv_color_white(), 0);
    lv_label_set_text(memo_page_count_label_, "共 0 项待办");
    lv_obj_set_pos(memo_page_count_label_, kCardX, kFootY + 2);

    memo_pager_box_ = lv_obj_create(page);
    lv_obj_set_pos(memo_pager_box_, kScreenW - kCardX - kPageCellW * 2, kFootY);
    lv_obj_set_size(memo_pager_box_, kPageCellW * 2, kFootH);
    lv_obj_set_style_bg_color(memo_pager_box_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(memo_pager_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(memo_pager_box_, 1, 0);
    lv_obj_set_style_border_color(memo_pager_box_, lv_color_white(), 0);
    lv_obj_set_style_radius(memo_pager_box_, 3, 0);
    lv_obj_set_style_pad_all(memo_pager_box_, 0, 0);
    lv_obj_set_style_clip_corner(memo_pager_box_, true, 0);
    lv_obj_remove_flag(memo_pager_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(memo_pager_box_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *cur_cell = lv_obj_create(memo_pager_box_);
    lv_obj_set_pos(cur_cell, 0, 0);
    lv_obj_set_size(cur_cell, kPageCellW - 1, kFootH - 2);
    StylePlainBox(cur_cell, lv_color_white(), LV_OPA_COVER, 0);

    memo_pager_cur_label_ = lv_label_create(cur_cell);
    lv_obj_set_style_text_font(memo_pager_cur_label_, font_sm, 0);
    lv_obj_set_style_text_color(memo_pager_cur_label_, lv_color_black(), 0);
    lv_label_set_text(memo_pager_cur_label_, "01");
    lv_obj_center(memo_pager_cur_label_);

    lv_obj_t *tot_cell = lv_obj_create(memo_pager_box_);
    lv_obj_set_pos(tot_cell, kPageCellW - 1, 0);
    lv_obj_set_size(tot_cell, kPageCellW - 1, kFootH - 2);
    StylePlainBox(tot_cell, lv_color_black(), LV_OPA_COVER, 0);

    memo_pager_tot_label_ = lv_label_create(tot_cell);
    lv_obj_set_style_text_font(memo_pager_tot_label_, font_sm, 0);
    lv_obj_set_style_text_color(memo_pager_tot_label_, lv_color_white(), 0);
    lv_label_set_text(memo_pager_tot_label_, "01");
    lv_obj_center(memo_pager_tot_label_);

    RefreshMemoPageInternal();

    ESP_LOGI(TAG, "备忘录页创建完成（黑底白卡，4 行/页，首条反白）");
}

// ============================================================
// 数据刷新
// ============================================================

void CustomLcdDisplay::RefreshMemoPageInternal() {
    if (memo_page_card_ == nullptr) {
        return;
    }

    // 只拿"还没响过"的条目：单次提醒响完即被删除，每日重复的会滚到下一次
    auto items = ReminderManager::GetInstance().List();

    // due 升序（due==0 表示只记事不响铃，排最后）
    std::stable_sort(items.begin(), items.end(), [](const ReminderItem& a, const ReminderItem& b) {
        if (a.due == 0 && b.due == 0) return false;
        if (a.due == 0) return false;
        if (b.due == 0) return true;
        return a.due < b.due;
    });

    memo_page_items_ = static_cast<int>(items.size());
    memo_page_total_ = (memo_page_items_ + kRowsPerPage - 1) / kRowsPerPage;
    if (memo_page_total_ < 1) {
        memo_page_total_ = 1;
    }
    if (memo_page_idx_ >= memo_page_total_) {
        memo_page_idx_ = memo_page_total_ - 1;
    }
    if (memo_page_idx_ < 0) {
        memo_page_idx_ = 0;
    }

    const int first = memo_page_idx_ * kRowsPerPage;
    int visible = memo_page_items_ - first;
    if (visible < 0) visible = 0;
    if (visible > kRowCount) visible = kRowCount;

    // 卡片高度跟着实际条目走：只有 2 条时不留一大块白，
    // 空列表则用整块高度当提示面板。
    // 卡片高 = 行数 * 行高：行条顶距卡顶 4px，末条底距卡底也是 4px，上下才对称
    int card_h = (visible > 0) ? (visible * kRowH) : kCardH;
    if (card_h > kCardH) card_h = kCardH;
    lv_obj_set_height(memo_page_card_, card_h);

    // 页脚跟着卡片走：只有 2 条时别把"共 N 项待办"钉在屏幕最底下、
    // 中间空出一大块。4 条时算出来正好是 kFootY，等于没动。
    int foot_y = kCardY + card_h + 10;
    if (foot_y > kFootY) foot_y = kFootY;
    lv_obj_set_pos(memo_page_count_label_, kCardX, foot_y + 2);
    lv_obj_set_pos(memo_pager_box_, kScreenW - kCardX - kPageCellW * 2, foot_y);

    for (int i = 0; i < kRowCount; i++) {
        const int idx = first + i;
        const bool has_item = (idx < memo_page_items_);

        if (!has_item) {
            lv_obj_add_flag(memo_page_row_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const ReminderItem& item = items[idx];

        // 正文：每天重复的加前缀，避免"看起来只响一次"
        char text_buf[96];
        if (item.daily) {
            snprintf(text_buf, sizeof(text_buf), "每天 %s", item.content.c_str());
        } else {
            snprintf(text_buf, sizeof(text_buf), "%s", item.content.c_str());
        }
        lv_label_set_text(memo_page_row_text_[i], text_buf);
        lv_label_set_text(memo_page_row_time_[i], item.time_label.c_str());
        lv_obj_clear_flag(memo_page_row_[i], LV_OBJ_FLAG_HIDDEN);

        // 强调：只有"第一页第一行"是下一条要响的，整行反白（和主页提醒卡一个语言）
        const bool highlight = (i == 0 && memo_page_idx_ == 0);
        if (highlight) {
            lv_obj_set_style_bg_opa(memo_page_row_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(memo_page_row_text_[i], lv_color_white(), 0);
            lv_obj_set_style_text_color(memo_page_row_time_[i], lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_opa(memo_page_row_[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(memo_page_row_text_[i], lv_color_black(), 0);
            lv_obj_set_style_text_color(memo_page_row_time_[i], lv_color_black(), 0);
        }
    }

    // 分隔线：只在"下面还有一条"的地方画，避免空行/空列表被线切碎
    for (int i = 0; i < kRowCount - 1; i++) {
        const bool need_rule = (first + i + 1) < memo_page_items_;
        if (need_rule) {
            lv_obj_clear_flag(memo_page_sep_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(memo_page_sep_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 空列表提示
    if (memo_page_items_ == 0) {
        lv_obj_clear_flag(memo_page_empty_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(memo_page_empty_, LV_OBJ_FLAG_HIDDEN);
    }

    // 页脚统计
    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "共 %d 项待办", memo_page_items_);
    lv_label_set_text(memo_page_count_label_, count_buf);

    // 页码（只有一页时整块隐藏，避免"01/01"这种噪音）
    if (memo_page_total_ > 1) {
        char cur_buf[12];
        char tot_buf[12];
        snprintf(cur_buf, sizeof(cur_buf), "%02d", memo_page_idx_ + 1);
        snprintf(tot_buf, sizeof(tot_buf), "%02d", memo_page_total_);
        lv_label_set_text(memo_pager_cur_label_, cur_buf);
        lv_label_set_text(memo_pager_tot_label_, tot_buf);
        lv_obj_clear_flag(memo_pager_box_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(memo_pager_box_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::RefreshMemoPage() {
    DisplayLockGuard lock(this);
    RefreshMemoPageInternal();
}

void CustomLcdDisplay::SwitchToMemoPage() {
    DisplayLockGuard lock(this);

    // "打开备忘录/翻页"是要看结果的动作：收起对话小卡和放大层，把目录页让出来
    tool_jumped_this_turn_ = true;
    chat_overlay_pinned_ = false;
    SetChatOverlayVisibleInternal(false);
    SetPageChatWindowsVisibleInternal(false);

    // 先刷新再显示：避免"页面切过来先闪一下旧数据"
    RefreshMemoPageInternal();

    if (display_mode_ != MODE_MEMO) {
        display_mode_ = MODE_MEMO;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "切换到备忘录页（%d 条 / %d 页）", memo_page_items_, memo_page_total_);
    }
}

bool CustomLcdDisplay::NextMemoPage() {
    DisplayLockGuard lock(this);
    RefreshMemoPageInternal();
    if (memo_page_total_ <= 1) {
        return false;
    }
    memo_page_idx_ = (memo_page_idx_ + 1) % memo_page_total_;
    RefreshMemoPageInternal();
    ESP_LOGI(TAG, "备忘录翻到第 %d/%d 页", memo_page_idx_ + 1, memo_page_total_);
    return true;
}

bool CustomLcdDisplay::PrevMemoPage() {
    DisplayLockGuard lock(this);
    RefreshMemoPageInternal();
    if (memo_page_total_ <= 1) {
        return false;
    }
    memo_page_idx_ = (memo_page_idx_ + memo_page_total_ - 1) % memo_page_total_;
    RefreshMemoPageInternal();
    ESP_LOGI(TAG, "备忘录翻到第 %d/%d 页", memo_page_idx_ + 1, memo_page_total_);
    return true;
}
