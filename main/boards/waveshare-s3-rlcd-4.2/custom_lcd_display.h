#ifndef __CUSTOM_LCD_DISPLAY_H__
#define __CUSTOM_LCD_DISPLAY_H__

#include <atomic>
#include <driver/gpio.h>
#include "lcd_display.h"
#include "rlcd_driver.h"
#include "managers/sensor_manager.h"
#include "managers/weather_manager.h"

// 主页（400x300, 1-bit 单色 RLCD）布局：
// ┌──────────────────────────────────────────────┐
// │ 23.5°C 48%                    [wifi][bat] 86% │  ← 状态栏 34px
// ├───────────────────────┬──────────────────────┤
// │                       │   11:42              │  ← 时间卡 158x102
// │      图片区            │   11月17日 周一        │
// │      214 x 252        ├──────────────────────┤
// │      （抖动照片）       │   提醒 / 语音识别      │  ← 卡 158x142
// │                       │   07:30 吃药          │
// └───────────────────────┴──────────────────────┘
// 右下角那张卡有两种状态（同一块区域复用）：
//   - 空闲态：最近 3 条要响的提醒，最近一条反白高亮
//   - 语音态：聆听/回答标签 + 用户识别文字（反白块）+ AI 回复
//
// 代码拆分为多个文件：
//   rlcd_driver.h/cc      - RLCD 硬件驱动层（SPI、像素映射、初始化命令）
//   weather_ui.cc          - 天气站 UI 布局（SetupWeatherUI）
//   memo_ui.cc             - 备忘录/闹钟目录页（SetupMemoUI + 分页刷新）
//   music_ui.cc            - 音乐页 UI 布局（SetupMusicUI）
//   data_update_task.cc    - 后台数据更新任务（时间/天气/传感器/电池/WiFi/AI状态）
//   custom_lcd_display.cc  - 核心类（构造/析构/AI适配/备忘录/基类重写）
class CustomLcdDisplay : public LcdDisplay {
private:
    enum DisplayMode {
        MODE_WEATHER = 0,
        MODE_MEMO = 1,
        MODE_MUSIC = 2,
    };
    DisplayMode display_mode_ = MODE_WEATHER;

    // RLCD 硬件驱动（独立模块，负责 SPI 通信和像素操作）
    RlcdDriver *rlcd_ = nullptr;
    lv_obj_t *weather_page_ = nullptr;
    lv_obj_t *memo_page_ = nullptr;
    lv_obj_t *music_page_ = nullptr;

    // ===== 天气站 UI 组件 =====
    // 状态栏（右上角浮动胶囊）
    lv_obj_t *sensor_label_ = nullptr;      // 左上角温湿度标签
    
    // 左侧图片区
    lv_obj_t *photo_img_ = nullptr;         // 抖动后的照片
    lv_obj_t *photo_hint_label_ = nullptr;  // 无图时的提示文字

    // 时间卡片（右上）
    lv_obj_t *time_label_ = nullptr;        // 大字时钟 "14:30"
    lv_obj_t *date_label_ = nullptr;        // 日期 "11月17日 周一"

    // AI 对话卡片（保留成员名，供 SetChatMessage 等基类适配使用）
    lv_obj_t *chat_card_ = nullptr;         // 兼容旧代码，指向备忘卡容器
    lv_obj_t *chat_status_label_ = nullptr; // 隐藏的状态文字（基类方法会写它）
    lv_obj_t *emotion_label_ = nullptr;     // 隐藏的表情文字
    lv_obj_t *emotion_img_ = nullptr;       // 隐藏的表情图片

    // 右下：提醒卡（空闲态）
    lv_obj_t *memo_card_ = nullptr;
    lv_obj_t *memo_count_label_ = nullptr;  // 右上角 "共 3 条"
    lv_obj_t *memo_row_label_[3] = {nullptr, nullptr, nullptr};  // 三行提醒

