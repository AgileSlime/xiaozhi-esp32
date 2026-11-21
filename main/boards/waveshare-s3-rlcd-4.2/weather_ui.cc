// 主页 UI（400×300 单色 RLCD）
//
// 布局（与设计稿一致）：
//   ┌────────────────────────────────────────────┐
//   │ 23.5°C 48%              [wifi][bat] 86%    │  状态栏 34px
//   ├──────────────────────┬─────────────────────┤
//   │                      │  11:42              │  时间卡 158×102
//   │   图片 214×252        │  11月17日 周一        │
//   │   （Bayer 抖动）       ├─────────────────────┤
//   │                      │  提醒 / 语音识别      │  卡 158×142
//   └──────────────────────┴─────────────────────┘
//
// 右下角那张卡是双态的（同一块区域复用）：
//   空闲 → 最近 3 条要响的提醒，第一条反白高亮
//   语音 → "聆听中" 胶囊 + 用户识别文字（反白块）+ AI 回复
// 切换由 data_update_task 依据 DeviceState 调用 UpdateHomeBottomCard() 完成。

#include "custom_lcd_display.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "managers/photo_loader.h"

// 天气站专用字体
LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);
LV_FONT_DECLARE(alibaba_puhui_48);
LV_FONT_DECLARE(alibaba_black_64);

// 小智自带字体（7415 常用汉字）
LV_FONT_DECLARE(font_puhui_16_4);

// 状态栏图标
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "HomeUI";

// ===== 布局常量（改版式只需要动这里）=====
namespace home_layout {
constexpr int kScreenW = 400;
constexpr int kScreenH = 300;

constexpr int kMargin = 10;

// 状态栏
constexpr int kStatusY = 5;
constexpr int kStatusH = 26;
constexpr int kCapsuleW = 132;
constexpr int kCapsuleH = 26;

// 左图
constexpr int kPhotoX = kMargin;
constexpr int kPhotoY = 38;
constexpr int kPhotoW = 214;
constexpr int kPhotoH = 252;
constexpr int kPhotoBorder = 2;
constexpr int kPhotoInnerW = kPhotoW - kPhotoBorder * 2;   // 210
constexpr int kPhotoInnerH = kPhotoH - kPhotoBorder * 2;   // 248

// 右列
constexpr int kRightX = 232;
constexpr int kRightW = 158;

// 时间卡
constexpr int kTimeY = 38;
constexpr int kTimeH = 102;

// 底部卡（提醒 / 语音共用）
constexpr int kCardY = 148;
constexpr int kCardH = 142;
}  // namespace home_layout

