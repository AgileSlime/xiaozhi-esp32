#include "reminder_manager.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/time.h>

#include <cJSON.h>
#include <esp_log.h>

#include "settings.h"

// 时间有效性下限：2024-01-01 00:00:00 UTC
static constexpr time_t kValidEpochFloor = 1704067200;

ReminderManager& ReminderManager::GetInstance() {
    static ReminderManager instance;
    return instance;
}

void ReminderManager::SetAlertCallback(AlertCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    alert_cb_ = std::move(cb);
}

void ReminderManager::SetChangeCallback(ChangeCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    change_cb_ = std::move(cb);
}

bool ReminderManager::Start(uint32_t stack_size, UBaseType_t priority) {
    if (task_handle_ != nullptr) {
        return true;
    }
    BaseType_t created = xTaskCreate(TaskEntry, "reminder_task", stack_size, this, priority, &task_handle_);
    if (created != pdPASS) {
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "提醒任务创建失败（内存不足？）");
        return false;
    }
    ESP_LOGI(TAG, "提醒任务已启动: stack=%u prio=%u", (unsigned)stack_size, (unsigned)priority);
    return true;
}

void ReminderManager::TaskEntry(void* arg) {
    static_cast<ReminderManager*>(arg)->Run();
    vTaskDelete(nullptr);
}

bool ReminderManager::ParseHHMM(const std::string& text, int* hour, int* minute) {
    if (text.size() != 5 || text[2] != ':') {
        return false;
    }
    static const int kDigitIndex[4] = {0, 1, 3, 4};
    for (int k = 0; k < 4; k++) {
        const char ch = text[kDigitIndex[k]];
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    const int hh = (text[0] - '0') * 10 + (text[1] - '0');
    const int mm = (text[3] - '0') * 10 + (text[4] - '0');
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return false;
    }
    if (hour) *hour = hh;
    if (minute) *minute = mm;
    return true;
}

time_t ReminderManager::NextOccurrence(int hour, int minute, time_t now) {
    struct tm now_tm = {};
    localtime_r(&now, &now_tm);

    struct tm target = now_tm;
    target.tm_hour = hour;
    target.tm_min = minute;
    target.tm_sec = 0;

    time_t candidate = mktime(&target);
    // 今天这个点已经过了（留 1 秒余量），就顺延到明天
    if (candidate <= now + 1) {
        target.tm_mday += 1;
        target.tm_hour = hour;
        target.tm_min = minute;
        target.tm_sec = 0;
        candidate = mktime(&target);
    }
    return candidate;
}

void ReminderManager::LoadLocked() {
    if (loaded_) {
        return;
    }

    items_.clear();
    next_id_ = 1;

    Settings settings(NVS_NAMESPACE, false);
    std::string json = settings.GetString(NVS_KEY, "[]");

    cJSON* arr = cJSON_Parse(json.c_str());
    if (arr != nullptr && cJSON_IsArray(arr)) {
        const int total = cJSON_GetArraySize(arr);
        for (int i = 0; i < total && static_cast<int>(items_.size()) < kMaxItems; i++) {
            cJSON* obj = cJSON_GetArrayItem(arr, i);
            if (obj == nullptr) {
                continue;
            }

            cJSON* jc = cJSON_GetObjectItem(obj, "c");
            cJSON* jt = cJSON_GetObjectItem(obj, "t");
            cJSON* jid = cJSON_GetObjectItem(obj, "id");
            cJSON* jr = cJSON_GetObjectItem(obj, "r");
            cJSON* jdue = cJSON_GetObjectItem(obj, "due");

            ReminderItem item;
            item.content = (jc && cJSON_IsString(jc)) ? jc->valuestring : "";
            item.time_label = (jt && cJSON_IsString(jt)) ? jt->valuestring : "";
            item.id = (jid && cJSON_IsNumber(jid)) ? static_cast<int>(jid->valuedouble) : (i + 1);
            item.daily = (jr && cJSON_IsNumber(jr)) ? (jr->valuedouble != 0) : false;
            item.due = (jdue && cJSON_IsNumber(jdue)) ? static_cast<time_t>(jdue->valuedouble) : 0;

            if (item.time_label.empty() || !ParseHHMM(item.time_label, &item.hour, &item.minute)) {
                item.hour = -1;
                item.minute = -1;
                item.due = 0;
            }

            if (item.id >= next_id_) {
                next_id_ = item.id + 1;
            }
            items_.push_back(item);
        }
    }
    if (arr != nullptr) {
        cJSON_Delete(arr);
    }

    loaded_ = true;
    ESP_LOGI(TAG, "备忘已加载: %d 条", static_cast<int>(items_.size()));
}