    // 右下：语音识别卡（语音态，与提醒卡互斥显示）
    lv_obj_t *voice_card_ = nullptr;
    lv_obj_t *voice_badge_ = nullptr;       // "聆听中" 胶囊
    lv_obj_t *voice_badge_label_ = nullptr;
    lv_obj_t *voice_timer_label_ = nullptr; // 右侧录音计时 "00:06"
    lv_obj_t *voice_user_bubble_ = nullptr; // 用户文字反白块
    lv_obj_t *voice_user_label_ = nullptr;
    lv_obj_t *voice_ai_label_ = nullptr;    // AI 回复

    // 表情区（主页不再显示大表情，仅给基类留句柄）
    lv_obj_t *memo_list_label_ = nullptr;     // 兼容旧接口，已不再创建
    lv_obj_t *music_title_label_ = nullptr;   // 音乐页面：歌名
    lv_obj_t *music_artist_label_ = nullptr;  // 音乐页面：歌手

    // ===== 音乐页面（400x300，唱片 + 歌词 + 控制 + 进度）=====
    lv_obj_t *music_disc_ = nullptr;                    // 唱片圆盘
    lv_obj_t *music_pip_[3] = {nullptr, nullptr, nullptr};  // 唱片上的旋转指示点
    lv_obj_t *music_lyric_cont_ = nullptr;              // 歌词容器（整体上下滚动）
    lv_obj_t *music_lyric_line_[3] = {nullptr, nullptr, nullptr};  // 3 行歌词（上/当前/下）
    lv_obj_t *music_lyric_hl_ = nullptr;                // 当前句的反白条（黑底白字）
    lv_obj_t *music_lyric_dot_ = nullptr;               // 旧版圆点，已被反白条取代（保留占位）
    lv_obj_t *music_play_icon_ = nullptr;               // 播放/暂停图标（A8 画布）
    lv_obj_t *music_prev_icon_ = nullptr;               // 上一首图标
    lv_obj_t *music_next_icon_ = nullptr;               // 下一首图标
    lv_obj_t *music_bar_fill_ = nullptr;                // 进度条已播部分
    lv_obj_t *music_bar_knob_ = nullptr;                // 进度条圆点
    lv_obj_t *music_time_cur_ = nullptr;                // 左侧已播时间
    lv_obj_t *music_time_total_ = nullptr;              // 右侧总时长

    int music_pip_angle_ = 0;       // 指示点角度（0..359）
    int music_pip_step_ = 0;        // 每次转多少度
    bool music_is_playing_ = false; // 播放中（决定唱片是否转）
    bool music_is_paused_ = false;  // 暂停中（决定播放键图标）
    int music_last_sec_ = -1;       // 上次刷新进度时的秒数（去重，避免每 90ms 重绘）

    lv_obj_t *music_time_label_ = nullptr;    // 音乐页面：左上角时钟
    lv_obj_t *music_chat_status_label_ = nullptr; // 音乐页面：小智状态文案（隐藏占位）
    lv_obj_t *music_emotion_label_ = nullptr; // 音乐页面：小智情绪标签（隐藏占位）
    lv_obj_t *music_emotion_img_ = nullptr;   // 音乐页面：小智表情图片（隐藏占位）

    // 音乐页内部刷新
    void RedrawPlayIcon();                          // 重画播放/暂停图标
    void UpdateMusicPips();                         // 按角度摆放 3 个指示点
    void UpdateMusicLyricLines(const char* lyric);  // 解析 4 行歌词 + 滚动动画
    static void MusicPipTimerCb(lv_timer_t* t);     // LVGL 定时器（跑在 LVGL 任务里，不用抢锁）

    // 备忘录页内部刷新（需已持锁）
    void RefreshMemoPageInternal();

