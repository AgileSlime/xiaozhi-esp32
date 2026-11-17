// CustomLcdDisplay 核心类
//
// 负责：
// - 构造/析构（初始化 RLCD 驱动 + LVGL + 创建 UI）
// - LVGL flush 回调（RGB565 → 1-bit 转换）
// - AI 消息适配（重写小智的 SetChatMessage / SetEmotion / ClearChatMessages）
// - 备忘录功能（加载/刷新备忘录列表）
// - 基类方法重写（UpdateStatusBar / SetTheme）
//
// 其他功能拆分到独立文件：
//   rlcd_driver.cc        - RLCD 硬件驱动
//   weather_ui.cc          - 天气站 UI 布局
//   music_ui.cc            - 音乐页 UI 布局
//   data_update_task.cc    - 后台数据更新任务

#include <vector>
#include <string>
#include <cstring>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "esp_lvgl_port.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "application.h"
#include "managers/reminder_manager.h"
#include "managers/photo_loader.h"
#include "lvgl_theme.h"

static const char *TAG = "CustomDisplay";

// ===== LVGL flush 回调 =====

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *self = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    RlcdDriver *rlcd = self->rlcd_;
    uint16_t *buffer = (uint16_t *)color_p;
    for(int y = area->y1; y <= area->y2; y++)
    {
        for(int x = area->x1; x <= area->x2; x++) 
        {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            rlcd->RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    rlcd->RLCD_Display();
    lv_disp_flush_ready(disp);
}

// ===== 构造 / 析构 =====

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t panel,
    int width, int height, int offset_x, int offset_y,
    bool mirror_x, bool mirror_y, bool swap_xy,
    spi_display_config_t spiconfig,
    spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height)
{
    // 1. 初始化 RLCD 硬件驱动
    rlcd_ = new RlcdDriver(spiconfig, width, height, spi_host);

    // 2. 初始化 LVGL
    ESP_LOGI(TAG, "初始化 LVGL");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;
    // LVGL 任务栈（默认 7KB）挪到 PSRAM：它只做绘制，不参与 DMA，内部 RAM 让给语音前端
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    int transfer = width * height;
    display_ = lv_display_create(width, height);
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
    size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
    uint8_t *lvgl_buffer1 = (uint8_t *)heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
    lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 3. 初始化 RLCD 屏幕
    ESP_LOGI(TAG, "初始化 RLCD 屏幕");
    rlcd_->RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }

    // 4. 创建天气页 + 备忘录页 + 音乐页 UI
    ESP_LOGI(TAG, "创建天气页 + 备忘录页 + 音乐页 UI");
    SetupWeatherUI();
    SetupMemoUI();
    SetupMusicUI();
    // 对话浮层必须最后创建：它是三页之上的覆盖层，z 序要在最上面
    SetupChatOverlay();
    // 告诉显示框架：当前自定义 UI 已经初始化完成
    // 否则基类的 SetStatus/ShowNotification 会一直误判为“UI 未准备好”
    setup_ui_called_ = true;
    ApplyDisplayMode();

    // 5. 启动时从 NVS 加载上次保存的备忘录
    LoadMemoFromNvs();
}

CustomLcdDisplay::~CustomLcdDisplay() {
    if (update_task_handle_) {
        vTaskDelete(update_task_handle_);
    }
    delete rlcd_;
}

// ===== 备忘录功能 =====

void CustomLcdDisplay::LoadMemoFromNvs() {
    // 直接调用 RefreshMemoDisplay 从 NVS 读取并更新 UI
    RefreshMemoDisplay();
}