void ReminderManager::SaveLocked() {
    cJSON* arr = cJSON_CreateArray();
    if (arr == nullptr) {
        return;
    }

    for (const auto& item : items_) {
        cJSON* obj = cJSON_CreateObject();
        if (obj == nullptr) {
            continue;
        }
        // t/c 字段顺序与旧版一致，旧 UI 直接读这两个字段，无需改动
        cJSON_AddStringToObject(obj, "t", item.time_label.c_str());
        cJSON_AddStringToObject(obj, "c", item.content.c_str());
        cJSON_AddNumberToObject(obj, "id", item.id);
        cJSON_AddNumberToObject(obj, "r", item.daily ? 1 : 0);
        cJSON_AddNumberToObject(obj, "due", static_cast<double>(item.due));
        cJSON_AddItemToArray(arr, obj);
    }

    char* text = cJSON_PrintUnformatted(arr);
    if (text != nullptr) {
        Settings settings(NVS_NAMESPACE, true);
        if (items_.empty()) {
            settings.EraseKey(NVS_KEY);
        } else {
            settings.SetString(NVS_KEY, text);
        }
        cJSON_free(text);
    }
    cJSON_Delete(arr);
}

void ReminderManager::RecomputeDueLocked(time_t now) {
    if (now < kValidEpochFloor) {
        return;   // 系统时间还没同步，先不算 due（等 NTP 成功后 due==0 会自动补算）
    }

    bool changed = false;
    for (auto& item : items_) {
        if (item.hour < 0) {
            continue;
        }

        // 加备忘时系统时间还无效 → due 记为 0，这里补算
        if (item.due == 0) {
            item.due = NextOccurrence(item.hour, item.minute, now);
            changed = true;
            continue;
        }

        // 每日重复：错过就滚到下一个周期
        if (item.daily && item.due < now) {
            item.due = NextOccurrence(item.hour, item.minute, now);
            changed = true;
        }

        // 单次提醒若已过期，保持原样，交给 FireDue 立即补触发一次（不会丢）
    }

    if (changed) {
        SaveLocked();
    }
}

int ReminderManager::FireDue() {
    struct Fired {
        std::string content;
        std::string time_label;
    };
    std::vector<Fired> fired;
    AlertCallback alert_cb;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            LoadLocked();
        }

        const time_t now = time(nullptr);
        if (now < kValidEpochFloor) {
            return 0;
        }

        for (auto it = items_.begin(); it != items_.end();) {
            if (it->due == 0 || it->due > now) {
                ++it;
                continue;
            }

            fired.push_back({it->content, it->time_label});
            last_fired_content_ = it->content;
            last_fired_time_ = it->time_label;

            if (it->daily && it->hour >= 0) {
                it->due = NextOccurrence(it->hour, it->minute, now);
                ++it;
            } else {
                it = items_.erase(it);
            }
            changed = true;
        }

        if (changed) {
            SaveLocked();
        }

        alert_cb = alert_cb_;
    }

    if (!changed) {
        return 0;
    }

    for (const auto& f : fired) {
        ESP_LOGI(TAG, "触发提醒: [%s] %s", f.time_label.c_str(), f.content.c_str());
        if (alert_cb) {
            alert_cb(f.content, f.time_label);
        }
    }

    NotifyChange();
    return static_cast<int>(fired.size());
}

void ReminderManager::NotifyChange() {
    ChangeCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = change_cb_;
    }
    if (cb) {
        cb();
    }
}

bool ReminderManager::Add(const std::string& content, const std::string& hhmm, bool daily, std::string* error) {
    int hour = -1;
    int minute = -1;

    if (!hhmm.empty() && !ParseHHMM(hhmm, &hour, &minute)) {
        if (error) *error = "时间格式无效：请使用 HH:MM（24 小时制），例如 07:30、15:00";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            LoadLocked();
        }
        if (static_cast<int>(items_.size()) >= kMaxItems) {
            if (error) *error = "备忘已满（最多 10 条），请先完成或清除一些";
            return false;
        }

        ReminderItem item;
        item.id = next_id_++;
        item.content = content;
        item.time_label = hhmm;
        item.hour = hour;
        item.minute = minute;
        item.daily = daily;

        const time_t now = time(nullptr);
        if (hour >= 0) {
            item.due = (now >= kValidEpochFloor) ? NextOccurrence(hour, minute, now) : 0;
        }

        items_.push_back(item);
        SaveLocked();
    }

    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);   // 立刻唤醒提醒任务重算睡眠时长
    }
    NotifyChange();
    return true;
}

