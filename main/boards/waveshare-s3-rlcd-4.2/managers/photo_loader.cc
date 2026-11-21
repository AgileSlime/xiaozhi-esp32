#include "photo_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "esp_jpeg_dec.h"
#include "../assets/default_photo.h"
#include "sdcard_manager.h"

// 日志 TAG 用类里的 HomePhoto::TAG（static constexpr），这里不再重复定义，
// 否则成员函数里的 TAG 会解析到类成员，这个文件级变量就成了未使用变量。

// Bayer 8×8 有序抖动矩阵（0-63）
// 用它把灰度映射成"疏密不同的黑点"，这是 1-bit 屏显示照片的唯一可行方式：
// 直接阈值二值化只能得到剪影，抖动才能保留明暗层次。
static const uint8_t kBayer8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

HomePhoto& HomePhoto::GetInstance() {
    static HomePhoto instance;
    return instance;
}

void HomePhoto::RefreshList() {
    files_.clear();
    scanned_ = true;

    auto& sd = SdcardManager::getInstance();
    if (!sd.isMounted()) {
        ESP_LOGW(TAG, "SD 卡未挂载，主页图片不可用");
        return;
    }

    auto jpg = sd.listFiles(kPhotoDir, ".jpg");
    auto jpeg = sd.listFiles(kPhotoDir, ".jpeg");

    files_.insert(files_.end(), jpg.begin(), jpg.end());
    files_.insert(files_.end(), jpeg.begin(), jpeg.end());

    std::sort(files_.begin(), files_.end());
    files_.erase(std::unique(files_.begin(), files_.end()), files_.end());

    ESP_LOGI(TAG, "图片目录扫描完成: %d 张（目录 %s）",
             static_cast<int>(files_.size()), kPhotoDir);
}

int HomePhoto::Count() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!scanned_) {
        RefreshList();
    }
    return static_cast<int>(files_.size());
}

bool HomePhoto::Available() {
    return Count() > 0;
}

bool HomePhoto::EnsureBuffers(int width, int height) {
    if (pixel_buf_ != nullptr && buf_w_ == width && buf_h_ == height) {
        return true;
    }

    if (pixel_buf_ != nullptr) {
        heap_caps_free(pixel_buf_);
        pixel_buf_ = nullptr;
    }

    const size_t bytes = static_cast<size_t>(width) * height * 2;
    pixel_buf_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (pixel_buf_ == nullptr) {
        ESP_LOGE(TAG, "图片缓冲分配失败（%u 字节）", static_cast<unsigned>(bytes));
        return false;
    }

    buf_w_ = width;
    buf_h_ = height;

    memset(&dsc_, 0, sizeof(dsc_));
    dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc_.header.w = width;
    dsc_.header.h = height;
    dsc_.header.stride = width * 2;
    dsc_.data_size = bytes;
    dsc_.data = pixel_buf_;
    dsc_ready_ = true;

    return true;
}

bool HomePhoto::RenderFromFile(const char* path, int width, int height) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "打不开图片: %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_len <= 0 || file_len > 8 * 1024 * 1024) {
        ESP_LOGW(TAG, "图片大小异常（%ld 字节），跳过: %s", file_len, path);
        fclose(f);
        return false;
    }

    uint8_t* jpg = static_cast<uint8_t*>(heap_caps_malloc(file_len, MALLOC_CAP_SPIRAM));
    if (jpg == nullptr) {
        ESP_LOGE(TAG, "读图缓冲分配失败");
        fclose(f);
        return false;
    }

    const size_t got = fread(jpg, 1, file_len, f);
    fclose(f);

    if (got != static_cast<size_t>(file_len)) {
        ESP_LOGW(TAG, "图片读取不完整: %s", path);
        heap_caps_free(jpg);
        return false;
    }

    const bool ok = RenderFromJpeg(jpg, static_cast<size_t>(file_len), path, width, height);
    heap_caps_free(jpg);
    return ok;
}

