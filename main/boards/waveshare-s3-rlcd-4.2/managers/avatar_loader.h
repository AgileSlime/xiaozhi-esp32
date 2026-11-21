#ifndef __AVATAR_LOADER_H__
#define __AVATAR_LOADER_H__

#include <lvgl.h>
#include <cstdint>
#include <cstddef>

// 播放器页唱片中心的 AI 头像（52×52，1-bit）
//
// 为什么不复用官网 emoji：
//   emoji 是彩色图，这块屏只有黑与白。亮黄色脸会被阈值吃掉，
//   只剩眼睛嘴巴几个黑点（"头像只有三个点"）；染成剪影又丢五官。
//   而且运行时缩放彩色图要占内部 RAM，那点内存是 AFE 唤醒词检测的命根子。
//
// 所以走和壁纸同一条路：JPEG → 灰度 → Floyd-Steinberg 抖动 → 1bpp → NVS。
// 存的是抖动好的位图（52×52 = 364 字节），显示时展开成 RGB565，
// 画什么就是什么，不缩放、不糊、不吃内部 RAM（缓冲都在 PSRAM）。
//
// 上传接口：POST /avatar（网页与控制台都走这里）
class AvatarLoader {
public:
    static AvatarLoader& GetInstance();

    // 网页上传：JPEG 在内存里解码 → 抖动 → 存 NVS → 立刻可用
    bool ApplyJpeg(const uint8_t* jpg, size_t len, const char* label = "网页上传");

    // 开机调用：把上次存的头像装回来
    bool LoadSaved();

    // 给 lv_image 用；还没头像时返回 nullptr（调用方应退回图标）
    const lv_img_dsc_t* Dsc() const;

    bool HasAvatar() const { return ready_; }
    bool Clear();                       // 恢复成默认（图标）

    static constexpr int kSize = 52;    // 头像边长（像素）
    static constexpr const char* TAG = "Avatar";

private:
    AvatarLoader() = default;
    ~AvatarLoader() = default;
    AvatarLoader(const AvatarLoader&) = delete;
    AvatarLoader& operator=(const AvatarLoader&) = delete;

    bool RenderFromJpeg(const uint8_t* jpg, size_t len);
    void ExpandToRgb565();              // bits_ → rgb565_
    bool SaveToNvs();
    bool LoadFromNvs();

    // 抖动后的 1bpp：每行 (kSize+7)/8 = 7 字节，MSB 在前，1 = 白色
    uint8_t bits_[kSize * ((kSize + 7) / 8)] = {};
    uint8_t* rgb565_ = nullptr;         // kSize*kSize*2 = 5408，PSRAM
    lv_image_dsc_t dsc_ = {};
    bool ready_ = false;
};

#endif
