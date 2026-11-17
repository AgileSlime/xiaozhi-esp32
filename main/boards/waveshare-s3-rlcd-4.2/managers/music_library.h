#ifndef __MUSIC_LIBRARY_H__
#define __MUSIC_LIBRARY_H__

#include <mutex>
#include <string>
#include <vector>

// SD 卡本地曲库（单例）
//
// 目录约定：
//   /sdcard/music/xxx.mp3          歌曲
//   /sdcard/music/xxx.lrc          同名歌词（可选，播放时自动加载）
//
// 只做"扫描 + 搜索"，真正播放交给 Application::PlayMusicFromUrl()（传本地路径也可）。
// 扫描结果缓存在内存里，调用 Refresh() 可强制重扫（例如刚插卡/拷完歌）。
class MusicLibrary {
public:
    static MusicLibrary& GetInstance();

    // 重新扫描 SD 卡（自动去重、按文件名排序）
    void Refresh();

    // 返回全部曲目（完整路径）
    std::vector<std::string> All();

    // 按关键字搜索：支持文件名子串匹配（大小写不敏感），空关键字返回全部
    std::vector<std::string> Search(const std::string& keyword);

    // 取纯文件名（去掉目录和扩展名），用作播放标题
    static std::string DisplayName(const std::string& path);

    // 取"当前曲目"的相邻曲目（step=+1 下一首，-1 上一首，循环）。
    // current 为空或不在曲库里时，step>0 返回第一首、step<0 返回最后一首。
    // 曲库为空返回空串。
    std::string Neighbor(const std::string& current, int step);

    // 曲库根目录是否存在
    static bool Available();

    static constexpr const char* kMusicDir = "/sdcard/music";

private:
    MusicLibrary() = default;
    ~MusicLibrary() = default;
    MusicLibrary(const MusicLibrary&) = delete;
    MusicLibrary& operator=(const MusicLibrary&) = delete;

    void RefreshLocked();

    mutable std::mutex mutex_;
    std::vector<std::string> files_;
    bool scanned_ = false;
};

#endif