// 解码主流程：内存里的 JPEG → 裁剪缩放 → 灰度 → 自动对比度 → 抖动 → RGB565。
// 文件路径和"网页上传的纯内存"两条入口共用这份实现，效果完全一致。
bool HomePhoto::RenderFromJpeg(const uint8_t* jpg, size_t file_len, const char* label,
                               int width, int height) {
    if (jpg == nullptr || file_len == 0) {
        return false;
    }

    jpeg_dec_config_t cfg = {};
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.scale.width = 0;
    cfg.scale.height = 0;
    cfg.clipper.width = 0;
    cfg.clipper.height = 0;
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.block_enable = false;

    // ---- 第一遍：只探尺寸 ----
    uint16_t src_w = 0;
    uint16_t src_h = 0;
    {
        jpeg_dec_handle_t probe = nullptr;
        if (jpeg_dec_open(&cfg, &probe) != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "JPEG 解码器初始化失败: %s", label);
            return false;
        }

        jpeg_dec_io_t pio = {};
        pio.inbuf = const_cast<uint8_t*>(jpg);
        pio.inbuf_len = static_cast<int>(file_len);
        pio.inbuf_remain = static_cast<int>(file_len);

        jpeg_dec_header_info_t pinfo = {};
        const jpeg_error_t perr = jpeg_dec_parse_header(probe, &pio, &pinfo);
        jpeg_dec_close(probe);

        if (perr != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "不是有效的 JPEG: %s (err=%d)", label, static_cast<int>(perr));
            return false;
        }

        src_w = pinfo.width;
        src_h = pinfo.height;
    }

    if (src_w < 16 || src_h < 16) {
        ESP_LOGW(TAG, "图片尺寸异常 %dx%d: %s", src_w, src_h, label);
        return false;
    }

    // ---- 选择解码缩放档位 ----
    // 主页照片位只有 width×height（210×248），而手机原图动辄 4000×3000。
    // 以前固定只缩一半：2000×1500 的 RGB888 解码缓冲要 9MB，8MB PSRAM 直接分配失败，
    // 表现出来的就是"图明明传进去了，主页还是默认壁纸"。
    // esp_jpeg 最高支持 1/8，这里按"缩完不小于目标"来挑最大档位，把解码缓冲压到最小。
    //
    // 注意：以前这里留了 1.25 倍余量（need = width + width/4），结果 420×496 这种
    // 最常见的中等图反而不缩，要硬解 625KB RGB888。WiFi 一起工作时 PSRAM 一紧张就
    // 分配失败——表现就是"有线能传、无线报 422 解不开"。现在改成 1:1 目标即可缩，
    // 420×496 正好缩 1/2 解成 210×248，内存从 625KB 降到 156KB，且 box filter
    // 退化成 1:1 取样，画质无损。
    const int need_w = width;
    const int need_h = height;
    int scale_div = 1;
    for (int d = 2; d <= 8; d *= 2) {
        if (src_w / d >= need_w && src_h / d >= need_h) {
            scale_div = d;
        }
    }
    if (scale_div > 1) {
        const int sw = (src_w / scale_div / 8) * 8;
        const int sh = (src_h / scale_div / 8) * 8;
        if (sw >= 16 && sh >= 16) {
            cfg.scale.width = sw;
            cfg.scale.height = sh;
            ESP_LOGI(TAG, "大图（%dx%d），解码时缩到 %dx%d（1/%d）",
                     src_w, src_h, sw, sh, scale_div);
        } else {
            scale_div = 1;
        }
    }

    // 极端图（上亿像素的全景、扫描件）即使缩到 1/8，RGB888 缓冲也要几 MB，
    // 硬解只会把 PSRAM 分配失败，让整张图静默跳过。这里提前挡掉并说清楚原因。
    // 6MB 是给 8MB PSRAM 留出的安全线（缩到 1/8 后仍有 200 万像素可用）。
    {
        const long dec_px = static_cast<long>(src_w / scale_div) * (src_h / scale_div);
        if (dec_px * 3L > 6L * 1024L * 1024L) {
            ESP_LOGW(TAG, "图片过大（%dx%d，缩 %d 倍后仍 %ld 像素），跳过: %s",
                     src_w, src_h, scale_div, dec_px, label);
            return false;
        }
    }

    // ---- 第二遍：真解码（PSRAM 分配失败时自动降档重试）----
    // WiFi 活动时 PSRAM 会被驱动占掉一部分，同样的图有线能解、无线就分配失败，
    // 用户看到的就是"解不开（422）"。这里在分配失败时把 scale 再降一档重来，
    // 最多降到 1/8，保证只要 PSRAM 还有一点余量就能把图解出来。
    uint8_t* rgb = nullptr;
    int out_len = 0;
    {
        jpeg_error_t derr = JPEG_ERR_FAIL;
        int cur_div = scale_div;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (attempt > 0) {
                cur_div *= 2;
                if (cur_div > 8) {
                    break;
                }
                const int sw = (src_w / cur_div / 8) * 8;
                const int sh = (src_h / cur_div / 8) * 8;
                if (sw < 16 || sh < 16) {
                    break;
                }
                cfg.scale.width = sw;
                cfg.scale.height = sh;
                ESP_LOGW(TAG, "解码缓冲不足，降档到 1/%d 重试（%dx%d）", cur_div, sw, sh);
            }

            jpeg_dec_handle_t dec = nullptr;
            if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "JPEG 解码器初始化失败（第二遍）");
                break;
            }

            jpeg_dec_io_t io = {};
            io.inbuf = const_cast<uint8_t*>(jpg);
            io.inbuf_len = static_cast<int>(file_len);
            io.inbuf_remain = static_cast<int>(file_len);

            jpeg_dec_header_info_t info = {};
            if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "JPEG 头解析失败（第二遍）");
                jpeg_dec_close(dec);
                break;
            }

            int this_len = 0;
            if (jpeg_dec_get_outbuf_len(dec, &this_len) != JPEG_ERR_OK || this_len <= 0) {
                ESP_LOGE(TAG, "拿不到解码输出长度");
                jpeg_dec_close(dec);
                break;
            }

            // esp_jpeg 要求输出缓冲 16 字节对齐；heap_caps_malloc 不保证，
            // 不对齐会直接报 err=-4 "Output buffer address is not 16-byte aligned"。
            uint8_t* buf = static_cast<uint8_t*>(
                    heap_caps_aligned_alloc(16, static_cast<size_t>(this_len), MALLOC_CAP_SPIRAM));
            if (buf == nullptr) {
                ESP_LOGW(TAG, "解码缓冲分配失败（需 %d 字节，PSRAM 剩余 %u），降档重试",
                         this_len,
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                jpeg_dec_close(dec);
                continue;   // 降一档再来
            }

            io.outbuf = buf;
            derr = jpeg_dec_process(dec, &io);
            jpeg_dec_close(dec);
            if (derr != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "JPEG 解码失败 (err=%d)", static_cast<int>(derr));
                heap_caps_free(buf);
                break;
            }
            rgb = buf;
            out_len = this_len;
            break;
        }
        if (rgb == nullptr) {
            return false;
        }
    }

    // ---- 反推解码后的实际宽高 ----
    // 这里必须精确：宽高错一个像素，后面按行采样就会整幅错位
    // （表现出来就是"显示的是图片局部、画面糊成一团"）。
    //
    // 策略：先按"原图尺寸 / 缩放档位"的整数候选逐个校验（能整除 out_pixels
    // 的那组就是真实宽高），全部不中才退回按比例估算。
    const int out_pixels = out_len / 3;
    const double ratio = static_cast<double>(src_w) / static_cast<double>(src_h);
    int ow = 0;
    int oh = 0;
    for (int d = 1; d <= 8 && ow == 0; d *= 2) {
        const int cw = src_w / d;
        const int ch = src_h / d;
        const int candidates[2][2] = {
                {cw, ch},                            // 直接整除
                {(cw / 8) * 8, (ch / 8) * 8},        // 8 对齐版本
        };
        for (int k = 0; k < 2; ++k) {
            const long w = candidates[k][0];
            const long h = candidates[k][1];
            if (w >= 8 && h >= 8 && w * h == out_pixels) {
                ow = static_cast<int>(w);
                oh = static_cast<int>(h);
                break;
            }
        }
    }
    if (ow == 0) {
        // 兜底：按原图宽高比估算
        ow = static_cast<int>(std::lround(std::sqrt(static_cast<double>(out_pixels) * ratio)));
        if (ow <= 0) {
            heap_caps_free(rgb);
            return false;
        }
        oh = out_pixels / ow;
        while (static_cast<long>(ow) * oh > out_pixels) {
            oh -= 1;
            if (oh <= 0) break;
        }
    }
    if (oh <= 0 || ow < 8) {
        heap_caps_free(rgb);
        return false;
    }
    ESP_LOGI(TAG, "解码输出尺寸推定 %dx%d（原图 %dx%d，%d 像素）",
             ow, oh, src_w, src_h, out_pixels);

    if (EnsureBuffers(width, height) == false) {
        heap_caps_free(rgb);
        return false;
    }

    // ---- 居中裁剪（cover）----
    const double dst_ratio = static_cast<double>(width) / static_cast<double>(height);
    const double src_ratio = static_cast<double>(ow) / static_cast<double>(oh);

    int crop_w = 0;
    int crop_h = 0;
    int off_x = 0;
    int off_y = 0;

    if (src_ratio > dst_ratio) {
        crop_h = oh;
        crop_w = static_cast<int>(oh * dst_ratio);
        off_x = (ow - crop_w) / 2;
    } else {
        crop_w = ow;
        crop_h = static_cast<int>(ow / dst_ratio);
        off_y = (oh - crop_h) / 2;
    }
    if (crop_w <= 0 || crop_h <= 0) {
        heap_caps_free(rgb);
        return false;
    }
    if (off_x + crop_w > ow) crop_w = ow - off_x;
    if (off_y + crop_h > oh) crop_h = oh - off_y;

    // ---- 采样成灰度 + 直方图 ----
    const int total = width * height;
    uint8_t* gray = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM));
    uint32_t* hist = static_cast<uint32_t*>(heap_caps_malloc(256 * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
    if (gray == nullptr || hist == nullptr) {
        if (gray) heap_caps_free(gray);
        if (hist) heap_caps_free(hist);
        heap_caps_free(rgb);
        ESP_LOGE(TAG, "灰度缓冲分配失败");
        return false;
    }

    memset(hist, 0, 256 * sizeof(uint32_t));

    // 每个目标像素覆盖源图的一小块矩形，取这块的平均灰度（box filter）。
    // 以前是"取矩形中点那一个像素"：从 500 宽压到 210 时每格跳过 2~3 个像素，
    // 高频细节会被随机丢成噪点（衣服纹理发花、人脸上出现麻点）。
    // 平均之后同一块区域的结果稳定，抖动出来的层次才干净。
    // 1:1 或放大时这块退化成一个像素，行为等同原来的点采样。
    for (int y = 0; y < height; y++) {
        const int sy0 = off_y + static_cast<int>(static_cast<long>(y) * crop_h / height);
        int sy1 = off_y + static_cast<int>(static_cast<long>(y + 1) * crop_h / height);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > oh) sy1 = oh;

        uint8_t* out_row = gray + y * width;
        for (int x = 0; x < width; x++) {
            const int sx0 = off_x + static_cast<int>(static_cast<long>(x) * crop_w / width);
            int sx1 = off_x + static_cast<int>(static_cast<long>(x + 1) * crop_w / width);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > ow) sx1 = ow;

            uint32_t sum = 0;
            int n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t* row = rgb + static_cast<long>(sy) * ow * 3;
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint8_t* p = row + static_cast<long>(sx) * 3;
                    // 亮度加权（BT.601）
                    sum += static_cast<uint32_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
                    n++;
                }
            }
            const uint8_t g = (n > 0) ? static_cast<uint8_t>(sum / static_cast<uint32_t>(n)) : 0;
            out_row[x] = g;
            hist[g]++;
        }
    }

    // ---- 自动对比度：按 1% / 99% 分位做拉伸 ----
    // 照片经过裁剪缩放后常常偏灰，不拉伸的话抖动出来要么死黑要么死白。
    const uint32_t low_cut = static_cast<uint32_t>(total * 0.01f);
    const uint32_t high_cut = static_cast<uint32_t>(total * 0.99f);

    int lo = 0;
    int hi = 255;
    uint32_t acc = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc >= low_cut) { lo = i; break; }
    }
    acc = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc >= high_cut) { hi = i; break; }
    }
    if (hi - lo < 12) {
        // 对比度本来就极低（纯色/夜景），不做拉伸，否则噪声被放大成雪花
        lo = 0;
        hi = 255;
    }

    const int span = hi - lo;
    const int inv_span = (span > 0) ? span : 1;

    // 顺手把抖动结果也存一份 1bpp：网页上传的图靠它活过"下一次重绘"
    // （不存的话每分钟的 UI 刷新会把它顶回默认壁纸）
    const size_t stride = (static_cast<size_t>(width) + 7) / 8;
    const size_t bits_bytes = stride * static_cast<size_t>(height);
    if (override_bits_ == nullptr || override_bytes_ < bits_bytes ||
        override_w_ != width || override_h_ != height) {
        if (override_bits_ != nullptr) {
            heap_caps_free(override_bits_);
            override_bits_ = nullptr;
        }
        override_bits_ = static_cast<uint8_t*>(heap_caps_malloc(bits_bytes, MALLOC_CAP_SPIRAM));
        override_bytes_ = (override_bits_ != nullptr) ? bits_bytes : 0;
        override_w_ = width;
        override_h_ = height;
    }
    if (override_bits_ != nullptr) {
        memset(override_bits_, 0, bits_bytes);
    }

    // ---- 抖动 + 写 RGB565 ----
    uint16_t* dst = reinterpret_cast<uint16_t*>(pixel_buf_);
    for (int y = 0; y < height; y++) {
        const uint8_t* src_row = gray + y * width;
        const uint8_t* bayer_row = kBayer8[y & 7];
        uint16_t* dst_row = dst + y * width;

        for (int x = 0; x < width; x++) {
            int v = src_row[x];
            if (span > 0) {
                v = (v - lo) * 255 / inv_span;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
            }
            // 阈值 = bayer * 4 + 2（把 0-63 铺到 0-254）
            const int threshold = static_cast<int>(bayer_row[x & 7]) * 4 + 2;
            const bool white = (v > threshold);
            dst_row[x] = white ? 0xFFFF : 0x0000;
            if (override_bits_ != nullptr) {
                uint8_t* bits_row = override_bits_ + static_cast<size_t>(y) * stride;
                if (white) {
                    bits_row[x >> 3] |= static_cast<uint8_t>(0x80 >> (x & 7));
                }
            }
        }
    }

    heap_caps_free(gray);
    heap_caps_free(hist);
    heap_caps_free(rgb);

    ESP_LOGI(TAG, "图片已抖动渲染: %s（源 %dx%d → 解码 %dx%d → 输出 %dx%d）",
             label, src_w, src_h, ow, oh, width, height);
    return true;
}

