#include "clock_service.h"

#include <cstdlib>
#include <sys/time.h>

#include <esp_log.h>

#include "application.h"
#include "../secret_config.h"
#include "sensor_manager.h"

// 时间有效性下限：2024-01-01 00:00:00 UTC
static constexpr time_t kValidEpochFloor = 1704067200;

ClockService& ClockService::GetInstance() {
    static ClockService instance;
    return instance;
}

void ClockService::AddListener(Listener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.push_back(std::move(listener));
}

bool ClockService::Start(uint32_t stack_size, UBaseType_t priority) {
    if (task_handle_ != nullptr) {
        return true;
    }

    // 时区必须在任何 localtime/mktime 之前设置
    setenv("TZ", TIMEZONE_STRING, 1);
    tzset();

    // 先发一拍：让启动阶段（NTP 还没开始时）也有有效时间可读，避免 UI 显示 00:00
    PublishTick(true);

    BaseType_t created = xTaskCreate(TaskEntry, "clock_task", stack_size, this, priority, &task_handle_);
    if (created != pdPASS) {
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "时钟任务创建失败（内存不足？）");
        return false;
    }

    ESP_LOGI(TAG, "时钟任务已启动: stack=%u prio=%u tick=%ums",
             (unsigned)stack_size, (unsigned)priority, (unsigned)kTickMs);
    return true;
}

void ClockService::RequestSync() {
    sync_requested_.store(true);
    ESP_LOGI(TAG, "已请求 NTP 同步");
}

ClockTick ClockService::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_tick_;
}

void ClockService::TaskEntry(void* arg) {
    static_cast<ClockService*>(arg)->Run();
    vTaskDelete(nullptr);
}

bool ClockService::NetworkReady() const {
    auto state = Application::GetInstance().GetDeviceState();
    return state != kDeviceStateStarting &&
           state != kDeviceStateWifiConfiguring &&
           state != kDeviceStateUnknown;
}

bool ClockService::SyncOnce() {
    ESP_LOGI(TAG, "开始 NTP 同步（服务器 %s）", NTP_SERVER);

    // syncNtpTime 内部自带 5 次 x 2 秒的等待，最长阻塞约 10 秒
    SensorManager::getInstance().syncNtpTime();

    time_t now = 0;
    time(&now);
    struct tm ti = {};
    localtime_r(&now, &ti);

    if (now >= kValidEpochFloor) {
        last_valid_epoch_ = now;
        ESP_LOGI(TAG, "NTP 同步成功: %04d-%02d-%02d %02d:%02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        return true;
    }

    ESP_LOGW(TAG, "NTP 同步失败，当前时间无效（年份=%d）", ti.tm_year + 1900);
    return false;
}

bool ClockService::RestoreFromRtcIfJumped(time_t now) {
    if (!synced_.load() || last_valid_epoch_ <= 0) {
        return false;
    }

    const long delta = static_cast<long>(now - last_valid_epoch_);
    if (delta >= -kTimeJumpThresholdSec && delta <= kTimeJumpThresholdSec) {
        last_valid_epoch_ = now;
        return false;
    }

    ESP_LOGW(TAG, "检测到系统时间跳变 %ld 秒，尝试从硬件 RTC 恢复", delta);

    struct tm rtc_tm = {};
    SensorManager::getInstance().getRtcTime(&rtc_tm);
    const time_t rtc_epoch = mktime(&rtc_tm);
    if (rtc_epoch < kValidEpochFloor) {
        ESP_LOGW(TAG, "RTC 时间无效，放弃恢复，以当前系统时间为准");
        last_valid_epoch_ = now;
        return false;
    }

    struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    last_valid_epoch_ = rtc_epoch;

    struct tm fixed = {};
    localtime_r(&rtc_epoch, &fixed);
    ESP_LOGI(TAG, "已从 RTC 恢复时间: %02d:%02d", fixed.tm_hour, fixed.tm_min);
    return true;
}

void ClockService::PublishTick(bool force_refresh) {
    ClockTick tick;
    time(&tick.epoch);
    localtime_r(&tick.epoch, &tick.tm_local);
    tick.synced = synced_.load();

    // 分钟变化：跨小时也要算（用 epoch/60 判断最稳）
    tick.minute_changed = force_refresh ||
                          last_epoch_ == 0 ||
                          (tick.epoch / 60 != last_epoch_ / 60);
    tick.day_changed = (last_yday_ < 0) || (tick.tm_local.tm_yday != last_yday_);

    last_epoch_ = tick.epoch;
    last_yday_ = tick.tm_local.tm_yday;

    std::vector<Listener> listeners_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_tick_ = tick;
        listeners_copy = listeners_;
    }

    // 注意：回调在 ClockTask 上下文执行，必须快速返回
    for (auto& listener : listeners_copy) {
        if (listener) {
            listener(tick);
        }
    }
}