// 内部版本：不获取锁（调用者必须已持有 DisplayLock）
void CustomLcdDisplay::RefreshMemoDisplayInternal() {
    if (memo_row_label_[0] == nullptr) return;

    // 数据源从 "NVS 里的 JSON 原文" 换成 ReminderManager：
    // 原文只有 t/c 两个字段，排不出"最近要响的是哪条"；ReminderManager 有 due，
    // 还能把"每天重复"的下次触发时间算好。
    auto items = ReminderManager::GetInstance().List();

    // due 升序（due==0 表示只记事不响铃，排最后）
    std::stable_sort(items.begin(), items.end(), [](const ReminderItem& a, const ReminderItem& b) {
        if (a.due == 0 && b.due == 0) return false;
        if (a.due == 0) return false;
        if (b.due == 0) return true;
        return a.due < b.due;
    });

    if (memo_count_label_ != nullptr) {
        char count_buf[24];
        if (items.empty()) {
            snprintf(count_buf, sizeof(count_buf), "共 0 条");
        } else {
            snprintf(count_buf, sizeof(count_buf), "共 %d 条", static_cast<int>(items.size()));
        }
        lv_label_set_text(memo_count_label_, count_buf);
    }

    for (int i = 0; i < 3; i++) {
        lv_obj_t* row = memo_row_label_[i];
        if (row == nullptr) continue;

        const bool has_item = (i < static_cast<int>(items.size()));
        if (!has_item) {
            // 第一条空着时给个提示，避免整块空白
            if (i == 0) {
                lv_label_set_text(row, items.empty() ? "暂无提醒" : "");
                lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
                lv_obj_set_style_text_color(row, lv_color_black(), 0);
            } else {
                lv_label_set_text(row, "");
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }

        const ReminderItem& item = items[i];
        char buf[80];
        if (!item.time_label.empty()) {
            snprintf(buf, sizeof(buf), "%s %s", item.time_label.c_str(), item.content.c_str());
        } else {
            snprintf(buf, sizeof(buf), "· %s", item.content.c_str());
        }
        lv_label_set_text(row, buf);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);

        if (i == 0) {
            // 最近要响的一条：反白高亮
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(row, lv_color_black(), 0);
            lv_obj_set_style_text_color(row, lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(row, lv_color_black(), 0);
        }
    }

    ESP_LOGI(TAG, "提醒列表已刷新，共 %d 条", static_cast<int>(items.size()));
}

// 外部版本：自动获取锁（供 MCP 工具等外部调用）
void CustomLcdDisplay::RefreshMemoDisplay() {
    DisplayLockGuard lock(this);
    RefreshMemoDisplayInternal();
    // 主页卡片和备忘录页用的是同一份数据，一次刷新两处，
    // 避免"闹钟响过被删掉了，目录页还挂着"这种不一致。
    RefreshMemoPageInternal();
}

// ===== AI 消息适配（重写小智的方法，只更新左下角卡片）=====

void CustomLcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!content || strlen(content) == 0) return;

    SetShowingSystemInfo(false);

    // 主页改版后不再有"左下角 AI 大卡片"，三类消息分流到右下角双态卡片：
    //   user      → 反白块里的识别文字（这就是用户要的"备忘录变文字识别器"）
    //   assistant → 下方黑字回复
    //   system    → 提示条，临时占用语音卡，10 秒后自动收回提醒列表
    if (strcmp(role, "user") == 0) {
        SetVoiceUserText(content);
    } else if (strcmp(role, "assistant") == 0) {
        SetVoiceAiText(content);
    } else {
        SetVoiceAiText(content);
        system_card_until_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS + 10000;
        UpdateHomeBottomCard(static_cast<int>(Application::GetInstance().GetDeviceState()));
    }

    // 同一条消息也进对话浮层：不管当前停在哪个页面，说话时都能看到最近三轮
    AppendChatTurnInternal(role, content);

    // 音乐页仍保留原样文字显示
    if (music_chat_status_label_) {
        lv_label_set_long_mode(music_chat_status_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(music_chat_status_label_, content);
    }
}

void CustomLcdDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    
    // 1. 更新左侧文字（完整映射小智所有 21 种表情 + 额外状态）
    const char* text = "待命";
    if (strcmp(emotion, "neutral") == 0)         text = "待命";
    else if (strcmp(emotion, "happy") == 0)      text = "开心";
    else if (strcmp(emotion, "laughing") == 0)   text = "大笑";
    else if (strcmp(emotion, "funny") == 0)      text = "搞笑";
    else if (strcmp(emotion, "sad") == 0)        text = "难过";
    else if (strcmp(emotion, "angry") == 0)      text = "生气";
    else if (strcmp(emotion, "crying") == 0)     text = "哭泣";
    else if (strcmp(emotion, "loving") == 0)     text = "喜爱";
    else if (strcmp(emotion, "embarrassed") == 0) text = "害羞";
    else if (strcmp(emotion, "surprised") == 0)  text = "惊讶";
    else if (strcmp(emotion, "shocked") == 0)    text = "震惊";
    else if (strcmp(emotion, "thinking") == 0)   text = "思考";
    else if (strcmp(emotion, "winking") == 0)    text = "眨眼";
    else if (strcmp(emotion, "cool") == 0)       text = "耍酷";
    else if (strcmp(emotion, "relaxed") == 0)    text = "放松";
    else if (strcmp(emotion, "delicious") == 0)  text = "好吃";
    else if (strcmp(emotion, "kissy") == 0)      text = "亲亲";
    else if (strcmp(emotion, "confident") == 0)  text = "自信";
    else if (strcmp(emotion, "sleepy") == 0)     text = "犯困";
    else if (strcmp(emotion, "silly") == 0)      text = "调皮";
    else if (strcmp(emotion, "confused") == 0)   text = "困惑";
    // 额外状态
    else if (strcmp(emotion, "fear") == 0)       text = "害怕";
    else if (strcmp(emotion, "disgusted") == 0)  text = "嫌弃";
    else if (strcmp(emotion, "microchip_ai") == 0) text = "就绪";
    // 未知情绪也显示中文，不显示英文原文
    else                                         text = "待命";
    
    if (emotion_label_) {
        lv_label_set_text(emotion_label_, text);
    }
    if (music_emotion_label_) {
        lv_label_set_text(music_emotion_label_, text);
    }
    
    // 2. 小智自带的 emoji 图片：统一不显示
    //
    //    这两个 lv_image 是给基类留的占位对象，创建时没给坐标（默认 (0,0)），
    //    以前一拿到 PNG 就 un-hide，结果表情图糊在左上角，盖住状态栏胶囊 ——
    //    主页和播放器页都因此"UI 不兼容"。
    //
    //    新版三页（状态栏胶囊 + 白卡）已经没有"贴一张彩色表情图"的位置：
    //    1-bit 屏上没有彩色，emoji 抖动成黑白后既不像表情也糊掉笔画；
    //    情绪本来就已经通过文字表达（聆听中 / 思考中 / 说话中…）。
    //    所以这里一律保持隐藏，图片不再参与布局。
    if (emotion_img_) {
        lv_obj_add_flag(emotion_img_, LV_OBJ_FLAG_HIDDEN);
    }
    if (music_emotion_img_) {
        lv_obj_add_flag(music_emotion_img_, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_status_label_) lv_label_set_text(chat_status_label_, "");
    if (music_chat_status_label_) lv_label_set_text(music_chat_status_label_, "");
    // 表情不清除，保持常驻
}

// ===== 重写状态栏更新（禁用基类的 Font Awesome 文字更新）=====

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    // 不调用基类实现！
    // 基类会尝试用 lv_label_set_text 更新 network_label_ 和 battery_label_，
    // 但那些是隐藏的占位标签。我们自己的图片图标由 DataUpdateTask 管理。
}

// ===== 重写主题切换 =====

void CustomLcdDisplay::SetTheme(Theme* theme) {
    // RLCD 是 1-bit 单色屏，只有黑白两色，不需要主题切换。
    // 基类的 SetTheme 会操作 container_、content_、top_bar_ 等控件，
    // 我们的天气站 UI 没有创建这些，直接跳过避免崩溃。
    
    // 但需要保存 theme 指针，SetEmotion 需要用它来加载 emoji 图片
    current_theme_ = theme;
    ESP_LOGI(TAG, "RLCD 单色屏，跳过主题切换（已保存 theme 指针）");
}

