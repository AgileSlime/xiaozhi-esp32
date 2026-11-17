#include "music_library.h"

#include <algorithm>
#include <cctype>

#include <esp_log.h>

#include "sdcard_manager.h"

static const char* TAG = "MusicLibrary";

MusicLibrary& MusicLibrary::GetInstance() {
    static MusicLibrary instance;
    return instance;
}

bool MusicLibrary::Available() {
    return SdcardManager::getInstance().isMounted();
}

void MusicLibrary::Refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    scanned_ = false;
    RefreshLocked();
}

void MusicLibrary::RefreshLocked() {
    files_.clear();
    scanned_ = true;

    auto& sdcard = SdcardManager::getInstance();
    if (!sdcard.isMounted()) {
        ESP_LOGW(TAG, "SD 卡未挂载，本地曲库为空");
        return;
    }

    auto mp3 = sdcard.listFiles(kMusicDir, ".mp3");
    auto wav = sdcard.listFiles(kMusicDir, ".wav");

    files_.insert(files_.end(), mp3.begin(), mp3.end());
    files_.insert(files_.end(), wav.begin(), wav.end());

    // 去重（listFiles 可能因为大小写扩展名重复返回）
    std::sort(files_.begin(), files_.end());
    files_.erase(std::unique(files_.begin(), files_.end()), files_.end());

    ESP_LOGI(TAG, "本地曲库扫描完成: %d 首", static_cast<int>(files_.size()));
}

std::vector<std::string> MusicLibrary::All() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scanned_) {
        RefreshLocked();
    }
    return files_;
}

std::string MusicLibrary::DisplayName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    return name;
}

std::string MusicLibrary::Neighbor(const std::string& current, int step) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scanned_) {
        RefreshLocked();
    }
    if (files_.empty()) {
        return "";
    }

    const int n = static_cast<int>(files_.size());
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (files_[i] == current) {
            idx = i;
            break;
        }
    }

    int next;
    if (idx < 0) {
        next = (step >= 0) ? 0 : n - 1;
    } else {
        // 取模保证负数也能回卷
        next = ((idx + step) % n + n) % n;
    }
    return files_[next];
}

std::vector<std::string> MusicLibrary::Search(const std::string& keyword) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scanned_) {
        RefreshLocked();
    }

    if (keyword.empty()) {
        return files_;
    }

    std::string needle = keyword;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<std::string> matched;
    for (const auto& path : files_) {
        std::string haystack = DisplayName(path);
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (haystack.find(needle) != std::string::npos) {
            matched.push_back(path);
        }
    }
    return matched;
}
