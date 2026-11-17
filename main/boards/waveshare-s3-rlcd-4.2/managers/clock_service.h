#ifndef __CLOCK_SERVICE_H__
#define __CLOCK_SERVICE_H__

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 时钟节拍快照：时钟任务每秒产生一次，其它任务只读
struct ClockTick {
    time_t epoch = 0;             // UTC epoch 秒
    struct tm tm_local = {};      // 本地时间（已按 TIMEZONE_STRING 换算）
    bool synced = false;          // 是否已成功做过 NTP 同步
    bool minute_changed = false;  // 与上一拍相比分钟是否变化
    bool day_changed = false;     // 与上一拍相比日期是否变化
};

// 时钟服务（单例）
//
// 设计目的（FreeRTOS 视角）：
//   把"时间"从 DataUpdateTask 的 1 秒大循环里拆出来，成为一个只做时间的独立任务。
//   - ClockTask：唯一的时间源，负责 NTP 同步 / 退避重试 / 24h 校准 / 时间跳变保护
//   - 其它任务（UI 刷新、闹钟、备忘录）只通过 Snapshot() 读快照，或注册 Listener 被动接收
//
// 线程约定：
//   1. Listener 在 ClockTask 的上下文里被同步调用，必须快速返回；
//      绝对禁止在 Listener 里拿 LVGL 锁、做网络请求或长时间阻塞。
//      需要操作 UI 时，请在 Listener 里 xTaskNotifyGive 唤醒自己的任务。
//   2. Snapshot() 可以在任意任务/任意时刻调用，内部加锁，开销是一次小结构体拷贝。
class ClockService {
public:
    static ClockService& GetInstance();

    using Listener = std::function<void(const ClockTick&)>;

    // 注册节拍监听（建议在 Start() 之前注册；运行期注册也安全）
    void AddListener(Listener listener);

    // 启动时钟任务（重复调用无效）
    bool Start(uint32_t stack_size = 4096, UBaseType_t priority = 2);

    // 请求尽快做一次 NTP 同步（联网成功、按键刷新等场景）
    void RequestSync();

    // 允许/禁止自动 NTP（调试用）
    void SetSyncEnabled(bool enabled) { sync_enabled_.store(enabled); }

    // 读取最近一次节拍快照
    ClockTick Snapshot() const;

    bool IsSynced() const { return synced_.load(); }
    bool IsRunning() const { return task_handle_ != nullptr; }

    // 节拍周期（毫秒）
    static constexpr uint32_t kTickMs = 1000;

private:
    ClockService() = default;
    ~ClockService() = default;
    ClockService(const ClockService&) = delete;
    ClockService& operator=(const ClockService&) = delete;

    static void TaskEntry(void* arg);
    void Run();

    // 网络是否已连通（排除配网/启动等前期状态）
    bool NetworkReady() const;

    // 执行一次 NTP 同步，返回时间是否有效
    bool SyncOnce();

    // 系统时间被外部篡改时从硬件 RTC 恢复
    bool RestoreFromRtcIfJumped(time_t now);

    // 生成并派发节拍
    void PublishTick(bool force_refresh);

    static constexpr const char* TAG = "ClockService";

    static constexpr uint32_t kIdleGuardMs = 5000;                   // 连续 idle 5 秒才允许联网同步
    static constexpr uint32_t kResyncIntervalMs = 24 * 3600 * 1000;  // 24 小时校准一次
    static constexpr int kMaxRetries = 5;                            // 首同步最多重试 5 次
    static constexpr uint32_t kMaxRetryDelayMs = 16000;              // 退避上限 16 秒
    static constexpr long kTimeJumpThresholdSec = 7200;              // 超过 2 小时视为时间被篡改

    TaskHandle_t task_handle_ = nullptr;

    mutable std::mutex mutex_;
    ClockTick last_tick_;
    std::vector<Listener> listeners_;

    std::atomic<bool> synced_{false};
    std::atomic<bool> sync_requested_{false};
    std::atomic<bool> sync_enabled_{true};

    // ↓↓↓ 以下成员只允许 ClockTask 访问，无需加锁 ↓↓↓
    uint32_t last_sync_ms_ = 0;
    uint32_t next_sync_at_ms_ = 0;
    uint32_t retry_delay_ms_ = 1000;
    int retry_count_ = 0;
    time_t last_epoch_ = 0;
    int last_yday_ = -1;
    time_t last_valid_epoch_ = 0;
    uint32_t idle_since_ms_ = 0;
};

#endif