bool HomePhoto::Load(lv_obj_t* img, int width, int height, int index) {
    if (img == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    return LoadLocked(img, width, height, index);
}

// 内置默认壁纸：位图在 PC 上已经抖成 1bpp，这里只做展开。
// 不经过 JPEG 解码，所以没有 156KB 解码缓冲，也不花几百毫秒 —— 开机第一帧就能出图。
bool HomePhoto::RenderDefault(int width, int height) {
    if (kDefaultPhotoW <= 0 || kDefaultPhotoH <= 0) {
        return false;
    }
    if (!EnsureBuffers(width, height)) {
        return false;
    }

    uint16_t* dst = reinterpret_cast<uint16_t*>(pixel_buf_);
    // 目标尺寸和位图尺寸一致时（常态 210×248），下面两个除法结果恒等于 x/y，
    // 相当于逐像素 1:1 展开；不一致时退化成最近邻缩放，不至于花屏。
    for (int y = 0; y < height; y++) {
        const int sy = static_cast<int>(static_cast<long>(y) * kDefaultPhotoH / height);
        const uint8_t* row = kDefaultPhotoBits + static_cast<long>(sy) * kDefaultPhotoStride;
        uint16_t* dst_row = dst + static_cast<long>(y) * width;
        for (int x = 0; x < width; x++) {
            const int sx = static_cast<int>(static_cast<long>(x) * kDefaultPhotoW / width);
            const bool white = (row[sx >> 3] & (0x80 >> (sx & 7))) != 0;
            dst_row[x] = white ? 0xFFFF : 0x0000;
        }
    }

    ESP_LOGI(TAG, "内置默认壁纸已展开: %dx%d", width, height);
    return true;
}

// 把网页上传那张图的 1bpp 位图展开成 RGB565（和 RenderDefault 同一套做法）
bool HomePhoto::RenderOverride(int width, int height) {
    if (!override_active_ || override_bits_ == nullptr ||
        override_w_ <= 0 || override_h_ <= 0) {
        return false;
    }
    if (!EnsureBuffers(width, height)) {
        return false;
    }

    const size_t stride = (static_cast<size_t>(override_w_) + 7) / 8;
    uint16_t* dst = reinterpret_cast<uint16_t*>(pixel_buf_);
    for (int y = 0; y < height; y++) {
        const int sy = static_cast<int>(static_cast<long>(y) * override_h_ / height);
        const uint8_t* row = override_bits_ + static_cast<long>(sy) * stride;
        uint16_t* dst_row = dst + static_cast<long>(y) * width;
        for (int x = 0; x < width; x++) {
            const int sx = static_cast<int>(static_cast<long>(x) * override_w_ / width);
            const bool white = (row[sx >> 3] & (0x80 >> (sx & 7))) != 0;
            dst_row[x] = white ? 0xFFFF : 0x0000;
        }
    }
    return true;
}

bool HomePhoto::LoadOverrideLocked(lv_obj_t* img, int width, int height) {
    if (!RenderOverride(width, height)) {
        return false;
    }
    showing_default_ = false;
    current_index_ = -1;
    current_name_ = override_name_;
    lv_image_set_src(img, &dsc_);
    return true;
}

bool HomePhoto::LoadDefault(lv_obj_t* img, int width, int height) {
    if (img == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);

    // 网页上传过图之后，默认图不该再抢位置
    if (override_active_) {
        if (RenderOverride(width, height)) {
            lv_image_set_src(img, &dsc_);
            current_index_ = -1;
            current_name_ = override_name_;
            showing_default_ = false;
            return true;
        }
    }

    // data_update_task 每分钟会重试一次"首次加载失败"，如果已经在显示默认图，
    // 直接返回成功、不重画 —— 否则每分钟刷一次同画面，1-bit 屏上会看到闪一下。
    if (showing_default_ && dsc_ready_ && buf_w_ == width && buf_h_ == height) {
        return true;
    }

    if (!RenderDefault(width, height)) {
        return false;
    }
    showing_default_ = true;
    current_index_ = -1;
    current_name_ = "内置默认图";
    lv_image_set_src(img, &dsc_);
    return true;
}

// 锁内实现：不重复加锁，供 Load / Next 复用
bool HomePhoto::LoadLocked(lv_obj_t* img, int width, int height, int index) {
    // 网页上传的那张图优先于 SD 卡：它就是"当前这张"
    if (override_active_) {
        if (LoadOverrideLocked(img, width, height)) {
            return true;
        }
        // 位图展不开（PSRAM 不够）才落到后面的 SD / 默认图
    }

    if (!scanned_) {
        RefreshList();
    }
    const int count = static_cast<int>(files_.size());
    if (count <= 0) {
        return false;
    }

    int target = index;
    if (target < 0) {
        // 按"当天是今年第几天"取模，保证一天一换、重启不跳图
        const time_t now = time(nullptr);
        struct tm ti = {};
        localtime_r(&now, &ti);
        const int day_of_year = ti.tm_yday >= 0 ? ti.tm_yday : 0;
        target = day_of_year % count;
    }
    target = ((target % count) + count) % count;

    const std::string& path = files_[target];
    if (!RenderFromFile(path.c_str(), width, height)) {
        // 单张坏图不应该让整块区域空着：顺延试下一张
        for (int i = 1; i < count; i++) {
            const int alt = (target + i) % count;
            if (RenderFromFile(files_[alt].c_str(), width, height)) {
                current_index_ = alt;
                current_name_ = files_[alt];
                showing_default_ = false;
                lv_image_set_src(img, &dsc_);
                return true;
            }
        }
        return false;
    }

    current_index_ = target;
    current_name_ = path;
    showing_default_ = false;
    lv_image_set_src(img, &dsc_);
    return true;
}

bool HomePhoto::Next(lv_obj_t* img, int width, int height) {
    if (img == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);

    // "换一张"是明确的换图指令：先撤销上传的那张，回到 SD 卡轮换
    // （否则 override 会永远压在上面，用户会觉得换图没反应）
    if (override_active_) {
        override_active_ = false;
        ESP_LOGI(TAG, "已撤销网页上传的图片，回到 SD 卡轮换");
    }

    if (!scanned_) {
        RefreshList();
    }
    const int count = static_cast<int>(files_.size());
    if (count <= 0) {
        return false;
    }
    return LoadLocked(img, width, height, (current_index_ + 1) % count);
}

// 网页上传 / 删除图片后调用：重新扫描目录，并清掉当前索引，
// 让下一次 Load 重新按"当天日期"挑图（否则新图要等重启才出现）
void HomePhoto::Rescan() {
    std::lock_guard<std::mutex> lk(mutex_);
    RefreshList();
    current_index_ = -1;
}

int HomePhoto::IndexOf(const std::string& path_or_name) const {
    std::lock_guard<std::mutex> lk(mutex_);

    const size_t slash = path_or_name.find_last_of('/');
    const std::string base =
        (slash == std::string::npos) ? path_or_name : path_or_name.substr(slash + 1);

    for (size_t i = 0; i < files_.size(); i++) {
        if (files_[i] == path_or_name) {
            return static_cast<int>(i);
        }
        const size_t s = files_[i].find_last_of('/');
        const std::string b = (s == std::string::npos) ? files_[i] : files_[i].substr(s + 1);
        if (b == base) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ===== 一次性图片（网页上传）=====

bool HomePhoto::ApplyJpegMemory(const uint8_t* jpg, size_t len, int width, int height) {
    if (jpg == nullptr || len == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);

    if (!RenderFromJpeg(jpg, len, "网页上传", width, height)) {
        return false;
    }
    override_active_ = true;
    showing_default_ = false;
    current_index_ = -1;
    current_name_ = override_name_;

    // 顺手存 NVS：这样它就成了新的"默认图"，重启、拔电都还是它。
    // 存失败也不影响这一次显示，只是重启后会退回上一张/内置图。
    if (SaveOverrideLocked()) {
        ESP_LOGI(TAG, "网页上传的图片已生效，并已存入 NVS（重启后仍是这张）");
    } else {
        ESP_LOGW(TAG, "网页上传的图片已生效，但写入 NVS 失败（重启后会恢复上一张）");
    }
    return true;
}

void HomePhoto::ClearOverride() {
    std::lock_guard<std::mutex> lk(mutex_);
    override_active_ = false;
}

bool HomePhoto::HasOverride() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return override_active_;
}

// ---------------------------------------------------------------- NVS 常驻

namespace {
// NVS 里这块数据的格式：魔数 + 宽 + 高 + 字节数 + 1bpp 位图本体
constexpr const char* kNvsNamespace = "hphoto";
constexpr const char* kNvsKey = "bmp";
constexpr uint8_t kNvsMagic[4] = {'H', 'P', 'B', '1'};
constexpr size_t kNvsHeader = 12;
}  // namespace

// 需已持锁
bool HomePhoto::SaveOverrideLocked() {
    if (!override_active_ || override_bits_ == nullptr ||
        override_w_ <= 0 || override_h_ <= 0) {
        return false;
    }
    const size_t stride = (static_cast<size_t>(override_w_) + 7) / 8;
    const size_t bits = stride * static_cast<size_t>(override_h_);
    if (bits == 0 || bits > override_bytes_) {
        return false;
    }

    const size_t total = kNvsHeader + bits;
    auto* blob = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM));
    if (blob == nullptr) {
        return false;
    }
    memcpy(blob, kNvsMagic, sizeof(kNvsMagic));
    blob[4] = static_cast<uint8_t>(override_w_ & 0xFF);
    blob[5] = static_cast<uint8_t>((override_w_ >> 8) & 0xFF);
    blob[6] = static_cast<uint8_t>(override_h_ & 0xFF);
    blob[7] = static_cast<uint8_t>((override_h_ >> 8) & 0xFF);
    blob[8] = static_cast<uint8_t>(bits & 0xFF);
    blob[9] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    blob[10] = static_cast<uint8_t>((bits >> 16) & 0xFF);
    blob[11] = static_cast<uint8_t>((bits >> 24) & 0xFF);
    memcpy(blob + kNvsHeader, override_bits_, bits);

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, kNvsKey, blob, total);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    heap_caps_free(blob);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写 NVS 失败: %s", esp_err_to_name(err));
        return false;
    }
    override_saved_ = true;
    return true;
}

