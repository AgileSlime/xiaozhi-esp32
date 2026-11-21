// 界面刷新任务（原 DataUpdateTask）
//
// 职责收窄后的版本（FreeRTOS 拆分第一刀）：
//   本任务只做"把数据画到屏幕上"：
//     - 时钟 / 日期标签（时间来源：ClockService 的节拍快照）
//     - 温湿度、天气、电池、WiFi、AI 状态
//
// 已经搬走的部分：
//   - NTP 同步 / 退避重试 / 24h 校准 / 时间跳变保护 → managers/clock_service.cc（clock_task）
//   - 备忘闹钟检查 / 到点提醒 / NVS 读写          → managers/reminder_manager.cc（reminder_task）
//
// 唤醒方式：
//   ClockService 每分钟（minute_changed）给本任务一次任务通知，替代原来"每秒空转 + 比对"。
//   vTaskDelay 也改成了 ulTaskNotifyTake，省电模式下不会被秒级通知打醒。

#include "custom_lcd_display.h"

#include <cmath>
#include <cstring>
#include <sys/time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "wifi_manager.h"
#include "managers/clock_service.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"
#include "managers/photo_loader.h"
#include "managers/photo_web_server.h"

// 声明状态栏图标（本任务负责更新图标）
LV_IMAGE_DECLARE(ui_img_wifi);
LV_IMAGE_DECLARE(ui_img_wifi_low);
LV_IMAGE_DECLARE(ui_img_wifi_off);
LV_IMAGE_DECLARE(ui_img_battery_full);
LV_IMAGE_DECLARE(ui_img_battery_medium);
LV_IMAGE_DECLARE(ui_img_battery_low);
LV_IMAGE_DECLARE(ui_img_battery_charging);

static const char *TAG = "UiRefresh";

void CustomLcdDisplay::StartDataUpdateTask() {
    // 订阅时钟节拍：只在"分钟变化"时唤醒 UI 任务，避免每秒空转
    ClockService::GetInstance().AddListener([this](const ClockTick& tick) {
        if (!tick.minute_changed) {
            return;
        }
        if (update_task_handle_ != nullptr) {
            xTaskNotifyGive(update_task_handle_);
        }
    });

    // 栈从 16KB 下调到 8KB，给音频/MQTT 留更多 SRAM 余量
    // 优先级保持较低，避免与语音收发实时链路抢占 CPU
    // 进一步：整个栈放到 PSRAM。这个任务只做 UI 刷新，不进中断、不碰 DMA，
    // 8KB 内部 RAM 让给语音前端更划算（AFE 的两个实例才是真正吃内部 RAM 的大户）。
    xTaskCreateWithCaps(DataUpdateTask, "ui_refresh_task", 8192, this, 2,
                        &update_task_handle_, MALLOC_CAP_SPIRAM);
}