void ClockService::Run() {
    // 启动时先让系统跑起来，避免与 WiFi 初始化、音频初始化抢 CPU
    vTaskDelay(pdMS_TO_TICKS(2000));

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        const uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // ===== 连续 idle 计时（网络请求守卫）=====
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            if (idle_since_ms_ == 0) {
                idle_since_ms_ = now_ms;
            }
        } else {
            idle_since_ms_ = 0;
        }
        const bool idle_long_enough = (idle_since_ms_ != 0) &&
                                      (now_ms - idle_since_ms_ >= kIdleGuardMs);

        // ===== 是否需要同步 =====
        bool should_sync = false;
        if (sync_enabled_.load() && NetworkReady() && idle_long_enough) {
            if (sync_requested_.load()) {
                should_sync = true;
            } else if (!synced_.load() && retry_count_ < kMaxRetries) {
                should_sync = true;
            } else if (synced_.load() && last_sync_ms_ != 0 &&
                       (now_ms - last_sync_ms_ >= kResyncIntervalMs)) {
                should_sync = true;
            }
            // 退避窗口内不发请求（关键：不再 vTaskDelay 卡住整个时钟任务）
            if (should_sync && next_sync_at_ms_ != 0 && now_ms < next_sync_at_ms_) {
                should_sync = false;
            }
        }

        if (should_sync) {
            sync_requested_.store(false);
            const bool ok = SyncOnce();
            const uint32_t after_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

            if (ok) {
                synced_.store(true);
                retry_count_ = 0;
                retry_delay_ms_ = 1000;
                next_sync_at_ms_ = 0;
                last_sync_ms_ = after_ms;
                last_epoch_ = 0;  // 强制下一拍标记 minute_changed，UI 立即刷新
            } else {
                retry_count_++;
                next_sync_at_ms_ = after_ms + retry_delay_ms_;
                retry_delay_ms_ = (retry_delay_ms_ * 2 > kMaxRetryDelayMs)
                                      ? kMaxRetryDelayMs
                                      : retry_delay_ms_ * 2;
                if (retry_count_ >= kMaxRetries) {
                    ESP_LOGE(TAG, "NTP 已连续失败 %d 次，暂停自动重试（使用 RTC 时间，可按键重试）",
                             retry_count_);
                } else {
                    ESP_LOGW(TAG, "NTP 失败 %d/%d，%u 秒后重试",
                             retry_count_, kMaxRetries, (unsigned)(retry_delay_ms_ / 2 / 1000));
                }
            }
        }

        // ===== 时间跳变保护 + 派发节拍 =====
        time_t now = 0;
        time(&now);
        bool forced = RestoreFromRtcIfJumped(now);
        PublishTick(forced);

        // 若上一轮被 NTP 阻塞较久，重新对齐节拍，避免"补拍风暴"
        const TickType_t now_ticks = xTaskGetTickCount();
        if ((now_ticks - last_wake) > pdMS_TO_TICKS(3000)) {
            last_wake = now_ticks;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kTickMs));
    }
}