// 需已持锁
bool HomePhoto::LoadSavedOverrideLocked() {
    nvs_handle_t h = 0;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return false;   // 从没存过，正常
    }

    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, kNvsKey, nullptr, &len);
    if (err != ESP_OK || len <= kNvsHeader) {
        nvs_close(h);
        return false;
    }

    auto* blob = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
    if (blob == nullptr) {
        nvs_close(h);
        return false;
    }
    err = nvs_get_blob(h, kNvsKey, blob, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        heap_caps_free(blob);
        return false;
    }

    if (memcmp(blob, kNvsMagic, sizeof(kNvsMagic)) != 0) {
        heap_caps_free(blob);
        return false;
    }
    const int w = blob[4] | (blob[5] << 8);
    const int hh = blob[6] | (blob[7] << 8);
    const size_t bits = static_cast<size_t>(blob[8]) |
                        (static_cast<size_t>(blob[9]) << 8) |
                        (static_cast<size_t>(blob[10]) << 16) |
                        (static_cast<size_t>(blob[11]) << 24);

    if (w <= 0 || hh <= 0 || w > 1024 || hh > 1024) {
        heap_caps_free(blob);
        return false;
    }
    const size_t stride = (static_cast<size_t>(w) + 7) / 8;
    if (bits != stride * static_cast<size_t>(hh) || kNvsHeader + bits > len) {
        ESP_LOGW(TAG, "NVS 里的壁纸数据不完整（%ux%u, %u 字节），忽略",
                 static_cast<unsigned>(w), static_cast<unsigned>(hh),
                 static_cast<unsigned>(bits));
        heap_caps_free(blob);
        return false;
    }

    if (override_bits_ != nullptr) {
        heap_caps_free(override_bits_);
        override_bits_ = nullptr;
    }
    override_bits_ = static_cast<uint8_t*>(heap_caps_malloc(bits, MALLOC_CAP_SPIRAM));
    if (override_bits_ == nullptr) {
        heap_caps_free(blob);
        return false;
    }
    memcpy(override_bits_, blob + kNvsHeader, bits);
    override_bytes_ = bits;
    override_w_ = w;
    override_h_ = hh;
    override_active_ = true;
    override_saved_ = true;
    override_name_ = "上次上传";
    heap_caps_free(blob);

    ESP_LOGI(TAG, "已装回上次上传的壁纸（%dx%d，%u 字节，取自 NVS）",
             w, hh, static_cast<unsigned>(bits));
    return true;
}

bool HomePhoto::LoadSavedOverride() {
    std::lock_guard<std::mutex> lk(mutex_);
    return LoadSavedOverrideLocked();
}

bool HomePhoto::SaveOverride() {
    std::lock_guard<std::mutex> lk(mutex_);
    return SaveOverrideLocked();
}

bool HomePhoto::HasSavedOverride() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return override_saved_;
}

bool HomePhoto::ForgetSavedOverride() {
    std::lock_guard<std::mutex> lk(mutex_);

    override_active_ = false;
    override_saved_ = false;

    nvs_handle_t h = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return true;   // 没有命名空间 = 本来就没存过
    }
    const esp_err_t err = nvs_erase_key(h, kNvsKey);
    nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "清除 NVS 壁纸失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "已清除常驻壁纸，主页回到内置默认图 / SD 卡照片");
    return true;
}
