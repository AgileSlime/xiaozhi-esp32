#include "avatar_loader.h"

#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>
#include <esp_log.h>
#include <nvs.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr const char* kNvsNamespace = "avatar";
constexpr const char* kNvsKey = "bmp";
constexpr int kBitsStride = (AvatarLoader::kSize + 7) / 8;   // 7 字节/行
}  // namespace

AvatarLoader& AvatarLoader::GetInstance() {
    static AvatarLoader instance;
    return instance;
}

bool AvatarLoader::RenderFromJpeg(const uint8_t* jpg, size_t len) {
    const int S = kSize;

    jpeg_dec_config_t cfg = {};
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.scale.width = 0;
    cfg.scale.height = 0;
    cfg.clipper.width = 0;
    cfg.clipper.height = 0;
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.block_enable = false;

    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "解码器初始化失败");
        return false;
    }

    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<uint8_t*>(jpg);
    io.inbuf_len = static_cast<int>(len);
    io.inbuf_remain = static_cast<int>(len);

    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK || info.width <= 0 || info.height <= 0) {
        ESP_LOGE(TAG, "JPEG 头解析失败");
        jpeg_dec_close(dec);
        return false;
    }

    // 选缩放档：缩完仍不小于 52 就行（省 PSRAM，也不用缩太多损失细节）
    int div = 1;
    while (true) {
        const int nw = info.width / div;
        const int nh = info.height / div;
        if (nw < S || nh < S) {
            div = std::max(1, div / 2);
            break;
        }
        div *= 2;
        if (div > 8) {
            div = 8;
            break;
        }
    }
    int sw = (info.width / div / 8) * 8;
    int sh = (info.height / div / 8) * 8;
    if (sw < S || sh < S) {
        sw = 0;   // 0 = 不缩放，按原图解码
        sh = 0;
    }
    cfg.scale.width = sw;
    cfg.scale.height = sh;

    jpeg_dec_close(dec);
    dec = nullptr;

    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "解码器初始化失败（第二遍）");
        return false;
    }
    io.inbuf = const_cast<uint8_t*>(jpg);
    io.inbuf_len = static_cast<int>(len);
    io.inbuf_remain = static_cast<int>(len);

    // 第二遍必须重新解一次头：新建的 decoder 不知道图像信息，
    // 直接 get_outbuf_len 会报 "Should parse header first"。
    jpeg_dec_header_info_t info2 = {};
    if (jpeg_dec_parse_header(dec, &io, &info2) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG 头解析失败（第二遍）");
        jpeg_dec_close(dec);
        return false;
    }

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(dec, &out_len) != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGE(TAG, "拿不到解码输出长度");
        jpeg_dec_close(dec);
        return false;
    }

    // esp_jpeg 要求输出缓冲 16 字节对齐，heap_caps_malloc 不保证，
    // 不对齐会直接被拒（err=-4 "Output buffer address is not 16-byte aligned"）。
    uint8_t* rgb = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(16, static_cast<size_t>(out_len), MALLOC_CAP_SPIRAM));
    if (rgb == nullptr) {
        ESP_LOGE(TAG, "解码缓冲分配失败（需 %d 字节）", out_len);
        jpeg_dec_close(dec);
        return false;
    }

    io.outbuf = rgb;
    const jpeg_error_t derr = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    if (derr != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败: %d", derr);
        heap_caps_free(rgb);
        return false;
    }

    // 解码后的实际尺寸（按 8 对齐可能和请求值差一点，用像素数反推）
    const int out_pixels = out_len / 3;
    int ow = 0;
    int oh = 0;
    for (int d = 1; d <= 8 && ow == 0; d *= 2) {
        const int cw = info.width / d;
        const int ch = info.height / d;
        const int cand[2][2] = {
                {cw, ch},
                {(cw / 8) * 8, (ch / 8) * 8},
        };
        for (int k = 0; k < 2; ++k) {
            const long w = cand[k][0];
            const long h = cand[k][1];
            if (w >= S && h >= S && w * h == out_pixels) {
                ow = static_cast<int>(w);
                oh = static_cast<int>(h);
                break;
            }
        }
    }
    if (ow == 0) {
        ow = static_cast<int>(std::lround(std::sqrt(
                static_cast<double>(out_pixels) * info.width / info.height)));
        if (ow <= 0) {
            heap_caps_free(rgb);
            return false;
        }
        oh = out_pixels / ow;
    }
    if (ow < S || oh < S) {
        ESP_LOGE(TAG, "解码尺寸 %dx%d 小于头像 %d", ow, oh, S);
        heap_caps_free(rgb);
        return false;
    }

    // 居中裁剪成 S×S
    //
    // 灰度图和误差缓冲都放 PSRAM 堆上，不能放栈上：
    //   gray 2704 字节 + err 5516 字节 ≈ 8.2KB，
    //   而上传是在 httpd 任务里跑的，它的栈只有 8KB —— 放栈上必溢出。
    const int off_x = (ow - S) / 2;
    const int off_y = (oh - S) / 2;

    uint8_t* gray = static_cast<uint8_t*>(heap_caps_malloc(S * S, MALLOC_CAP_SPIRAM));
    int16_t* err = static_cast<int16_t*>(
            heap_caps_malloc((S * S + S + 2) * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (gray == nullptr || err == nullptr) {
        ESP_LOGE(TAG, "灰度/误差缓冲分配失败");
        heap_caps_free(gray);
        heap_caps_free(err);
        heap_caps_free(rgb);
        return false;
    }

    for (int y = 0; y < S; ++y) {
        const uint8_t* src_row = rgb + static_cast<size_t>(y + off_y) * ow * 3 + off_x * 3;
        for (int x = 0; x < S; ++x) {
            const uint8_t* p = src_row + x * 3;
            gray[y * S + x] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        }
    }
    heap_caps_free(rgb);
    rgb = nullptr;

    // 对比度拉伸
    {
        int minv = 255;
        int maxv = 0;
        for (int i = 0; i < S * S; ++i) {
            minv = std::min(minv, static_cast<int>(gray[i]));
            maxv = std::max(maxv, static_cast<int>(gray[i]));
        }
        if (maxv - minv >= 32) {
            const int span = maxv - minv;
            for (int i = 0; i < S * S; ++i) {
                const int v = (static_cast<int>(gray[i]) - minv) * 255 / span;
                gray[i] = static_cast<uint8_t>(std::clamp(v, 0, 255));
            }
        }
    }

    // Floyd-Steinberg 抖动 → 1bpp（1 = 白）
    std::memset(bits_, 0, sizeof(bits_));
    for (int i = 0; i < S * S; ++i) {
        err[i] = gray[i];
    }
    heap_caps_free(gray);
    gray = nullptr;

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            const int idx = y * S + x;
            const int old = std::clamp(static_cast<int>(err[idx]), 0, 255);
            const int nb = (old >= 128) ? 255 : 0;
            if (nb) {
                bits_[y * kBitsStride + (x >> 3)] |= static_cast<uint8_t>(0x80 >> (x & 7));
            }
            const int e = old - nb;
            if (x + 1 < S) err[idx + 1] += static_cast<int16_t>(e * 7 / 16);
            if (y + 1 < S) {
                if (x > 0) err[idx + S - 1] += static_cast<int16_t>(e * 3 / 16);
                err[idx + S] += static_cast<int16_t>(e * 5 / 16);
                if (x + 1 < S) err[idx + S + 1] += static_cast<int16_t>(e / 16);
            }
        }
    }
    heap_caps_free(err);

    ESP_LOGI(TAG, "头像已生成（%dx%d → 解码 %dx%d → 裁剪 %dx%d）",
             info.width, info.height, ow, oh, S, S);
    return true;
}