    // ===== 备忘录页（400x300，闹钟目录：只列没响过的）=====
    lv_obj_t *memo_time_label_ = nullptr;         // 顶栏时间胶囊里的 HH:MM
    lv_obj_t *memo_battery_pct_label_ = nullptr;  // 顶栏电量文字
    lv_obj_t *memo_battery_icon_img_ = nullptr;   // 顶栏电池图标
    lv_obj_t *memo_page_card_ = nullptr;          // 列表卡容器
    lv_obj_t *memo_page_row_[4] = {nullptr, nullptr, nullptr, nullptr};        // 整行容器（首行反白）
    lv_obj_t *memo_page_row_text_[4] = {nullptr, nullptr, nullptr, nullptr};   // 4 行正文
    lv_obj_t *memo_page_row_time_[4] = {nullptr, nullptr, nullptr, nullptr};   // 4 行右侧时间
    lv_obj_t *memo_page_sep_[3] = {nullptr, nullptr, nullptr};                 // 行分隔线（按实际行数显隐）
    lv_obj_t *memo_page_empty_ = nullptr;         // 空列表提示
    lv_obj_t *memo_page_count_label_ = nullptr;   // 页脚 "共 4 项待办"
    lv_obj_t *memo_pager_box_ = nullptr;          // 页脚页码框（只有 1 页时隐藏）
    lv_obj_t *memo_pager_cur_label_ = nullptr;    // 当前页（反白）
    lv_obj_t *memo_pager_tot_label_ = nullptr;    // 总页数

    int memo_page_idx_ = 0;     // 当前页（0 基）
    int memo_page_total_ = 1;   // 总页数（至少 1）
    int memo_page_items_ = 0;   // 未提醒的条目总数

    // 图片图标（不能用基类的 label，因为我们用 lv_image 而不是 Font Awesome 文字）
    lv_obj_t *wifi_icon_img_ = nullptr;
    lv_obj_t *battery_icon_img_ = nullptr;
    lv_obj_t *battery_pct_label_ = nullptr;  // 电池百分比文字
    lv_obj_t *music_wifi_icon_img_ = nullptr;     // 音乐页面状态栏 WiFi 图标
    lv_obj_t *music_battery_icon_img_ = nullptr;  // 音乐页面状态栏电池图标
    lv_obj_t *music_battery_pct_label_ = nullptr; // 音乐页面状态栏电量文字

    // 数据更新任务句柄
    TaskHandle_t update_task_handle_ = nullptr;
    
    // 系统信息滚动标志（为 true 时暂停 DataUpdateTask 更新，避免锁竞争）
    std::atomic<bool> showing_system_info_{false};
    
    // 省电模式：5 分钟无活动后降低刷新频率（1秒 → 5秒）
    std::atomic<bool> power_saving_{false};     // 是否处于省电模式
    uint32_t last_activity_ms_ = 0;             // 上次用户活动的时间（tick 毫秒）
    static const uint32_t IDLE_TIMEOUT_MS = 5 * 60 * 1000;  // 5 分钟无活动进入省电
    static const int NORMAL_REFRESH_MS = 1000;  // 正常刷新间隔 1 秒
    static const int SAVING_REFRESH_MS = 5000;  // 省电刷新间隔 5 秒
    
    // 语音态计时起点（进入 Connecting/Listening 的 tick 毫秒）
    uint32_t voice_start_ms_ = 0;
    // system 提示临时占用语音卡的截止时间（tick 毫秒，0=未占用）
    uint32_t system_card_until_ms_ = 0;
    int last_bottom_mode_ = -1;      // 上一次的右下角模式（0=提醒 1=语音）
    int last_photo_check_day_ = -1;  // 上次换图的日期，用于隔天自动换

    // 上次更新的值（用于避免不必要的 UI 刷新）
    int last_min_ = -1;
    int last_hour_ = -1;
    float last_temp_ = -99.0f;
    float last_humi_ = -99.0f;