void CustomLcdDisplay::ApplyDisplayMode() {
    // 先隐藏所有页面
    if (weather_page_) lv_obj_add_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
    if (memo_page_) lv_obj_add_flag(memo_page_, LV_OBJ_FLAG_HIDDEN);
    if (music_page_) lv_obj_add_flag(music_page_, LV_OBJ_FLAG_HIDDEN);

    // 显示当前页面
    switch (display_mode_) {
        case MODE_WEATHER:
            if (weather_page_) lv_obj_remove_flag(weather_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_MEMO:
            if (memo_page_) lv_obj_remove_flag(memo_page_, LV_OBJ_FLAG_HIDDEN);
            break;
        case MODE_MUSIC:
            if (music_page_) lv_obj_remove_flag(music_page_, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

void CustomLcdDisplay::CycleDisplayMode() {
    DisplayLockGuard lock(this);
    // 三页循环：天气 → 备忘录 → 音乐 → 天气
    switch (display_mode_) {
        case MODE_WEATHER:  display_mode_ = MODE_MEMO; break;
        case MODE_MEMO:     display_mode_ = MODE_MUSIC; break;
        case MODE_MUSIC:    display_mode_ = MODE_WEATHER; break;
    }
    if (display_mode_ == MODE_MEMO) {
        RefreshMemoPageInternal();
    }
    ApplyDisplayMode();
    const char* name = "未知";
    switch (display_mode_) {
        case MODE_WEATHER:  name = "天气页"; break;
        case MODE_MEMO:     name = "备忘录页"; break;
        case MODE_MUSIC:    name = "音乐页"; break;
    }
    ESP_LOGI(TAG, "页面切换: %s", name);
}

// 音乐页的 SetMusicInfo / SetMusicLyric / SetMusicProgress / SetMusicPlayState
// 已经搬到 music_ui.cc，和版式常量放在一起，改布局不用跨文件。
void CustomLcdDisplay::SwitchToMusicPage() {
    DisplayLockGuard lock(this);
    // 放歌是个"要看结果"的动作：收起对话浮层，把播放器页让出来。
    // 标记这一轮已经跳页，AI 后面开始说话时不会再把浮层顶回来。
    tool_jumped_this_turn_ = true;
    SetChatOverlayVisibleInternal(false);
    if (display_mode_ != MODE_MUSIC) {
        display_mode_ = MODE_MUSIC;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到音乐页");
    }
}

void CustomLcdDisplay::SwitchToWeatherPage() {
    DisplayLockGuard lock(this);
    tool_jumped_this_turn_ = true;
    SetChatOverlayVisibleInternal(false);
    if (display_mode_ != MODE_WEATHER) {
        display_mode_ = MODE_WEATHER;
        ApplyDisplayMode();
        ESP_LOGI(TAG, "自动切换到天气页");
    }
}

// ===== 省电模式 =====

void CustomLcdDisplay::NotifyUserActivity() {
    last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (power_saving_) {
        power_saving_ = false;
        ESP_LOGI(TAG, "用户活动检测到，退出省电模式");
    }
}

// ===== 主页右下角双态卡片 =====

namespace {

// UTF-8 安全的"取尾部 N 个字符"。
// 这块卡片只有 146×54，最多放 3 行 × 8 个汉字，长句子必须截断；
// 截断时保留尾部（用户最新说的话），前面用省略号。
std::string TailUtf8(const std::string& text, size_t max_chars) {
    if (text.empty()) {
        return text;
    }

    size_t i = 0;
    size_t count = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0x80) == 0) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i += 1;   // 非法字节，跳过避免死循环
        }
        count++;
    }

    if (count <= max_chars) {
        return text;
    }

    // 重新走一遍，找到"倒数第 max_chars 个字符"的起始字节
    const size_t skip = count - max_chars;
    i = 0;
    size_t seen = 0;
    while (i < text.size() && seen < skip) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0x80) == 0) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i += 1;
        }
        seen++;
    }

    return "…" + text.substr(i);
}

}  // namespace

bool CustomLcdDisplay::IsVoiceCardVisible() const {
    return voice_card_ != nullptr && !lv_obj_has_flag(voice_card_, LV_OBJ_FLAG_HIDDEN);
}

void CustomLcdDisplay::SetVoiceUserText(const char* text) {
    DisplayLockGuard lock(this);
    if (voice_user_label_ == nullptr || text == nullptr) {
        return;
    }
    const std::string snippet = TailUtf8(text, 16);
    lv_label_set_text(voice_user_label_, snippet.c_str());
}

void CustomLcdDisplay::SetVoiceAiText(const char* text) {
    DisplayLockGuard lock(this);
    if (voice_ai_label_ == nullptr || text == nullptr) {
        return;
    }
    const std::string snippet = TailUtf8(text, 16);
    lv_label_set_text(voice_ai_label_, snippet.c_str());
}

void CustomLcdDisplay::SetVoiceElapsedMs(uint32_t ms) {
    DisplayLockGuard lock(this);
    if (voice_timer_label_ == nullptr) {
        return;
    }
    char buf[16];
    const uint32_t total_sec = ms / 1000;
    snprintf(buf, sizeof(buf), "%02u:%02u",
             static_cast<unsigned>(total_sec / 60), static_cast<unsigned>(total_sec % 60));
    lv_label_set_text(voice_timer_label_, buf);
}