bool ReminderManager::RemoveByIndex(int index_1based, std::string* removed) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            LoadLocked();
        }
        const int total = static_cast<int>(items_.size());
        if (index_1based < 1 || index_1based > total) {
            return false;
        }
        if (removed) {
            *removed = items_[index_1based - 1].content;
        }
        items_.erase(items_.begin() + (index_1based - 1));
        SaveLocked();
    }
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    NotifyChange();
    return true;
}

void ReminderManager::Clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            LoadLocked();
        }
        items_.clear();
        SaveLocked();
    }
    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    NotifyChange();
}

bool ReminderManager::Snooze(int minutes) {
    if (minutes <= 0) {
        minutes = 5;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!loaded_) {
            LoadLocked();
        }
        if (last_fired_content_.empty()) {
            return false;
        }
        if (static_cast<int>(items_.size()) >= kMaxItems) {
            return false;
        }

        const time_t now = time(nullptr);

        ReminderItem item;
        item.id = next_id_++;
        item.content = last_fired_content_;
        item.daily = false;
        item.due = now + static_cast<time_t>(minutes) * 60;

        const time_t snoozed = item.due;
        struct tm snooze_tm = {};
        localtime_r(&snoozed, &snooze_tm);
        char label[8];
        snprintf(label, sizeof(label), "%02d:%02d", snooze_tm.tm_hour, snooze_tm.tm_min);
        item.time_label = label;
        item.hour = snooze_tm.tm_hour;
        item.minute = snooze_tm.tm_min;

        items_.push_back(item);
        SaveLocked();
    }

    if (task_handle_ != nullptr) {
        xTaskNotifyGive(task_handle_);
    }
    NotifyChange();
    return true;
}

std::vector<ReminderItem> ReminderManager::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        const_cast<ReminderManager*>(this)->LoadLocked();
    }
    return items_;
}

std::string ReminderManager::ListText() const {
    const auto items = List();
    if (items.empty()) {
        return "当前没有备忘";
    }

    std::string result = "当前备忘列表:\n";
    int index = 1;
    for (const auto& item : items) {
        result += std::to_string(index++) + ". ";
        if (!item.time_label.empty()) {
            result += "[" + item.time_label + "] ";
        }
        result += item.content;
        if (item.daily) {
            result += "（每天）";
        }
        result += "\n";
    }
    return result;
}

int ReminderManager::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        const_cast<ReminderManager*>(this)->LoadLocked();
    }
    return static_cast<int>(items_.size());
}

uint32_t ReminderManager::SecondsToNext() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loaded_) {
        const_cast<ReminderManager*>(this)->LoadLocked();
    }

    const time_t now = time(nullptr);
    time_t soonest = 0;
    for (const auto& item : items_) {
        if (item.due != 0 && (soonest == 0 || item.due < soonest)) {
            soonest = item.due;
        }
    }
    if (soonest == 0) {
        return UINT32_MAX;
    }
    if (soonest <= now) {
        return 0;
    }
    return static_cast<uint32_t>(soonest - now);
}

void ReminderManager::Run() {
    vTaskDelay(pdMS_TO_TICKS(2000));   // 等系统时间从 RTC 恢复完成

    while (true) {
        uint32_t wait_ms = kMaxSleepMs;
        int fired = 0;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!loaded_) {
                LoadLocked();
            }

            const time_t now = time(nullptr);
            if (now >= kValidEpochFloor) {
                RecomputeDueLocked(now);

                time_t soonest = 0;
                for (const auto& item : items_) {
                    if (item.due != 0 && (soonest == 0 || item.due < soonest)) {
                        soonest = item.due;
                    }
                }

                if (soonest != 0) {
                    long delta = static_cast<long>(soonest - now);
                    if (delta < 0) delta = 0;
                    if (delta > static_cast<long>(kMaxSleepMs / 1000)) {
                        delta = static_cast<long>(kMaxSleepMs / 1000);
                    }
                    wait_ms = static_cast<uint32_t>(delta) * 1000 + 50;
                }
            } else {
                wait_ms = kInvalidTimeSleepMs;
            }
        }

        // 有新备忘 / 删除 / 清空时会被 Add() 立刻唤醒
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));

        fired = FireDue();

        // 每秒级检查没必要：FireDue 内部已按 due 判断，这里只做"到点即触发"
        if (fired > 0) {
            ESP_LOGI(TAG, "本轮触发 %d 条提醒", fired);
        }
    }
}