void AvatarLoader::ExpandToRgb565() {
    const int S = kSize;
    if (rgb565_ == nullptr) {
        rgb565_ = static_cast<uint8_t*>(heap_caps_malloc(S * S * 2, MALLOC_CAP_SPIRAM));
        if (rgb565_ == nullptr) {
            ESP_LOGE(TAG, "RGB565 缓冲分配失败");
            return;
        }
    }
    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            const bool white = (bits_[y * kBitsStride + (x >> 3)] >> (7 - (x & 7))) & 1;
            const uint16_t px = white ? 0xFFFF : 0x0000;
            const size_t o = (static_cast<size_t>(y) * S + x) * 2;
            rgb565_[o] = static_cast<uint8_t>(px & 0xFF);
            rgb565_[o + 1] = static_cast<uint8_t>(px >> 8);
        }
    }
    dsc_ = {};
    dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc_.header.w = S;
    dsc_.header.h = S;
    dsc_.data_size = S * S * 2;
    dsc_.data = rgb565_;
    ready_ = true;
}

bool AvatarLoader::SaveToNvs() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败");
        return false;
    }
    const esp_err_t err = nvs_set_blob(h, kNvsKey, bits_, sizeof(bits_));
    if (err == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "头像已存入 NVS（%u 字节）", static_cast<unsigned>(sizeof(bits_)));
    } else {
        ESP_LOGE(TAG, "NVS 写入失败: %d", err);
    }
    nvs_close(h);
    return err == ESP_OK;
}

bool AvatarLoader::LoadFromNvs() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = sizeof(bits_);
    const esp_err_t err = nvs_get_blob(h, kNvsKey, bits_, &sz);
    nvs_close(h);
    if (err != ESP_OK || sz != sizeof(bits_)) {
        return false;
    }
    ExpandToRgb565();
    ESP_LOGI(TAG, "已装回上次的头像（%u 字节，取自 NVS）", static_cast<unsigned>(sz));
    return ready_;
}

bool AvatarLoader::ApplyJpeg(const uint8_t* jpg, size_t len, const char* label) {
    if (jpg == nullptr || len == 0) {
        return false;
    }
    if (!RenderFromJpeg(jpg, len)) {
        return false;
    }
    ExpandToRgb565();
    if (!ready_) {
        return false;
    }
    SaveToNvs();
    ESP_LOGI(TAG, "头像已更新：%s", label != nullptr ? label : "网页上传");
    return true;
}

bool AvatarLoader::LoadSaved() {
    return LoadFromNvs();
}

const lv_img_dsc_t* AvatarLoader::Dsc() const {
    // 懒加载：首次访问时从 NVS 读（省掉在 board 初始化里多挂一个调用点）
    if (!ready_) {
        const_cast<AvatarLoader*>(this)->LoadFromNvs();
    }
    return ready_ ? &dsc_ : nullptr;
}

bool AvatarLoader::Clear() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, kNvsKey);
        nvs_commit(h);
        nvs_close(h);
    }
    std::memset(bits_, 0, sizeof(bits_));
    ready_ = false;
    ESP_LOGI(TAG, "头像已清除，恢复默认图标");
    return true;
}
