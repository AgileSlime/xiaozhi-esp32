#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_log.h>
#include <esp_system.h>
#include <string>
#include <soc/rtc.h>
#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "config.h"
#include "codecs/box_audio_codec.h"
#include "wifi_station.h"
#include "mcp_server.h"
#include "settings.h"
#include <cJSON.h>
#include "lvgl.h"
#include "managers/sensor_manager.h"
#include "managers/sdcard_manager.h"
#include "managers/clock_service.h"
#include "managers/reminder_manager.h"
#include "managers/music_library.h"
#include "managers/photo_loader.h"
#include "managers/photo_web_server.h"

// 声明小智字体（用于系统信息显示时临时切换字体大小）
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_puhui_16_4);
#include "managers/weather_manager.h"

#define TAG "waveshare_rlcd_4_2"

class CustomBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button user_button_;  // GPIO18 用户按键
    CustomLcdDisplay *display_;
    adc_oneshot_unit_handle_t adc1_handle;
    adc_cali_handle_t cali_handle;

    // 校验时间标签是否为 HH:MM（24 小时制）
    bool IsValidMemoTimeLabel(const std::string& time_str) {
        if (time_str.empty()) {
            return true;  // 允许无时间备忘
        }
        if (time_str.size() != 5 || time_str[2] != ':') {
            return false;
        }
        if (time_str[0] < '0' || time_str[0] > '9' ||
            time_str[1] < '0' || time_str[1] > '9' ||
            time_str[3] < '0' || time_str[3] > '9' ||
            time_str[4] < '0' || time_str[4] > '9') {
            return false;
        }
        int hh = (time_str[0] - '0') * 10 + (time_str[1] - '0');
        int mm = (time_str[3] - '0') * 10 + (time_str[4] - '0');
        return (hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59);
    }

    void InitializeI2c() {
        // I2C 总线初始化
        // 这条 I2C 总线被多个设备共享：
        // - ES8311 音频解码器
        // - ES7210 音频编码器
        // - SHTC3 温湿度传感器
        // - PCF85063 RTC 时钟
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = ESP32_I2C_HOST;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSensors() {
        // 初始化传感器（使用同一条 I2C 总线）
        SensorManager::getInstance().init(i2c_bus_);
        ESP_LOGI(TAG, "传感器初始化完成");
    }

    void InitializeSdcard() {
        // 初始化 SD 卡（SDMMC 模式，板载 SD 卡槽默认引脚）
        bool ok = SdcardManager::getInstance().init();
        if (ok) {
            ESP_LOGI(TAG, "SD 卡初始化成功");
        } else {
            ESP_LOGW(TAG, "SD 卡初始化失败（可能未插卡），白噪音功能不可用");
        }
    }

    void InitializeButtons() { 
        // BOOT 按钮（GPIO0）- 主要交互按键
        boot_button_.OnClick([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        // USER 按钮（GPIO18）- 辅助功能按键
        user_button_.OnClick([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            if (display_) {
                display_->CycleDisplayMode();
            }
            ESP_LOGI(TAG, "USER 按钮单击：切换天气页/音乐页");
        });

        user_button_.OnDoubleClick([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            // 双击：刷新所有数据（天气、传感器、时间）
            RefreshAllData();
        });

        user_button_.OnLongPress([this]() {
            if (display_) display_->NotifyUserActivity();  // 记录用户活动
            // 长按：显示系统信息
            ShowSystemInfo();
        });
    }

    // USER 按钮功能实现
    void ShowSystemInfo() {
        // 显示详细系统信息到 AI 对话区（启用多行滚动）
        char info[512];
        
        // 内存信息
        size_t free_heap = esp_get_free_heap_size();
        size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        
        // CPU 信息
        rtc_cpu_freq_config_t cpu_freq_conf;
        rtc_clk_cpu_freq_get_config(&cpu_freq_conf);
        uint32_t cpu_freq_mhz = cpu_freq_conf.freq_mhz;
        
        // 电池信息
        int battery_level = 0;
        bool charging = false, discharging = false;
        GetBatteryLevel(battery_level, charging, discharging);
        
        // WiFi 信息
        auto& app = Application::GetInstance();
        const char* wifi_status = "未连接";
        if (app.GetDeviceState() != kDeviceStateStarting && 
            app.GetDeviceState() != kDeviceStateWifiConfiguring) {
            wifi_status = "已连接";
        }
        
        // 运行时间
        uint64_t uptime_sec = esp_timer_get_time() / 1000000;
        uint32_t uptime_hours = uptime_sec / 3600;
        uint32_t uptime_mins = (uptime_sec % 3600) / 60;
        
        // 计算百分比
        int heap_percent = (int)(((total_heap - free_heap) * 100.0f) / total_heap);
        int psram_percent = total_psram > 0 ? 
                           (int)(((total_psram - free_psram) * 100.0f) / total_psram) : 0;
        
        // 详细格式（单份内容，用于鱼咬尾拼接）
        snprintf(info, sizeof(info), 
                 "=== 系统信息 ===\n"
                 "CPU: %luMHz\n"
                 "运行: %luh%lumin\n"
                 "SRAM: \n %dKB/%dKB (%d%%)\n"
                 "PSRAM: \n %dMB/%dMB (%d%%)\n"
                 "电池: %d%% %s\n"
                 "WiFi: %s\n"
                 "==============\n"
                 "\n",  // 分隔符
                 cpu_freq_mhz,
                 uptime_hours, uptime_mins,
                 (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                 (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                 battery_level, charging ? "充电中" : "放电中",
                 wifi_status);
        
        if (display_) {
            lv_obj_t* chat_label = display_->GetChatStatusLabel();
            if (chat_label) {
                // 暂停 DataUpdateTask 对 UI 的更新（避免锁竞争导致 watchdog 超时）
                display_->SetShowingSystemInfo(true);
                
                {
                    DisplayLockGuard lock(display_);
                    
                    // 先删除旧动画（防止冲突）
                    lv_anim_delete(chat_label, nullptr);
                    
                    // 🐟 鱼咬尾：拼接两份相同内容
                    std::string info_double = std::string(info) + std::string(info);
                    
                    // 🔑 关键修复：切换到 TOP_LEFT 绝对定位
                    // 原因：label 初始化时用的是 LV_ALIGN_LEFT_MID（居中对齐），
                    // LVGL 内部会存储这个对齐方式，布局刷新时会重新计算位置，
                    // 导致动画里 set_y 设的值被覆盖。
                    // 切换到 TOP_LEFT 后，Y=0 就是父容器顶部，动画不会被干扰。
                    const int text_x = 64 + 20;  // emotion_w + 间距，保持文字在分隔线右侧
                    lv_obj_align(chat_label, LV_ALIGN_TOP_LEFT, text_x, 0);
                    
                    lv_label_set_text(chat_label, info_double.c_str());
                    lv_label_set_long_mode(chat_label, LV_LABEL_LONG_WRAP);
                    
                    // 强制计算布局，获取实际高度
                    lv_obj_update_layout(chat_label);
                    int label_h = lv_obj_get_height(chat_label);  // 双份内容的总高度
                    int single_h = label_h / 2;  // 单份内容高度
                    
                    // 🐟 鱼咬尾动画原理：
                    // 内容 = [A][A]（两份完全相同的文字首尾相接）
                    // Y=0 时显示第一个 A 的开头
                    // 向上滚动到 Y=-single_h 时，显示第二个 A 的开头
                    // 因为两个 A 完全一样，动画重复跳回 Y=0 时视觉上无缝衔接！
                    lv_anim_t a;
                    lv_anim_init(&a);
                    lv_anim_set_var(&a, chat_label);
                    lv_anim_set_values(&a, 0, -single_h);
                    lv_anim_set_delay(&a, 1500);  // 开始前停顿 1.5 秒，让用户先看到开头
                    lv_anim_set_duration(&a, single_h * 30);  // 速度：每像素 30ms
                    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
                    lv_anim_set_repeat_delay(&a, 0);  // 无缝重复，不停顿
                    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
                        lv_obj_set_y((lv_obj_t *)obj, v);
                    });
                    lv_anim_start(&a);
                }  // ← DisplayLockGuard 在这里自动释放
            }
        }
        
        ESP_LOGI(TAG, "系统信息: CPU=%luMHz, 运行=%luh%lumin, SRAM=%d%%, PSRAM=%d%%, 电量=%d%%", 
                 cpu_freq_mhz, uptime_hours, uptime_mins, heap_percent, psram_percent, battery_level);
    }

    void RefreshAllData() {
        ESP_LOGI(TAG, "手动刷新所有数据...");
        
        // 暂时停用板载和风天气拉取，天气请通过 MCP 工具 self.weather.update 刷新
        // WeatherManager::getInstance().update();
        
        // 请求时钟任务在下一个空闲窗口做一次 NTP 同步（非阻塞，不卡按键处理）
        ClockService::GetInstance().RequestSync();
        
        // 强制刷新屏幕显示
        if (display_) {
            display_->SetChatMessage("system", "正在刷新数据...\n时间已更新，天气等待 MCP 同步");
        }
        
        ESP_LOGI(TAG, "数据刷新完成");
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        // ===== 系统信息工具 =====
        mcp_server.AddTool("self.system.info",
            "Get device system information (CPU, memory, battery, WiFi status).\n"
            "Use when user asks: '系统信息', 'CPU频率', '内存使用情况', '电量多少', 'system status', 'how much RAM'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                // 收集系统信息
                size_t free_heap = esp_get_free_heap_size();
                size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
                size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
                
                rtc_cpu_freq_config_t cpu_freq_conf;
                rtc_clk_cpu_freq_get_config(&cpu_freq_conf);
                uint32_t cpu_freq_mhz = cpu_freq_conf.freq_mhz;
                
                int battery_level = 0;
                bool charging = false, discharging = false;
                GetBatteryLevel(battery_level, charging, discharging);
                
                auto& app = Application::GetInstance();
                const char* wifi_status = "未连接";
                if (app.GetDeviceState() != kDeviceStateStarting && 
                    app.GetDeviceState() != kDeviceStateWifiConfiguring) {
                    wifi_status = "已连接";
                }
                
                uint64_t uptime_sec = esp_timer_get_time() / 1000000;
                uint32_t uptime_hours = uptime_sec / 3600;
                uint32_t uptime_mins = (uptime_sec % 3600) / 60;
                
                int heap_percent = (int)(((total_heap - free_heap) * 100.0f) / total_heap);
                int psram_percent = total_psram > 0 ? 
                                   (int)(((total_psram - free_psram) * 100.0f) / total_psram) : 0;
                
                // 格式化为自然语言（AI 容易读出来）
                char info[512];
                snprintf(info, sizeof(info),
                         "系统运行正常。CPU频率%luMHz，已运行%lu小时%lu分钟。"
                         "内存方面，SRAM使用了%dKB，占总量%dKB的%d%%；"
                         "PSRAM使用了%dMB，占总量%dMB的%d%%。"
                         "电池电量%d%%，当前%s。WiFi%s。",
                         cpu_freq_mhz, uptime_hours, uptime_mins,
                         (total_heap - free_heap) / 1024, total_heap / 1024, heap_percent,
                         (total_psram - free_psram) / 1024 / 1024, total_psram / 1024 / 1024, psram_percent,
                         battery_level, charging ? "正在充电" : "使用电池供电",
                         wifi_status);
                
                ESP_LOGI(TAG, "AI查询系统信息");
                return std::string(info);
            });

        // ===== 天气写入工具（由 AI 侧 MCP 查询后回写到设备）=====
        mcp_server.AddTool("self.weather.update",
            "Write weather data to the device screen cache.\n"
            "Use this after AI gets weather from an external MCP/weather source.\n"
            "Args:\n"
            "  `city`: City name (e.g. '苏州')\n"
            "  `text`: Weather text (e.g. '晴', '多云', '小雨')\n"
            "  `temp`: Temperature string without unit (e.g. '5', '-2', '26')\n"
            "  `update_time`: Optional time text (e.g. '2026-02-11 23:45')",
            PropertyList({
                Property("city", kPropertyTypeString),
                Property("text", kPropertyTypeString),
                Property("temp", kPropertyTypeString),
                Property("update_time", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto city = properties["city"].value<std::string>();
                auto text = properties["text"].value<std::string>();
                auto temp = properties["temp"].value<std::string>();
                auto update_time = properties["update_time"].value<std::string>();

                bool ok = WeatherManager::getInstance().updateFromExternal(city, text, temp, update_time);
                if (!ok) {
                    return std::string("天气写入失败：请检查 city/text/temp 是否为空");
                }

                ESP_LOGI(TAG, "AI写入天气成功: %s %s %s°C", city.c_str(), text.c_str(), temp.c_str());
                return std::string("天气已更新：") + city + " " + text + " " + temp + "°C";
            });
        
        // ===== 配网工具 =====
        mcp_server.AddTool("self.disp.network", "重新配网", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            EnterWifiConfigMode();
            return true;
        });

        // ===== 屏幕切换工具（语音可调用）=====
        mcp_server.AddTool(
            "self.disp.switch",
            "Switch display page between weather(home), memo(alarm list), and music.\n"
            "Use when user says: '切到音乐页', '打开天气页', '切换屏幕', '打开备忘录', '回到主页', 'switch screen'.\n"
            "Args:\n"
            "  `mode`: 'toggle' | 'memo' | 'music' | 'weather' (default: 'toggle')\n"
            "  'weather' means the home page (photo + clock + reminders).",
            PropertyList({
                Property("mode", kPropertyTypeString, std::string("toggle"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!display_) {
                    return std::string("显示器未初始化，暂时无法切换页面");
                }

                auto mode = properties["mode"].value<std::string>();

                // 统一小写判断
                for (auto& ch : mode) {
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = static_cast<char>(ch - 'A' + 'a');
                    }
                }

                display_->NotifyUserActivity();

                if (mode == "toggle") {
                    display_->CycleDisplayMode();
                } else if (mode == "memo") {
                    display_->SwitchToMemoPage();
                } else if (mode == "music") {
                    display_->SwitchToMusicPage();
                } else if (mode == "weather") {
                    display_->SwitchToWeatherPage();
                } else {
                    return std::string("参数 mode 无效，请使用 toggle/memo/music/weather");
                }

                if (display_->IsMemoMode()) return std::string("已切换到备忘录页");
                if (display_->IsMusicMode()) return std::string("已切换到音乐页");
                return std::string("已切换到主页");
            }
        );

        // ===== 图片上传（局域网网页）=====
        mcp_server.AddTool("self.photo.upload",
            "Start the LAN photo upload web server and return the address to open in a phone/PC browser.\n"
            "Use when user says: '怎么传图片', '我要换壁纸', '打开图片上传', '上传照片', 'upload photos'.\n"
            "After calling, tell the user to open the returned URL in a browser on the same WiFi.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                auto& web = PhotoWebServer::GetInstance();
                if (!web.Start()) {
                    return std::string("图片上传服务启动失败，请确认 SD 卡已插好");
                }
                const std::string url = web.GetUrl();
                if (url.empty()) {
                    return std::string("上传服务已开启，但设备还没连上 WiFi；联网后再问我一次地址");
                }
                return "图片上传已开启：" + url + " ，手机浏览器打开这个地址就能传图";
            });

        mcp_server.AddTool("self.photo.url",
            "Get the photo upload page address in case the user forgot it.\n"
            "Use when user asks: '图片上传地址是多少', '相册网址', 'upload address'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                auto& web = PhotoWebServer::GetInstance();
                if (!web.IsRunning()) {
                    return std::string("图片上传服务还没开，说「打开图片上传」我就开");
                }
                const std::string url = web.GetUrl();
                if (url.empty()) {
                    return std::string("设备还没连上 WiFi，连上再问我");
                }
                return "图片上传地址：" + url;
            });

        mcp_server.AddTool("self.photo.next",
            "Show the next photo on the home page.\n"
            "Use when user says: '换一张图', '下一张壁纸', '换个照片', 'next photo'",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (!display_) {
                    return std::string("屏幕还没准备好");
                }
                display_->NotifyUserActivity();
                display_->SwitchToWeatherPage();
                if (!display_->NextHomePhoto()) {
                    return std::string("SD 卡里还没有图片，可以说「打开图片上传」传几张");
                }
                return std::string("已换到下一张");
            });

        // ===== 备忘 / 提醒工具 =====
        // 数据统一由 ReminderManager 管理（独立 reminder_task），这里只做参数校验和话术。
        // NVS 结构: [{"t":"15:00","c":"开会","id":1,"r":0,"due":...}, ...]

        mcp_server.AddTool("self.memo.add",
            "Add a memo / reminder / alarm. It shows on screen, survives reboot, and rings at the given time.\n"
            "Use when user says: '提醒我下午3点开会', '记住买牛奶', '每天早上7点叫我起床', '十分钟后提醒我'.\n"
            "Args://n"
            "  `content`: Short memo text (max ~8 Chinese chars for good display)\n"
            "  `time`: Time label in strict HH:MM 24-hour format, e.g. '07:30'. Empty if no alarm needed.\n"
            "  `repeat`: true = repeat every day at that time (default false).\n"
            "Important://n"
            "  - You MUST convert relative expressions to HH:MM before calling this tool.\n"
            "  - Examples: '5分钟后' -> '21:18', '半小时后' -> '21:43', '晚上8点' -> '20:00'.\n"
            "  - Do NOT pass natural language like '5分钟后' or '明天'.",
            PropertyList({
                Property("content", kPropertyTypeString),
                Property("time", kPropertyTypeString, std::string("")),
                Property("repeat", kPropertyTypeBoolean, false)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto content = properties["content"].value<std::string>();
                auto time_str = properties["time"].value<std::string>();
                bool repeat = false;
                try { repeat = properties["repeat"].value<bool>(); } catch (...) { repeat = false; }

                std::string error;
                auto& reminders = ReminderManager::GetInstance();
                if (!reminders.Add(content, time_str, repeat, &error)) {
                    return error.empty() ? std::string("备忘添加失败") : error;
                }

                ESP_LOGI(TAG, "备忘已添加: 内容=%s, 时间=%s, 每日=%d",
                         content.c_str(), time_str.c_str(), (int)repeat);

                // 新加的这条立刻在目录页上出现，用户看得见"确实记下了"
                if (display_) {
                    display_->SwitchToMemoPage();
                }

                char buf[128];
                if (time_str.empty()) {
                    snprintf(buf, sizeof(buf), "已记录备忘: %s（共%d条）",
                             content.c_str(), reminders.Count());
                } else {
                    snprintf(buf, sizeof(buf), "已设置提醒: %s %s%s（共%d条）",
                             time_str.c_str(), content.c_str(),
                             repeat ? "，每天重复" : "", reminders.Count());
                }
                return std::string(buf);
            });

        mcp_server.AddTool("self.memo.list",
            "List all memos / reminders / alarms still waiting to ring, and show them on screen.\n"
            "Use when user asks: '我有什么待办', '看看备忘', '我的闹钟', 'what do I need to do'\n"
            "Only un-fired ones are listed: once an alarm rings it is removed automatically.",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                // 读列表时顺便把目录页顶到屏幕前面：用户问"我有什么待办"，
                // 屏幕上就该出现那份清单，而不是只有语音在念。
                if (display_) {
                    display_->SwitchToMemoPage();
                }
                return ReminderManager::GetInstance().ListText();
            });

        mcp_server.AddTool("self.memo.page",
            "Turn the memo page to the next / previous page (4 alarms per page).\n"
            "Use when user says: '下一页', '翻页', '上一页', 'next page'.\n"
            "Args:\n"
            "  `direction`: 'next' or 'prev' (default 'next')",
            PropertyList({
                Property("direction", kPropertyTypeString, std::string("next"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!display_) {
                    return std::string("显示器未初始化，暂时无法翻页");
                }

                auto dir = properties["direction"].value<std::string>();
                for (auto& ch : dir) {
                    if (ch >= 'A' && ch <= 'Z') {
                        ch = static_cast<char>(ch - 'A' + 'a');
                    }
                }

                display_->SwitchToMemoPage();   // 翻页前先确保停在目录页
                const bool turned = (dir == "prev" || dir == "previous" || dir == "last")
                                        ? display_->PrevMemoPage()
                                        : display_->NextMemoPage();
                if (!turned) {
                    return std::string("只有一页，没有别的可翻");
                }
                char buf[64];
                snprintf(buf, sizeof(buf), "第 %d 页，共 %d 页", display_->MemoPageIndex() + 1,
                         display_->MemoPageTotal());
                return std::string(buf);
            });

        mcp_server.AddTool("self.memo.done",
            "Mark a memo as done and remove it.\n"
            "Use when user says: '第一条做完了', '删掉买牛奶那条', '完成了开会'\n"
            "Args://n"
            "  `index`: 1-based index. If unsure, call self.memo.list first.",
            PropertyList({
                Property("index", kPropertyTypeInteger, 1, 10)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                const int idx = properties["index"].value<int>();
                std::string removed;
                auto& reminders = ReminderManager::GetInstance();
                if (!reminders.RemoveByIndex(idx, &removed)) {
                    return std::string("序号无效，当前共") + std::to_string(reminders.Count()) + "条备忘";
                }
                ESP_LOGI(TAG, "备忘已完成: %s", removed.c_str());
                return std::string("已完成: ") + removed;
            });

        mcp_server.AddTool("self.memo.clear",
            "Clear ALL memos / reminders / alarms.\n"
            "Use when user says: '清空备忘', '全部删掉', 'clear all memos'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                ReminderManager::GetInstance().Clear();
                ESP_LOGI(TAG, "所有备忘已清除");
                return std::string("所有备忘已清除");
            });

        mcp_server.AddTool("self.reminder.snooze",
            "Snooze the reminder that just rang: remind again after N minutes.\n"
            "Use when user says: '等会儿再说', '五分钟后再提醒我', 'snooze'\n"
            "Args://n"
            "  `minutes`: minutes to wait before ringing again (default 5, range 1-120)",
            PropertyList({
                Property("minutes", kPropertyTypeInteger, 5)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                int minutes = 5;
                try { minutes = properties["minutes"].value<int>(); } catch (...) { minutes = 5; }
                if (minutes < 1) minutes = 1;
                if (minutes > 120) minutes = 120;

                if (!ReminderManager::GetInstance().Snooze(minutes)) {
                    return std::string("当前没有刚响过的提醒，无法稍后提醒");
                }
                char buf[96];
                snprintf(buf, sizeof(buf), "好的，%d 分钟后再提醒你", minutes);
                return std::string(buf);
            });

        // ===== 音乐控制工具（播放由 self.music.play_url 负责）=====
        mcp_server.AddTool("self.music.stop",
            "Stop the current music playback.\n"
            "Use when user says: '停止播放', '别放了', '关掉音乐', 'stop the music'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& app = Application::GetInstance();
                if (!app.IsMusicPlaying()) {
                    return std::string("当前没有在播放音乐");
                }
                app.StopMusicPlayback();
                return std::string("音乐已停止");
            });

        mcp_server.AddTool("self.music.status",
            "Get current music playback status (title, progress).\n"
            "Use when user asks: '现在放的是什么歌', '放到哪了', 'music status'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& app = Application::GetInstance();
                if (!app.IsMusicPlaying()) {
                    return std::string("当前没有在播放音乐。你可以让我搜索并播放一首歌。");
                }
                const uint32_t progress_ms = app.GetMusicProgressMs();
                const uint32_t total_ms = app.GetMusicTotalMs();
                char buf[256];
                snprintf(buf, sizeof(buf), "正在播放《%s》，已播放 %lu:%02lu%s",
                         app.GetCurrentMusicTitle().c_str(),
                         (unsigned long)(progress_ms / 60000),
                         (unsigned long)((progress_ms / 1000) % 60),
                         total_ms > 0 ? "" : "（时长未知）");
                return std::string(buf);
            });

        // ===== 本地曲库工具（SD 卡 /sdcard/music）=====
        mcp_server.AddTool("self.music.search",
            "Search songs stored on the SD card (directory /sdcard/music).\n"
            "Use when user asks: '有没有周杰伦的歌', '搜一下晴天', '本地有什么歌'.\n"
            "Args://n"
            "  `keyword`: part of the song file name; empty string lists all songs",
            PropertyList({
                Property("keyword", kPropertyTypeString, std::string(""))
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto keyword = properties["keyword"].value<std::string>();
                if (!MusicLibrary::Available()) {
                    return std::string("SD 卡未挂载，无法搜索本地音乐。也可以直接给我一个网络音乐 URL。");
                }
                auto results = MusicLibrary::GetInstance().Search(keyword);
                if (results.empty()) {
                    return std::string("本地没有找到匹配的歌曲（目录 /sdcard/music）");
                }
                std::string text = "找到 " + std::to_string(results.size()) + " 首:\n";
                int index = 1;
                for (const auto& path : results) {
                    if (index > 20) {
                        text += "...\n";
                        break;
                    }
                    text += std::to_string(index++) + ". " + MusicLibrary::DisplayName(path) + "\n";
                }
                return text;
            });

        mcp_server.AddTool("self.music.play_local",
            "Play a song stored on the SD card (/sdcard/music). Same-name .lrc lyrics are loaded automatically.\n"
            "Use when user says: '播放晴天', '放一首周杰伦', '播放本地音乐'.\n"
            "Args://n"
            "  `keyword`: song name keyword (first match is played)\n"
            "  `index`: 1-based index returned by self.music.search (0 = unused)",
            PropertyList({
                Property("keyword", kPropertyTypeString, std::string("")),
                Property("index", kPropertyTypeInteger, 0, 0, 999)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                if (!MusicLibrary::Available()) {
                    return std::string("SD 卡未挂载，无法播放本地音乐");
                }

                int index = 0;
                std::string keyword;
                try { index = properties["index"].value<int>(); } catch (...) { index = 0; }
                try { keyword = properties["keyword"].value<std::string>(); } catch (...) { keyword = ""; }

                std::vector<std::string> results;
                if (index > 0) {
                    results = MusicLibrary::GetInstance().All();
                    if (index > static_cast<int>(results.size())) {
                        return std::string("序号超出范围，本地共") + std::to_string(results.size()) + "首";
                    }
                    const std::string picked = results[index - 1];
                    results.assign(1, picked);
                } else {
                    results = MusicLibrary::GetInstance().Search(keyword);
                }

                if (results.empty()) {
                    return std::string("没有找到这首歌");
                }

                const std::string path = results.front();
                const std::string title = MusicLibrary::DisplayName(path);

                auto& app = Application::GetInstance();
                if (!app.PlayMusicFromUrl(path, title, "本地音乐", "", "")) {
                    return std::string("播放失败：文件打不开，或刚刚播过同一首（15 秒内防重播）");
                }
                return std::string("正在播放: ") + title;
            });

        // ===== 语音播放控制：播放/暂停、上下一首、快进 =====
        // 屏幕没有触摸，这几个是唯一入口，所以每个都要能"只靠一句话"跑通。
        mcp_server.AddTool("self.music.pause",
            "Pause the current music playback (keeps the position).\n"
            "Use when user says: '暂停', '先停一下', 'pause'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& app = Application::GetInstance();
                if (!app.IsMusicPlaying()) {
                    return std::string("当前没有在播放音乐");
                }
                if (!app.PauseMusicPlayback()) {
                    return std::string("暂停失败");
                }
                return std::string("已暂停，说「继续播放」可以接着听");
            });

        mcp_server.AddTool("self.music.resume",
            "Resume the paused music from where it stopped.\n"
            "Use when user says: '继续播放', '接着放', 'resume'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& app = Application::GetInstance();
                if (!app.IsMusicPlaying()) {
                    return std::string("当前没有可以继续的歌曲");
                }
                if (!app.ResumeMusicPlayback()) {
                    return std::string("继续播放失败");
                }
                return std::string("继续播放: ") + app.GetCurrentMusicTitle();
            });

        mcp_server.AddTool("self.music.next",
            "Play the next song in the SD card library (/sdcard/music), wrapping around.\n"
            "Use when user says: '下一首', '换一首', '切歌', 'next song'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                if (!MusicLibrary::Available()) {
                    return std::string("SD 卡未挂载，无法切换歌曲");
                }
                auto& app = Application::GetInstance();
                const std::string next = MusicLibrary::GetInstance().Neighbor(app.GetCurrentMusicUrl(), 1);
                if (next.empty()) {
                    return std::string("本地曲库里没有歌曲");
                }
                const std::string title = MusicLibrary::DisplayName(next);
                if (!app.PlayMusicFromUrl(next, title, "本地音乐", "", "", 0, true)) {
                    return std::string("切歌失败，文件可能无法读取");
                }
                return std::string("下一首: ") + title;
            });

        mcp_server.AddTool("self.music.prev",
            "Play the previous song in the SD card library (/sdcard/music), wrapping around.\n"
            "Use when user says: '上一首', '回到上一首', 'previous song'",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                if (!MusicLibrary::Available()) {
                    return std::string("SD 卡未挂载，无法切换歌曲");
                }
                auto& app = Application::GetInstance();
                const std::string prev = MusicLibrary::GetInstance().Neighbor(app.GetCurrentMusicUrl(), -1);
                if (prev.empty()) {
                    return std::string("本地曲库里没有歌曲");
                }
                const std::string title = MusicLibrary::DisplayName(prev);
                if (!app.PlayMusicFromUrl(prev, title, "本地音乐", "", "", 0, true)) {
                    return std::string("切歌失败，文件可能无法读取");
                }
                return std::string("上一首: ") + title;
            });

        mcp_server.AddTool("self.music.seek",
            "Jump within the current song. Only works for local SD card songs.\n"
            "Use when user says: '快进30秒', '后退一点', '跳到1分30秒', '从头开始'.\n"
            "Args:\n"
            "  `seconds`: relative offset in seconds, positive = forward, negative = backward\n"
            "  `absolute_seconds`: absolute position in seconds; >0 takes priority over `seconds`",
            PropertyList({
                Property("seconds", kPropertyTypeInteger, 30, -3600, 3600),
                Property("absolute_seconds", kPropertyTypeInteger, 0, 0, 3600)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto& app = Application::GetInstance();
                if (!app.IsMusicPlaying()) {
                    return std::string("当前没有在播放音乐，无法快进");
                }
                int seconds = 30;
                int absolute = 0;
                try { seconds = properties["seconds"].value<int>(); } catch (...) { seconds = 30; }
                try { absolute = properties["absolute_seconds"].value<int>(); } catch (...) { absolute = 0; }

                int delta_sec = seconds;
                if (absolute > 0) {
                    delta_sec = absolute - static_cast<int>(app.GetMusicProgressMs() / 1000);
                }

                int new_pos_ms = 0;
                if (!app.SeekMusic(delta_sec, &new_pos_ms)) {
                    return std::string("跳转失败：网络音乐不支持跳转，或当前没有可跳转的本地歌曲");
                }
                char buf[96];
                snprintf(buf, sizeof(buf), "已跳到 %d 分 %02d 秒",
                         new_pos_ms / 60000, (new_pos_ms / 1000) % 60);
                return std::string(buf);
            });
    }

    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH, RLCD_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, spi_config);
        
        // 启动天气站数据更新任务
        display_->StartDataUpdateTask();

        // 网页上传图片后立刻刷新主页，不用重启设备
        PhotoWebServer::GetInstance().SetUploadedCallback([this](const std::string& path) {
            auto& photo = HomePhoto::GetInstance();
            photo.Rescan();
            if (display_ == nullptr) {
                return;
            }
            const int idx = photo.IndexOf(path);
            display_->LoadHomePhoto(idx >= 0 ? idx : -1);
        });
    }

    // 启动后台服务：时钟任务 + 提醒任务
    // 这两个任务各自独立，只通过回调和共享快照与 UI 交互，互不持锁
    void StartBackgroundServices() {
        auto& reminders = ReminderManager::GetInstance();

        // 到点提醒：抛回 Application 主任务执行，避免在提醒任务里抢 LVGL 锁
        reminders.SetAlertCallback([](const std::string& content, const std::string& time_label) {
            auto& app = Application::GetInstance();
            app.Schedule([&app, content, time_label]() {
                char buf[160];
                if (time_label.empty()) {
                    snprintf(buf, sizeof(buf), "备忘提醒: %s", content.c_str());
                } else {
                    snprintf(buf, sizeof(buf), "备忘提醒: %s %s", time_label.c_str(), content.c_str());
                }
                // 闹钟响的时候如果停在备忘录页/音乐页，先回主页——
                // 提醒弹窗和语音卡片都画在主页上，留在别的页等于什么都看不见。
                auto display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    display->SwitchToWeatherPage();
                }
                app.Alert("提醒", buf, "happy", Lang::Sounds::OGG_POPUP);
            });
        });

        // 列表变化（新增/删除/触发后自动删除）：同样回主任务刷新右下角列表
        reminders.SetChangeCallback([]() {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                auto display = Board::GetInstance().GetDisplay();
                if (display != nullptr) {
                    static_cast<CustomLcdDisplay*>(display)->RefreshMemoDisplay();
                }
            });
        });

        reminders.Start();
        ClockService::GetInstance().Start();
    }

    uint16_t BatterygetVoltage(void) {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle;
        static adc_cali_handle_t cali_handle = NULL;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
            };
            adc_oneshot_new_unit(&init_config, &adc_handle);
    
            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config);
    
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (initialized) {
            int raw_value = 0;
            int raw_voltage = 0;
            int voltage = 0;
            adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value);
            adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage);
            voltage = raw_voltage * 3;  // 分压电阻比例
            return (uint16_t)voltage;
        }
        return 0;
    }

    uint8_t BatterygetPercent() {
        // 静态变量用于指数移动平均（EMA）滤波，消除 ADC 噪声导致的电量漂移
        static float ema_voltage = 0.0f;    // 平滑后的电压值
        static bool ema_initialized = false;
        const float alpha = 0.1f;           // 平滑系数：越小越平滑（0.1 ≈ 约 10 次采样才能跟上真实变化）

        // 采样 10 次取平均（减少瞬时噪声）
        int voltage = 0;
        for (uint8_t i = 0; i < 10; i++) {
            voltage += BatterygetVoltage();
        }
        voltage /= 10;

        // EMA 滤波：new_value = alpha * 当前值 + (1-alpha) * 历史值
        if (!ema_initialized) {
            ema_voltage = (float)voltage;
            ema_initialized = true;
        } else {
            ema_voltage = alpha * (float)voltage + (1.0f - alpha) * ema_voltage;
        }

        int smoothed = (int)(ema_voltage + 0.5f);  // 四舍五入
        // 电压→百分比映射（抛物线拟合）
        int percent = (-1 * smoothed * smoothed + 9016 * smoothed - 19189000) / 10000;
        percent = (percent > 100) ? 100 : (percent < 0) ? 0 : percent;
        return (uint8_t)percent;
    }

public:
    CustomBoard() : boot_button_(BOOT_BUTTON_GPIO), user_button_(USER_BUTTON_GPIO) {    
        InitializeI2c();
        InitializeSensors();  // 在 I2C 初始化后立即初始化传感器
        InitializeSdcard();   // SD 卡初始化（白噪音播放需要）
        InitializeButtons();     
        InitializeTools();
        InitializeLcdDisplay();
        StartBackgroundServices();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = false;
        discharging = !charging;
        level = (int)BatterygetPercent();
        return true;
    }
};

DECLARE_BOARD(CustomBoard);
