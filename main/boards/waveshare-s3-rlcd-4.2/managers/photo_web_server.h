#ifndef __PHOTO_WEB_SERVER_H__
#define __PHOTO_WEB_SERVER_H__

#include <string>
#include <functional>
#include <atomic>
#include <cstdint>
#include <esp_http_server.h>

// 图片上传网页服务（局域网 HTTP）
//
// 为什么用 HTTP 而不是蓝牙 / USB：
//   - 设备本身已经常连 WiFi，手机和电脑都在同一个局域网，不需要任何额外硬件
//   - 浏览器直接打开即可上传，安卓 / iOS / Windows / macOS 通吃，不用装 App
//   - BLE 吞吐只有 10KB/s 量级，一张图要十几秒，还要装专用 App
//   - USB MSC 要独占 SD 卡，而且和串口日志争同一个 USB 口
//
// 页面能力：
//   GET  /       上传页面（选图/拖图，显示当前是否已有自定义图）
//   GET  /list   状态 JSON：{"override":bool,"sd_count":n}
//   POST /upload 原始二进制 JPEG/PNG → 内存解码 → 抖成 1bpp 直接显示，不落 SD
//   POST /delete 清掉内存里的自定义图，回到默认壁纸
//
// 上传采用 raw body（不是 multipart），服务端只做「循环 recv → 攒进 PSRAM」，
// 不需要解析 multipart 边界，也不在 httpd 任务栈上放大数组。
class PhotoWebServer {
public:
    // 上传成功后回调（参数是写入的完整路径），供 board 刷新主页显示
    using UploadedCallback = std::function<void(const std::string& path)>;

    static PhotoWebServer& GetInstance();

    // 启动服务，返回是否成功（重复调用安全）
    bool Start();

    // 停止服务
    void Stop();

    bool IsRunning() const { return server_ != nullptr; }

    // 返回 "http://192.168.x.x" 形式的访问地址；没连上 WiFi 时返回空串
    std::string GetUrl() const;

    void SetUploadedCallback(UploadedCallback cb) { on_uploaded_ = std::move(cb); }

    // ===== WiFi 省电与网页速度的冲突 =====
    // 设备空闲时 WiFi 跑在 LOW_POWER(MAX_MODEM)，DTIM 被拉长到 1.02 秒，
    // 射频每 1 秒才醒一次收包。一个 5.6KB 的页面要发好几轮，
    // 累计四五秒才出来，手机浏览器早就超时了——用户看到的就是"网站打不开"。
    // 所以有网页请求时临时切到 PERFORMANCE，闲置一段时间再由后台任务收回。
    static constexpr uint32_t kActiveWindowMs = 15000;

    // 最近一次收到请求距今多久（毫秒）；从没收到过返回 UINT32_MAX
    uint32_t MsSinceActive() const;

    // 是否仍在活跃窗口内（打包上传时靠它判断别中途降速）
    bool IsActiveWindow() const { return MsSinceActive() < kActiveWindowMs; }

    // 是否已经把 WiFi 拉到高性能（后台任务据此决定要不要收回省电）
    bool WifiBoosted() const { return wifi_boosted_.load(); }

    // 收回高性能，还回省电模式
    void ReleaseWifiBoost();

    static constexpr const char* kPhotoDir = "/sdcard/photos";
    static constexpr size_t kMaxUploadBytes = 8 * 1024 * 1024;  // 单张上限 8MB

private:
    PhotoWebServer() = default;
    ~PhotoWebServer();
    PhotoWebServer(const PhotoWebServer&) = delete;
    PhotoWebServer& operator=(const PhotoWebServer&) = delete;

    static esp_err_t IndexHandler(httpd_req_t* req);
    static esp_err_t ListHandler(httpd_req_t* req);
    static esp_err_t UploadHandler(httpd_req_t* req);
    static esp_err_t DeleteHandler(httpd_req_t* req);
    static esp_err_t CmdHandler(httpd_req_t* req);
    // POST /reset：删掉常驻壁纸，主页回到内置默认图 / SD 卡
    static esp_err_t ResetHandler(httpd_req_t* req);

    // 每个 handler 入口调一次：记时间戳，必要时把 WiFi 拉到高性能
    static void MarkActive();

    // 服务刚启动、页面还没被访问过时，last_active_ms_ 用这个值表示"从没活跃过"
    static constexpr uint32_t kNeverActive = 0xFFFFFFFFu;

    // 从 query 里取 name 参数并做安全过滤，成功返回 true
    static bool GetSafeName(httpd_req_t* req, char* out, size_t out_len);

    void NotifyUploaded(const std::string& path) {
        if (on_uploaded_) {
            on_uploaded_(path);
        }
    }

    httpd_handle_t server_ = nullptr;
    UploadedCallback on_uploaded_;

    // httpd 在自己的任务里跑，后台任务要读这两个，用原子量避免撕裂
    std::atomic<uint32_t> last_active_ms_{kNeverActive};
    std::atomic<bool> wifi_boosted_{false};
};

#endif  // __PHOTO_WEB_SERVER_H__