    // LVGL flush 回调（将 RGB565 转换为 1-bit 并刷新到 RLCD）
    static void Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);

    // UI 创建（实现在 weather_ui.cc / music_ui.cc / memo_ui.cc）
    void SetupWeatherUI();
    void SetupMusicUI();
    void ApplyDisplayMode();
    
    // 备忘录
    void LoadMemoFromNvs();   // 从 NVS 加载备忘录到 UI
    
    // 数据更新任务（实现在 data_update_task.cc）
    static void DataUpdateTask(void *arg);

public:
    CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy, spi_display_config_t spiconfig, spi_host_device_t spi_host = SPI3_HOST);
    ~CustomLcdDisplay();

    // 获取 RLCD 驱动（供外部调用硬件方法，如对比度调节）
    RlcdDriver* rlcd() const { return rlcd_; }
    
    // 获取 AI 对话标签（供 CustomBoard 设置滚动模式显示系统信息等）
    lv_obj_t* GetChatStatusLabel() const { return chat_status_label_; }
    
    // 系统信息滚动控制（供 CustomBoard 设置标志，避免 DataUpdateTask 锁竞争）
    void SetShowingSystemInfo(bool showing) { showing_system_info_ = showing; }
    
    // 省电模式：记录用户活动，唤醒省电模式
    void NotifyUserActivity();
    bool IsPowerSaving() const { return power_saving_; }
    
    // 重写小智的 AI 显示方法，适配到左下角卡片
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void ClearChatMessages() override;
    
    // 重写状态栏更新（我们用图片图标，不用 Font Awesome 文字）
    virtual void UpdateStatusBar(bool update_all = false) override;
    
    // 重写主题切换（RLCD 单色屏不需要主题切换，避免基类操作不存在的控件导致崩溃）
    virtual void SetTheme(Theme* theme) override;
    virtual void SetMusicInfo(const char* title, const char* artist) override;
    virtual void SetMusicLyric(const char* lyric) override;
    virtual void SetMusicProgress(uint32_t current_ms, uint32_t total_ms) override;
    virtual void SetMusicPlayState(bool playing, bool paused) override;
    virtual void SwitchToMusicPage() override;
    virtual void SwitchToWeatherPage() override;
    
    // 启动数据更新任务（需要在网络连接后调用）
    void StartDataUpdateTask();

    // ===== 主页右下角双态卡片 =====
    // 空闲态显示提醒列表，语音态显示识别文字。由 data_update_task 在设备状态变化时调用。
    void UpdateHomeBottomCard(int device_state);   // device_state 传 DeviceState 枚举值
    void SetVoiceUserText(const char* text);       // 用户识别结果（stt）
    void SetVoiceAiText(const char* text);         // AI 回复（sentence_start）
    void SetVoiceElapsedMs(uint32_t ms);           // 录音时长
    bool IsVoiceCardVisible() const;               // 当前是否处于语音态

    // 主页美图：加载/换一张
    bool LoadHomePhoto(int index = -1);
    bool NextHomePhoto();            // 换下一张（语音「换一张图」）
    
    // 刷新右下角备忘录列表显示（从 NVS 读取后格式化显示）
    void RefreshMemoDisplay();           // 自动获取锁（外部调用用这个）
    void RefreshMemoDisplayInternal();   // 不获取锁（已持锁时用这个，避免死锁）
    void CycleDisplayMode();
    bool IsMusicMode() const { return display_mode_ == MODE_MUSIC; }
    bool IsMemoMode() const { return display_mode_ == MODE_MEMO; }

    // ===== 备忘录页（闹钟目录）=====
    void SetupMemoUI();
    virtual void SwitchToMemoPage() override;   // 切到备忘录页并刷新（自动加锁）
    void RefreshMemoPage();                     // 刷新（自动加锁）
    bool NextMemoPage();                        // 下一页，返回是否真的翻了
    bool PrevMemoPage();                        // 上一页
    int  MemoPageIndex() const { return memo_page_idx_; }   // 0 基
    int  MemoPageTotal() const { return memo_page_total_; }
    int  MemoCount() const { return memo_page_items_; }
};

#endif