void CustomLcdDisplay::UpdateHomeBottomCard(int device_state) {
    DisplayLockGuard lock(this);
    if (memo_card_ == nullptr || voice_card_ == nullptr) {
        return;
    }

    const DeviceState ds = static_cast<DeviceState>(device_state);
    const bool voice_state = (ds == kDeviceStateConnecting ||
                              ds == kDeviceStateListening ||
                              ds == kDeviceStateSpeaking);

    // 以前这里会在"开始说话"时把备忘录页强制切回主页，否则看不到识别文字。
    // 现在识别文字进的是全屏对话浮层，浮层盖得住任何页面，
    // 所以不再抢用户的页面切换——停在备忘录页听完话，还停在备忘录页。

    // system 提示临时占用（例如"音量已调到 50%"），到期自动收回
    const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (system_card_until_ms_ != 0 && now_ms >= system_card_until_ms_) {
        system_card_until_ms_ = 0;
    }
    const bool system_card = (system_card_until_ms_ != 0);
    const bool show_voice = voice_state || system_card;

    const int mode = show_voice ? 1 : 0;

    // 进入语音态时开始计时；离开时重置
    if (voice_state && last_bottom_mode_ != 1) {
        voice_start_ms_ = now_ms;
    }

    if (mode != last_bottom_mode_) {
        if (mode == 1) {
            lv_obj_add_flag(memo_card_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(voice_card_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(voice_card_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(memo_card_, LV_OBJ_FLAG_HIDDEN);
            RefreshMemoDisplayInternal();
        }
        last_bottom_mode_ = mode;
    }

    if (mode == 0) {
        return;
    }

    // 徽标文字
    const char* badge = "聆听中";
    if (system_card && !voice_state) {
        badge = "提示";
    } else if (ds == kDeviceStateConnecting) {
        badge = "连接中";
    } else if (ds == kDeviceStateSpeaking) {
        badge = "回答中";
    } else if (ds == kDeviceStateListening) {
        badge = "聆听中";
    }

    if (voice_badge_label_ != nullptr) {
        lv_label_set_text(voice_badge_label_, badge);
    }

    if (voice_state) {
        SetVoiceElapsedMs(now_ms - voice_start_ms_);
    }
}

bool CustomLcdDisplay::LoadHomePhoto(int index) {
    DisplayLockGuard lock(this);
    if (photo_img_ == nullptr) {
        return false;
    }

    // 图片按容器的内容区尺寸抖动（外框 214×252，去掉 2px 边框 = 210×248）
    bool ok = HomePhoto::GetInstance().Load(photo_img_, 210, 248, index);

    // SD 卡没插、目录为空、图全坏了 —— 退回固件里内置的默认壁纸。
    // 只有连默认图都展不出来（PSRAM 分配失败）才显示"把 JPG 放进 /sdcard/photos"。
    if (!ok) {
        ok = HomePhoto::GetInstance().LoadDefault(photo_img_, 210, 248);
        if (ok) {
            ESP_LOGI(TAG, "主页图片使用内置默认图（SD 卡无可用照片）");
        }
    }

    if (ok) {
        lv_obj_clear_flag(photo_img_, LV_OBJ_FLAG_HIDDEN);
        if (photo_hint_label_ != nullptr) {
            lv_obj_add_flag(photo_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "主页图片已加载: %s", HomePhoto::GetInstance().CurrentName().c_str());
    } else {
        lv_obj_add_flag(photo_img_, LV_OBJ_FLAG_HIDDEN);
        if (photo_hint_label_ != nullptr) {
            lv_obj_clear_flag(photo_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGW(TAG, "主页图片加载失败（SD 卡未插 / 目录为空 / 图片损坏）");
    }
    return ok;
}

bool CustomLcdDisplay::NextHomePhoto() {
    DisplayLockGuard lock(this);
    if (photo_img_ == nullptr) {
        return false;
    }

    const bool ok = HomePhoto::GetInstance().Next(photo_img_, 210, 248);
    if (ok) {
        lv_obj_clear_flag(photo_img_, LV_OBJ_FLAG_HIDDEN);
        if (photo_hint_label_ != nullptr) {
            lv_obj_add_flag(photo_hint_label_, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "已切到下一张: %s", HomePhoto::GetInstance().CurrentName().c_str());
    }
    return ok;
}
