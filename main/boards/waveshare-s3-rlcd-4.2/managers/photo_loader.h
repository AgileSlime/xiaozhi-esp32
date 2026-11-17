#ifndef __PHOTO_LOADER_H__
#define __PHOTO_LOADER_H__

#include <lvgl.h>
#include <string>
#include <vector>
#include <mutex>

// 主页美图加载器（单例）
//
// 为什么需要单独一层：
//   这块屏是 1-bit 单色 RLCD，LVGL 内部虽然按 RGB565 渲染，但 flush 回调里
//   只做一次固定阈值二值化（pixel < 0x7FFF → 黑）。照片如果直接交给 LVGL 显示，
//   中间调会被这一刀切掉，只剩下黑白剪影（人脸糊成一团黑影）。
//   所以必须在"送进 LVGL 之前"先做好有序抖动（Bayer 8×8），让每个像素只剩
//   纯黑/纯白，再让阈值去切时 1:1 保留抖动颗粒，观感才是"网点照片"。
//
// 流程：JPEG 解码(RGB888) → 自动对比度 → 中心裁剪 + 缩放 → 灰度 → Bayer 抖动
//       → 写进 RGB565 缓冲 → 包成 lv_image_dsc_t 交给 lv_image
//
// 目录约定：
//   /sdcard/photos/*.jpg      横竖都行，程序按 210×248 居中裁剪（cover）
//   内置默认图               assets/default_photo.c，无卡/空目录时使用
//
// 内存：输出缓冲 210×248×2 = 102 KB，放 PSRAM；解码临时缓冲同样在 PSRAM。
class HomePhoto {
public:
    static HomePhoto& GetInstance();

    // 扫描 /sdcard/photos，返回可用图片路径
    int Count();
    bool Available();

    // 加载第 index 张（0-based）。index < 0 表示"按当天日期自动挑一张"，
    // 保证同一天开机看到的是同一张，隔天自动换。
    bool Load(lv_obj_t* img, int width, int height, int index = -1);

    // 下一张（供按键/语音切换）
    bool Next(lv_obj_t* img, int width, int height);

    // 内置默认壁纸（编在固件里，见 assets/default_photo.c）
    // 没插 SD 卡、或 /sdcard/photos 是空目录时，主页显示这张，不留白也不显示提示文字。
    // 位图已经是在 PC 上抖动好的 1bpp，这里只做"展开成 RGB565"，不解码、不占额外内存。
    bool LoadDefault(lv_obj_t* img, int width, int height);

    // 当前显示的是第几张、叫什么
    int CurrentIndex() const { return current_index_; }
    const std::string& CurrentName() const { return current_name_; }

    // 重新扫描目录（网页上传/删除图片后调用，让新文件立刻可见）
    void Rescan();

    // 按完整路径或文件名查找索引，找不到返回 -1
    int IndexOf(const std::string& path_or_name) const;

    static constexpr const char* kPhotoDir = "/sdcard/photos";

private:
    HomePhoto() = default;
    ~HomePhoto() = default;
    HomePhoto(const HomePhoto&) = delete;
    HomePhoto& operator=(const HomePhoto&) = delete;

    void RefreshList();
    bool RenderFromFile(const char* path, int width, int height);
    bool RenderDefault(int width, int height);
    bool EnsureBuffers(int width, int height);
    // 不加锁的加载实现：调用者必须已持有 mutex_
    bool LoadLocked(lv_obj_t* img, int width, int height, int index);

    static constexpr const char* TAG = "HomePhoto";

    // 网页上传会在 HTTP 任务里改写 files_，UI 任务同时在读，
    // 所有对 files_ 的访问都必须持锁（LoadLocked 是锁内版本，避免递归加锁）
    mutable std::mutex mutex_;

    std::vector<std::string> files_;
    bool scanned_ = false;

    int current_index_ = -1;
    std::string current_name_;
    bool showing_default_ = false;      // 当前显示的是内置默认图（不是 SD 卡里的照片）

    int buf_w_ = 0;
    int buf_h_ = 0;
    uint8_t* pixel_buf_ = nullptr;      // RGB565 输出（PSRAM）
    lv_image_dsc_t dsc_ = {};
    bool dsc_ready_ = false;
};

#endif