void CustomLcdDisplay::DataUpdateTask(void *arg) {
    CustomLcdDisplay *self = (CustomLcdDisplay *)arg;

    // 电池电量变化很慢，降频采样可显著减轻 ADC 和 UI 刷新压力
    const uint32_t BATTERY_POLL_INTERVAL = 10 * 1000;   // 每 10 秒采样一次
    uint32_t last_battery_poll_ms = 0;
    int cached_battery_level = 0;
    bool cached_charging = false, cached_discharging = false;
    bool battery_cached = false;

    // 等待一会让系统启动完成（SD 卡 / 音频 / WiFi 都在初始化）
    vTaskDelay(pdMS_TO_TICKS(3000));

    // 主页美图：先立刻摊一张上屏（NVS 常驻图 / SD 卡图 / 内置默认图），
    // 不等 WiFi。否则断网时这个调用被下面"等 WiFi 最多 15 秒"挡住，
    // 屏幕会一直停在默认图——这就是"断网就变回默认图"的根因。
    self->LoadHomePhoto(-1);

    // 图片上传服务：WiFi 一有地址就自动开。
    // 以前只在用户说「打开图片上传」时才启动，于是直接拿手机开网页的人
    // 只会看到"无法访问此网站"，看上去就是"上传坏了"。
    for (int i = 0; i < 30 && WifiManager::GetInstance().GetIpAddress().empty(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!WifiManager::GetInstance().GetIpAddress().empty()) {
        auto& web = PhotoWebServer::GetInstance();
        if (web.IsRunning() || web.Start()) {
            ESP_LOGI(TAG, "图片上传服务已就绪: %s", web.GetUrl().c_str());
        }
    } else {
        ESP_LOGW(TAG, "WiFi 还没拿到地址，图片上传服务暂不启动");
    }

    // 初始化活动时间（系统启动算一次活动）
    self->last_activity_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (1) {
        auto& app = Application::GetInstance();
        DeviceState ds = app.GetDeviceState();

        const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const bool in_audio_session = (ds == kDeviceStateConnecting ||
                                       ds == kDeviceStateListening ||
                                       ds == kDeviceStateSpeaking);

        // ===== 网页闲置后把 WiFi 还回省电 =====
        // 有网页在访问时 photo_web_server 会把 WiFi 拉到 PERFORMANCE，
        // 否则省电模式下每个请求要白等一轮 1.02 秒的 DTIM，页面根本开不出来。
        // 反过来，这里负责在没人访问时收回——但只在设备真的空闲时收，
        // 正在对话或放歌的时候降射频会直接把声音卡出爆音。
        if (PhotoWebServer::GetInstance().WifiBoosted() &&
            !PhotoWebServer::GetInstance().IsActiveWindow() &&
            ds == kDeviceStateIdle && !app.IsMusicPlaying()) {
            PhotoWebServer::GetInstance().ReleaseWifiBoost();
        }

        // ===== 时间（只读快照，不做任何同步动作）=====
        const ClockTick tick = ClockService::GetInstance().Snapshot();
        const struct tm timeinfo = tick.tm_local;
        const bool minute_changed = (timeinfo.tm_min != self->last_min_) ||
                                    (timeinfo.tm_hour != self->last_hour_);

        // ===== 时钟/日期标签（每分钟）=====
        // 若正在显示系统信息滚动，跳过 UI 更新（避免锁竞争）
        if (minute_changed && !self->showing_system_info_) {
            DisplayLockGuard lock(self);

            char time_buf[16];
            strftime(time_buf, sizeof(time_buf), "%H:%M", &timeinfo);
            if (self->time_label_) lv_label_set_text(self->time_label_, time_buf);
            if (self->music_time_label_) lv_label_set_text(self->music_time_label_, time_buf);
            if (self->memo_time_label_) lv_label_set_text(self->memo_time_label_, time_buf);
            if (self->chat_overlay_time_) lv_label_set_text(self->chat_overlay_time_, time_buf);

            // 日期合成一行（时间卡只有 158×102，拆成"星期 + 日号"两块摆不下）
            static const char *weeks_cn[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
            const int wday = (timeinfo.tm_wday >= 0 && timeinfo.tm_wday <= 6) ? timeinfo.tm_wday : 0;

            char date_buf[32];
            snprintf(date_buf, sizeof(date_buf), "%d月%d日 %s",
                     timeinfo.tm_mon + 1, timeinfo.tm_mday, weeks_cn[wday]);
            if (self->date_label_) lv_label_set_text(self->date_label_, date_buf);

            self->last_min_ = timeinfo.tm_min;
            self->last_hour_ = timeinfo.tm_hour;
            ESP_LOGI(TAG, "时间已更新: %s %s", time_buf, date_buf);
        }

        // ===== 其他 UI 更新（需要重新获取锁）=====
        if (!self->showing_system_info_) {
            DisplayLockGuard lock(self);
            static uint32_t last_noncritical_ui_update_ms = 0;
            const bool allow_noncritical_update =
                !in_audio_session ||
                last_noncritical_ui_update_ms == 0 ||
                (now_ms - last_noncritical_ui_update_ms >= 5000);

            // 对话期间将非关键 UI/传感器刷新降频到 5 秒一次，避免与语音链路抢占 CPU
            if (allow_noncritical_update) {
                last_noncritical_ui_update_ms = now_ms;

                // 2. 温湿度更新
                SensorData sd = SensorManager::getInstance().getTempHumidity();
                if (sd.valid) {
                    if (fabs(sd.temperature - self->last_temp_) > 0.2f || fabs(sd.humidity - self->last_humi_) > 1.0f) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%.1f°C  %.0f%%", sd.temperature, sd.humidity);
                        if (self->sensor_label_) lv_label_set_text(self->sensor_label_, buf);
                        self->last_temp_ = sd.temperature;
                        self->last_humi_ = sd.humidity;
                    }
                }

                // 3. 天气：主页版式（图片 + 时间 + 提醒）里没有天气的位置，
                //    这里不写 UI；天气数据仍可由 MCP 工具 self.weather.update 写入，
                //    需要展示时再找地方。

                // 4. 电池状态更新（采样降频 + 变化更新）
                auto& board = Board::GetInstance();
                if (!battery_cached || last_battery_poll_ms == 0 ||
                    (now_ms - last_battery_poll_ms >= BATTERY_POLL_INTERVAL)) {
                    int level = 0;
                    bool charging = false, discharging = false;
                    if (board.GetBatteryLevel(level, charging, discharging)) {
                        cached_battery_level = level;
                        cached_charging = charging;
                        cached_discharging = discharging;
                        battery_cached = true;
                        last_battery_poll_ms = now_ms;
                    }
                }

                if (battery_cached) {
                    static int last_icon_mode = -1;       // 0=low,1=medium,2=full,3=charging
                    static int last_battery_level = -1;   // 上次显示的电量百分比
                    int icon_mode = 2;
                    if (cached_charging) {
                        icon_mode = 3;
                    } else if (cached_battery_level < 20) {
                        icon_mode = 0;
                    } else if (cached_battery_level < 60) {
                        icon_mode = 1;
                    }

                    if (icon_mode != last_icon_mode) {
                        const void* icon_src = &ui_img_battery_full;
                        if (icon_mode == 3) icon_src = &ui_img_battery_charging;
                        else if (icon_mode == 0) icon_src = &ui_img_battery_low;
                        else if (icon_mode == 1) icon_src = &ui_img_battery_medium;

                        if (self->battery_icon_img_) {
                            lv_image_set_src(self->battery_icon_img_, icon_src);
                        }
                        if (self->music_battery_icon_img_) {
                            lv_image_set_src(self->music_battery_icon_img_, icon_src);
                        }
                        if (self->memo_battery_icon_img_) {
                            lv_image_set_src(self->memo_battery_icon_img_, icon_src);
                        }
                        last_icon_mode = icon_mode;
                    }

                    if (self->battery_pct_label_ && cached_battery_level != last_battery_level) {
                        char bat_buf[16];
                        snprintf(bat_buf, sizeof(bat_buf), "%d%%", cached_battery_level);
                        lv_label_set_text(self->battery_pct_label_, bat_buf);
                        if (self->music_battery_pct_label_) lv_label_set_text(self->music_battery_pct_label_, bat_buf);
                        if (self->memo_battery_pct_label_) lv_label_set_text(self->memo_battery_pct_label_, bat_buf);
                        last_battery_level = cached_battery_level;
                    }

                    // 低电量提醒（对齐原版行为）：
                    // - 放电且低于 20% 时显示弹窗并播一次提示音
                    // - 回升到 25% 及以上（或进入充电）后隐藏，避免 19/20% 抖动反复闪烁
                    static bool low_battery_popup_visible = false;
                    const bool should_show_low_battery = (!cached_charging && cached_discharging && cached_battery_level < 20);
                    const bool should_hide_low_battery = (cached_charging || !cached_discharging || cached_battery_level >= 25);
                    if (self->low_battery_popup_) {
                        if (!low_battery_popup_visible && should_show_low_battery) {
                            lv_obj_remove_flag(self->low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                            app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                            low_battery_popup_visible = true;
                        } else if (low_battery_popup_visible && should_hide_low_battery) {
                            lv_obj_add_flag(self->low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                            low_battery_popup_visible = false;
                        }
                    }
                }

                // 5. WiFi 图标更新（状态变化时才更新）
                static DeviceState last_wifi_state = kDeviceStateUnknown;
                if (ds != last_wifi_state) {
                    const void* wifi_src = &ui_img_wifi_off;
                    if (ds != kDeviceStateStarting && ds != kDeviceStateWifiConfiguring) {
                        wifi_src = &ui_img_wifi;
                    } else if (ds == kDeviceStateWifiConfiguring) {
                        wifi_src = &ui_img_wifi_low;
                    }
                    if (self->wifi_icon_img_) {
                        lv_image_set_src(self->wifi_icon_img_, wifi_src);
                    }
                    if (self->music_wifi_icon_img_) {
                        lv_image_set_src(self->music_wifi_icon_img_, wifi_src);
                    }
                    last_wifi_state = ds;
                }
            }

            // 6. AI 状态更新
            static DeviceState last_ds = kDeviceStateUnknown;
            if (ds != last_ds) {
                // AI 状态发生变化（对话、聆听等），视为用户活动
                if (ds == kDeviceStateListening || ds == kDeviceStateSpeaking ||
                    ds == kDeviceStateConnecting) {
                    self->NotifyUserActivity();
                }

                // 更新左侧表情区域（显示当前状态简称）
                const char* emotion_text = "待命";
                const char* status_text = "";
                switch (ds) {
                    case kDeviceStateConnecting:      emotion_text = "连接"; status_text = "连接中..."; break;
                    case kDeviceStateListening:       emotion_text = "聆听"; status_text = "聆听中..."; break;
                    case kDeviceStateSpeaking:        emotion_text = "说话"; break;  // 对话文字由 SetChatMessage 更新
                    case kDeviceStateStarting:        emotion_text = "启动"; status_text = "启动中..."; break;
                    case kDeviceStateWifiConfiguring: emotion_text = "配网"; break;   // 详细文案由 Alert() -> SetChatMessage 设置
                    case kDeviceStateUpgrading:       emotion_text = "升级"; status_text = "升级中..."; break;
                    case kDeviceStateActivating:      emotion_text = "激活"; break;   // 详细文案由 Alert() -> SetChatMessage 设置
                    case kDeviceStateFatalError:      emotion_text = "错误"; status_text = "发生错误"; break;
                    case kDeviceStateIdle:            emotion_text = "待命"; break;   // 空闲时表情由 SetEmotion 管理
                    default: break;
                }
                if (self->emotion_label_) {
                    lv_label_set_text(self->emotion_label_, emotion_text);
                }
                if (self->music_emotion_label_) {
                    lv_label_set_text(self->music_emotion_label_, emotion_text);
                }
                // 非说话/配网/激活状态时更新右侧文字（这些状态由 Alert/SetChatMessage 管理详细信息）
                if (ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->chat_status_label_ && strlen(status_text) > 0) {
                    // 状态短文案（如"聆听中..."）必须强制不滚动，避免继承上一条长文本动画
                    self->SetShowingSystemInfo(false);
                    lv_anim_delete(self->chat_status_label_, nullptr);
                    lv_label_set_long_mode(self->chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_obj_align(self->chat_status_label_, LV_ALIGN_LEFT_MID, 64 + 20, 0);
                    lv_label_set_text(self->chat_status_label_, status_text);
                }
                if (ds != kDeviceStateSpeaking && ds != kDeviceStateWifiConfiguring &&
                    ds != kDeviceStateActivating && self->music_chat_status_label_ && strlen(status_text) > 0) {
                    lv_label_set_long_mode(self->music_chat_status_label_, LV_LABEL_LONG_WRAP);
                    lv_label_set_text(self->music_chat_status_label_, status_text);
                }
                last_ds = ds;
            }
        }  // DisplayLockGuard 自动释放

        // ===== 省电模式检测 =====
        // 5 分钟无活动（无按钮、无 AI 对话）时进入省电模式，降低刷新频率
        {
            uint32_t check_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t activity_ms = self->last_activity_ms_;
            if (!self->power_saving_ && activity_ms > 0 && check_ms >= activity_ms) {
                if (check_ms - activity_ms > self->IDLE_TIMEOUT_MS) {
                    self->power_saving_ = true;
                    ESP_LOGI(TAG, "⚡ 5 分钟无活动，进入省电模式（刷新间隔 %d 秒 → %d 秒）",
                             self->NORMAL_REFRESH_MS / 1000, self->SAVING_REFRESH_MS / 1000);
                }
            }
        }

        // ===== 主页右下角卡片：空闲显示提醒，语音显示识别文字 =====
        // 内部有状态去重，重复调用不会引起重绘
        self->UpdateHomeBottomCard(static_cast<int>(ds));

        // ===== 对话显示：每页一张小卡（语音态出现，空闲立刻收回）=====
        // 三页的小卡都长在本页版式里，不覆盖全屏；工具跳页那一轮自动让位。
        self->UpdatePageChatWindows(static_cast<int>(ds));

        // ===== 全屏放大的对话记录 =====
        // 它只在"话说完之前"有意义。以前是闲置 12 秒才收，实际用起来就是：
        // 说完话一抬头，整屏还盖着对话，想看天气还得等十几秒。
        // 现在改成——这一轮话一结束（回到空闲）立刻收，12 秒只作为兜底。
        {
            const bool in_session = (ds == kDeviceStateConnecting ||
                                     ds == kDeviceStateListening ||
                                     ds == kDeviceStateSpeaking);
            if (in_session && self->chat_overlay_visible_) {
                self->close_overlay_on_idle_ = true;
            }
            if (ds == kDeviceStateIdle && self->chat_overlay_visible_) {
                if (self->close_overlay_on_idle_) {
                    self->close_overlay_on_idle_ = false;
                    self->SetChatOverlayVisible(false);
                    ESP_LOGI(TAG, "这一轮对话结束，全屏对话记录自动收起");
                } else {
                    const uint32_t hide_at = self->chat_hide_at_ms_;
                    if (hide_at != 0 && now_ms >= hide_at) {
                        self->SetChatOverlayVisible(false);
                    }
                }
            }
        }

        // ===== 隔天自动换图 + 首次加载失败重试 =====
        {
            const int today = timeinfo.tm_yday;
            const bool need_load = (self->last_photo_check_day_ >= 0 && today != self->last_photo_check_day_);
            // 网页上传的那张图 index 恒为 -1，如果不排除，会每分钟被判成"没加载过"重画一次，
            // 1-bit 屏上就是每分钟闪一下。
            const bool never_loaded = (HomePhoto::GetInstance().CurrentIndex() < 0) &&
                                      !HomePhoto::GetInstance().HasOverride();

            if (need_load || (never_loaded && minute_changed)) {
                self->last_photo_check_day_ = today;
                self->LoadHomePhoto(-1);
            } else if (self->last_photo_check_day_ < 0 && today >= 0) {
                self->last_photo_check_day_ = today;
            }
        }

        // 动态刷新间隔：正常 1 秒，省电 5 秒
        // 用任务通知等待：时钟分钟变化会立刻唤醒，不必每秒空转
        int delay_ms = self->power_saving_ ? self->SAVING_REFRESH_MS : self->NORMAL_REFRESH_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}
