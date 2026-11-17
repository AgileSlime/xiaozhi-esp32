#ifndef __REMINDER_MANAGER_H__
#define __REMINDER_MANAGER_H__

#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 一条备忘 / 提醒
struct ReminderItem {
    int id = 0;
    std::string content;
    std::string time_label;   // "HH:MM"；空串表示只记事、不响铃
    int hour = -1;
    int minute = -1;
    time_t due = 0;           // 下次触发时间（epoch 秒），0 表示不触发
    bool daily = false;       // 是否每天重复
};

// 备忘 & 闹钟管理器（单例）
//
// 为什么单独一个任务：
//   原来"检查备忘/触发闹钟"塞在 DataUpdateTask 的 1 秒循环里，到点后直接调用
//   Application::Alert()，而 Alert 会走 Application 主任务 + LVGL 锁，
//   等于在 UI 刷新任务里同步等另一把锁 —— 这是典型的优先级反转 + 锁竞争。
//
// 现在的分工：
//   - ReminderTask：唯一负责"到点判断 + 触发"，睡到下一个 due 才醒（ulTaskNotifyTake），
//     不轮询、不占 CPU；不直接碰 LVGL。
//   - 触发时通过 AlertCallback 抛回 Application 主任务（board 里用 app.Schedule 包装）。
//
// 存储：
//   NVS namespace "memo"，key "items"，JSON 数组：
//     [{"t":"15:00","c":"开会","id":1,"r":0,"due":1730000000}, ...]
//   其中 t/c 字段与旧版完全一致，所以现有 UI（RefreshMemoDisplayInternal）无需改动即可显示。
class ReminderManager {
public:
    static ReminderManager& GetInstance();

    // 触发回调（在 ReminderTask 上下文执行，禁止直接操作 LVGL）
    using AlertCallback = std::function<void(const std::string& content, const std::string& time_label)>;
    // 列表变化回调（增删改、触发后自动删除都会调用；同样在 ReminderTask 上下文）
    using ChangeCallback = std::function<void()>;

    void SetAlertCallback(AlertCallback cb);
    void SetChangeCallback(ChangeCallback cb);

    bool Start(uint32_t stack_size = 5120, UBaseType_t priority = 3);
    bool IsRunning() const { return task_handle_ != nullptr; }

    // 新增一条备忘。hhmm 必须是 "HH:MM" 或空串；daily=true 表示每天重复。
    // 成功返回 true，error 里带回失败原因。
    bool Add(const std::string& content, const std::string& hhmm, bool daily, std::string* error);

    // 按 1-based 序号删除（与语音里"第一条/第二条"对应）
    bool RemoveByIndex(int index_1based, std::string* removed);

    // 清空全部
    void Clear();

    // 稍后提醒：把最近一次触发过的提醒顺延 minutes 分钟
    bool Snooze(int minutes);

    std::vector<ReminderItem> List() const;
    std::string ListText() const;   // 供 MCP 工具直接返回给大模型
    int Count() const;

    // 距离下一条提醒还有多少秒；没有则返回 UINT32_MAX
    uint32_t SecondsToNext() const;

    // 时间格式校验（供外部工具复用）
    static bool ParseHHMM(const std::string& text, int* hour, int* minute);

private:
    ReminderManager() = default;
    ~ReminderManager() = default;
    ReminderManager(const ReminderManager&) = delete;
    ReminderManager& operator=(const ReminderManager&) = delete;

    static void TaskEntry(void* arg);
    void Run();

    void LoadLocked();                                  // 需持锁
    void SaveLocked();                                  // 需持锁
    void RecomputeDueLocked(time_t now);                // 需持锁
    int  FireDue();                                     // 返回本次触发条数
    void NotifyChange();

    static time_t NextOccurrence(int hour, int minute, time_t now);

    static constexpr const char* TAG = "ReminderManager";
    static constexpr const char* NVS_NAMESPACE = "memo";
    static constexpr const char* NVS_KEY = "items";
    static constexpr int kMaxItems = 10;
    static constexpr uint32_t kMaxSleepMs = 60000;   // 最长睡 60 秒就醒一次（防系统时间被改）
    static constexpr uint32_t kInvalidTimeSleepMs = 30000;

    mutable std::mutex mutex_;
    std::vector<ReminderItem> items_;
    bool loaded_ = false;
    int next_id_ = 1;

    AlertCallback alert_cb_;
    ChangeCallback change_cb_;

    TaskHandle_t task_handle_ = nullptr;

    // 最近一次触发的快照，供 Snooze 使用
    std::string last_fired_content_;
    std::string last_fired_time_;
};

#endif