void CustomLcdDisplay::SetupWeatherUI() {
    DisplayLockGuard lock(this);
    using namespace home_layout;

    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    weather_page_ = lv_obj_create(root);
    lv_obj_set_size(weather_page_, kScreenW, kScreenH);
    lv_obj_set_pos(weather_page_, 0, 0);
    lv_obj_set_style_bg_opa(weather_page_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_page_, 0, 0);
    lv_obj_set_style_pad_all(weather_page_, 0, 0);
    lv_obj_set_style_radius(weather_page_, 0, 0);
    lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *screen = weather_page_;

    const lv_font_t *font_cn    = &font_puhui_16_4;   // 所有中文（含生僻字）
    const lv_font_t *font_clock = &alibaba_puhui_48;  // 大号时间

    // ================= 状态栏 =================
    // 左上：温湿度（黑底白字，无卡片）
    sensor_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sensor_label_, font_cn, 0);
    lv_obj_set_style_text_color(sensor_label_, lv_color_white(), 0);
    lv_obj_align(sensor_label_, LV_ALIGN_TOP_LEFT, kMargin, kStatusY + 3);
    lv_label_set_text(sensor_label_, "--.-°C  --.-%");

    // 右上：白底胶囊（图标本身是白底黑图案，放黑底会变成白方块）
    lv_obj_t *status_bar = lv_obj_create(screen);
    lv_obj_set_size(status_bar, kCapsuleW, kCapsuleH);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_white(), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, kCapsuleH / 2, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_RIGHT, -8, 4);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_set_style_pad_left(status_bar, 8, 0);
    lv_obj_set_style_pad_right(status_bar, 8, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_bar, 4, 0);

    wifi_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(wifi_icon_img_, &ui_img_wifi_off);
    lv_image_set_scale(wifi_icon_img_, 213);   // 24px → 20px

    battery_icon_img_ = lv_image_create(status_bar);
    lv_image_set_src(battery_icon_img_, &ui_img_battery_full);
    lv_image_set_scale(battery_icon_img_, 213);

    battery_pct_label_ = lv_label_create(status_bar);
    lv_obj_set_style_text_font(battery_pct_label_, font_cn, 0);
    lv_obj_set_style_text_color(battery_pct_label_, lv_color_black(), 0);
    lv_label_set_text(battery_pct_label_, "---%");

    // ================= 左侧：自定义美图 =================
    lv_obj_t *photo_card = lv_obj_create(screen);
    lv_obj_set_pos(photo_card, kPhotoX, kPhotoY);
    lv_obj_set_size(photo_card, kPhotoW, kPhotoH);
    lv_obj_set_style_bg_color(photo_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(photo_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(photo_card, kPhotoBorder, 0);
    lv_obj_set_style_border_color(photo_card, lv_color_white(), 0);
    lv_obj_set_style_radius(photo_card, 0, 0);
    lv_obj_set_style_pad_all(photo_card, 0, 0);
    lv_obj_set_style_clip_corner(photo_card, true, 0);
    lv_obj_remove_flag(photo_card, LV_OBJ_FLAG_SCROLLABLE);

    photo_img_ = lv_image_create(photo_card);
    lv_obj_center(photo_img_);
    lv_obj_add_flag(photo_img_, LV_OBJ_FLAG_HIDDEN);   // 加载成功后再显示

    photo_hint_label_ = lv_label_create(photo_card);
    lv_obj_set_style_text_font(photo_hint_label_, font_cn, 0);
    lv_obj_set_style_text_color(photo_hint_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(photo_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(photo_hint_label_, kPhotoInnerW - 20);
    lv_label_set_long_mode(photo_hint_label_, LV_LABEL_LONG_WRAP);
    lv_obj_center(photo_hint_label_);
    lv_label_set_text(photo_hint_label_, "把 JPG 放进\n/sdcard/photos");
    // 默认隐藏：固件里编了内置默认壁纸，SD 卡没图时也轮不到这句提示。
    // 只有连默认图都展不出来（PSRAM 不够）才由 LoadHomePhoto 放出来，
    // 否则开机前 3 秒会先闪一下这行字再被图片盖住。
    lv_obj_add_flag(photo_hint_label_, LV_OBJ_FLAG_HIDDEN);

    // 首帧就铺上图片：优先 NVS 常驻图（已在构造前 LoadSavedOverride），
    // 没有 override 才用内置默认壁纸。
    // 这一句不碰 SD 卡（只展位图），所以不会把"未挂载 → 空列表"的状态写进
    // HomePhoto 的扫描缓存；3 秒后 data_update_task 再走正常流程，
    // 有卡有图就换成卡里的照片，没图就沿用这张。
    {
        auto& photo = HomePhoto::GetInstance();
        bool shown = false;
        if (photo.HasOverride()) {
            // 有常驻图：直接按 override 渲染，别先闪默认图再换
            shown = photo.Load(photo_img_, kPhotoInnerW, kPhotoInnerH, -1);
        }
        if (!shown) {
            shown = photo.LoadDefault(photo_img_, kPhotoInnerW, kPhotoInnerH);
        }
        if (shown) {
            lv_obj_clear_flag(photo_img_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ================= 右上：时间卡 =================
    lv_obj_t *time_card = lv_obj_create(screen);
    lv_obj_set_pos(time_card, kRightX, kTimeY);
    lv_obj_set_size(time_card, kRightW, kTimeH);
    lv_obj_set_style_bg_color(time_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(time_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(time_card, 0, 0);
    lv_obj_set_style_radius(time_card, 12, 0);
    lv_obj_set_style_pad_all(time_card, 0, 0);
    lv_obj_remove_flag(time_card, LV_OBJ_FLAG_SCROLLABLE);

    time_label_ = lv_label_create(time_card);
    lv_obj_set_style_text_font(time_label_, font_clock, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(time_label_, kRightW);
    lv_label_set_text(time_label_, "--:--");
    lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 2);

    date_label_ = lv_label_create(time_card);
    lv_obj_set_style_text_font(date_label_, font_cn, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(date_label_, kRightW);
    lv_label_set_text(date_label_, "--月--日 周-");
    lv_obj_align(date_label_, LV_ALIGN_BOTTOM_MID, 0, -9);

    // ================= 右下：提醒卡（空闲态）=================
    memo_card_ = lv_obj_create(screen);
    lv_obj_set_pos(memo_card_, kRightX, kCardY);
    lv_obj_set_size(memo_card_, kRightW, kCardH);
    lv_obj_set_style_bg_color(memo_card_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(memo_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(memo_card_, 0, 0);
    lv_obj_set_style_radius(memo_card_, 12, 0);
    lv_obj_set_style_pad_all(memo_card_, 0, 0);
    lv_obj_remove_flag(memo_card_, LV_OBJ_FLAG_SCROLLABLE);

    chat_card_ = memo_card_;   // 兼容旧代码里的引用

    lv_obj_t *memo_title = lv_label_create(memo_card_);
    lv_obj_set_style_text_font(memo_title, font_cn, 0);
    lv_obj_set_style_text_color(memo_title, lv_color_black(), 0);
    lv_label_set_text(memo_title, "提醒");
    lv_obj_align(memo_title, LV_ALIGN_TOP_LEFT, 8, 5);

    memo_count_label_ = lv_label_create(memo_card_);
    lv_obj_set_style_text_font(memo_count_label_, font_cn, 0);
    lv_obj_set_style_text_color(memo_count_label_, lv_color_black(), 0);
    lv_label_set_text(memo_count_label_, "");
    lv_obj_align(memo_count_label_, LV_ALIGN_TOP_RIGHT, -8, 5);

    lv_obj_t *memo_sep = lv_obj_create(memo_card_);
    lv_obj_set_size(memo_sep, kRightW - 16, 1);
    lv_obj_set_style_bg_color(memo_sep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(memo_sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(memo_sep, 0, 0);
    lv_obj_set_style_radius(memo_sep, 0, 0);
    lv_obj_align(memo_sep, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_remove_flag(memo_sep, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_label_create(memo_card_);
        lv_obj_set_pos(row, 5, 34 + i * 33);
        lv_obj_set_size(row, kRightW - 10, 30);
        lv_obj_set_style_text_font(row, font_cn, 0);
        lv_obj_set_style_text_color(row, lv_color_black(), 0);
        lv_obj_set_style_text_align(row, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_pad_left(row, 5, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        lv_label_set_text(row, "");
        memo_row_label_[i] = row;
    }

    // ================= 右下：语音识别卡（语音态，默认隐藏）=================
    voice_card_ = lv_obj_create(screen);
    lv_obj_set_pos(voice_card_, kRightX, kCardY);
    lv_obj_set_size(voice_card_, kRightW, kCardH);
    lv_obj_set_style_bg_color(voice_card_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(voice_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(voice_card_, 0, 0);
    lv_obj_set_style_radius(voice_card_, 12, 0);
    lv_obj_set_style_pad_all(voice_card_, 0, 0);
    lv_obj_remove_flag(voice_card_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(voice_card_, LV_OBJ_FLAG_HIDDEN);

    voice_badge_ = lv_obj_create(voice_card_);
    lv_obj_set_pos(voice_badge_, 8, 6);
    lv_obj_set_size(voice_badge_, 92, 24);
    lv_obj_set_style_bg_color(voice_badge_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(voice_badge_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(voice_badge_, 0, 0);
    lv_obj_set_style_radius(voice_badge_, 12, 0);
    lv_obj_set_style_pad_all(voice_badge_, 0, 0);
    lv_obj_remove_flag(voice_badge_, LV_OBJ_FLAG_SCROLLABLE);

    // 小圆点（静态，不做闪烁动画：1-bit 屏每次重绘都要整屏走 SPI）
    lv_obj_t *dot = lv_obj_create(voice_badge_);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    voice_badge_label_ = lv_label_create(voice_badge_);
    lv_obj_set_style_text_font(voice_badge_label_, font_cn, 0);
    lv_obj_set_style_text_color(voice_badge_label_, lv_color_white(), 0);
    lv_label_set_text(voice_badge_label_, "聆听中");
    lv_obj_align(voice_badge_label_, LV_ALIGN_LEFT_MID, 24, 0);

    voice_timer_label_ = lv_label_create(voice_card_);
    lv_obj_set_style_text_font(voice_timer_label_, font_cn, 0);
    lv_obj_set_style_text_color(voice_timer_label_, lv_color_black(), 0);
    lv_label_set_text(voice_timer_label_, "00:00");
    lv_obj_align(voice_timer_label_, LV_ALIGN_TOP_RIGHT, -8, 8);

    // 用户说话内容（反白块）
    voice_user_bubble_ = lv_obj_create(voice_card_);
    lv_obj_set_pos(voice_user_bubble_, 5, 32);
    lv_obj_set_size(voice_user_bubble_, kRightW - 10, 62);
    lv_obj_set_style_bg_color(voice_user_bubble_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(voice_user_bubble_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(voice_user_bubble_, 0, 0);
    lv_obj_set_style_radius(voice_user_bubble_, 8, 0);
    lv_obj_set_style_pad_all(voice_user_bubble_, 5, 0);
    lv_obj_remove_flag(voice_user_bubble_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(voice_user_bubble_, true, 0);

    voice_user_label_ = lv_label_create(voice_user_bubble_);
    lv_obj_set_style_text_font(voice_user_label_, font_cn, 0);
    lv_obj_set_style_text_color(voice_user_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(voice_user_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(voice_user_label_, kRightW - 20);
    lv_label_set_long_mode(voice_user_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(voice_user_label_, "");
    lv_obj_set_pos(voice_user_label_, 0, 0);

    // AI 回复
    voice_ai_label_ = lv_label_create(voice_card_);
    lv_obj_set_style_text_font(voice_ai_label_, font_cn, 0);
    lv_obj_set_style_text_color(voice_ai_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(voice_ai_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(voice_ai_label_, 8, 98);
    lv_obj_set_size(voice_ai_label_, kRightW - 16, 38);
    lv_label_set_long_mode(voice_ai_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(voice_ai_label_, "");

    // ================= 给基类留的隐藏占位控件 =================
    // SetChatMessage / SetEmotion / Alert 会写这些句柄，必须给合法对象，
    // 但主页上不再显示大表情区，所以一律隐藏。
    chat_status_label_ = lv_label_create(screen);
    lv_label_set_text(chat_status_label_, "");
    lv_obj_add_flag(chat_status_label_, LV_OBJ_FLAG_HIDDEN);
    chat_message_label_ = chat_status_label_;

    emotion_label_ = lv_label_create(screen);
    lv_label_set_text(emotion_label_, "");
    lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_img_ = lv_image_create(screen);
    lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, 1, 1);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    network_label_ = lv_label_create(screen);
    lv_label_set_text(network_label_, "");
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    battery_label_ = lv_label_create(screen);
    lv_label_set_text(battery_label_, "");
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(screen);
    lv_label_set_text(status_label_, "");
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    notification_label_ = lv_label_create(screen);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    mute_label_ = lv_label_create(screen);
    lv_label_set_text(mute_label_, "");
    lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, 320, 42);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 12, 0);
    lv_obj_set_style_pad_all(low_battery_popup_, 6, 0);
    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_obj_set_style_text_font(low_battery_label_, font_cn, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_black(), 0);
    lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(low_battery_label_, 300);
    lv_obj_center(low_battery_label_);
    lv_label_set_text(low_battery_label_, "电量低，请尽快充电");

    emoji_label_ = lv_label_create(screen);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    // 先把提醒列表画出来，避免"UI 造好 → 数据任务启动"这 3 秒里卡片是空的
    RefreshMemoDisplayInternal();

    ESP_LOGI(TAG, "主页 UI 创建完成（400x300，左图 214x252 / 时间卡 / 双态卡片）");
}
