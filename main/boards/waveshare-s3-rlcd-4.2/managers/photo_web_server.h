#ifndef __PHOTO_WEB_SERVER_H__
#define __PHOTO_WEB_SERVER_H__

#include <string>
#include <functional>
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
//   GET  /             上传页面（选择/拖拽图片，显示已上传列表）
//   GET  /list         已上传图片 JSON 列表
//   POST /upload?name= 原始二进制写入 /sdcard/photos/<name>
//   POST /delete?name= 删除指定图片
//
// 上传采用 raw body（不是 multipart），服务端只做「循环 recv → fwrite」，
// 不需要解析 multipart 边界，内存占用只有 4KB 栈缓冲，传 8MB 图也不吃 RAM。
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

    // 从 query 里取 name 参数并做安全过滤，成功返回 true
    static bool GetSafeName(httpd_req_t* req, char* out, size_t out_len);

    void NotifyUploaded(const std::string& path) {
        if (on_uploaded_) {
            on_uploaded_(path);
        }
    }

    httpd_handle_t server_ = nullptr;
    UploadedCallback on_uploaded_;
};

#endif  // __PHOTO_WEB_SERVER_H__
