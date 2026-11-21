#ifndef __MUSIC_STREAM_H__
#define __MUSIC_STREAM_H__

#include <string>
#include <vector>

// 在线音乐（网易云）中转客户端（单例）
//
// 为什么不直接在设备上连网易云：
//   1. 登录态（MUSIC_U cookie）没法在设备上保存和续期，扫码界面这块 400×300 的
//      1-bit 屏也做不出来；
//   2. 播放直链有时效（几十分钟到几小时），必须每次现取；
//   3. 网易云接口对 UA / Referer / 加密参数有要求，改一次设备端就要重新编译烧录。
//   所以这些事都放在一台常开的电脑 / NAS 上跑（tools/netease-relay/relay.py），
//   设备只认一个约定好的接口，换歌源不动固件。
//
// 约定（一次请求拿全：直链 + 标题 + 歌手 + 歌词 + 时长）：
//   GET {base}/play?kw=<urlencoded>
//   → { "ok": true,
//       "id": 186016, "title": "晴天", "artist": "周杰伦",
//       "url": "http://m8.music.126.net/....mp3",   // 明文 MP3 直链，有时效
//       "lyric": "[00:00.00]...",                    // LRC 原文，可为空
//       "duration_ms": 269000,
//       "reason": "" }                               // ok=false 时给人看的原因
//
//   GET {base}/search?kw=<urlencoded>&limit=8
//   → { "ok": true, "songs": [ {"id":..,"title":..,"artist":..,"duration_ms":..}, ... ] }
//
//   GET {base}/health  → { "ok": true, "logged_in": true }
//
// 中转地址存在 NVS（命名空间 music_stream，键 base），说一句"音乐服务器设为 ..."
// 就能改，不用重新编译。
class MusicStream {
public:
    struct Song {
        std::string id;
        std::string title;
        std::string artist;
        int duration_ms = 0;
    };

    struct Resolved {
        bool ok = false;
        std::string reason;      // ok=false 时给用户/大模型看的原因
        Song song;
        std::string url;         // MP3 明文直链
        std::string lyric;       // LRC 原文
        std::string cover;       // 专辑封面图 URL（可为空）
    };

    static MusicStream& GetInstance();

    // 中转服务地址（形如 http://192.168.1.10:8800）。设置后写进 NVS。
    void SetBase(const std::string& base);
    std::string Base();
    bool Configured();

    // 搜索（只返回元信息，用于"搜一下有哪些歌"）
    bool Search(const std::string& keyword, int limit, std::vector<Song>* out, std::string* err);

    // 一次拿全：搜第一首 → 取直链 → 取歌词
    Resolved Play(const std::string& keyword);

    // 健康检查（判断中转服务在不在、有没有登录）
    bool Health(bool* logged_in, std::string* err);

private:
    MusicStream() = default;
    ~MusicStream() = default;
    MusicStream(const MusicStream&) = delete;
    MusicStream& operator=(const MusicStream&) = delete;

    static constexpr const char* TAG = "MusicStream";
    static constexpr const char* kNvsNs = "music_stream";

    std::string base_;      // 带缓存，避免每次读 NVS
    bool base_loaded_ = false;
};

#endif
