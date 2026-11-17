#ifndef __DEFAULT_PHOTO_H__
#define __DEFAULT_PHOTO_H__

#include <stdint.h>

// 内置默认壁纸（无 SD 卡 / /sdcard/photos 为空时主页显示的那张）
//
// 存的是**已经抖动好的 1bpp 位图**，不是 JPEG。原因：
//   - 这块屏只显示黑白，彩色 JPEG 进来最终也要被抖成 1bpp，没必要在设备上做；
//   - 编进固件当常量，开机第一帧就有图，不占 PSRAM、不花解码时间；
//   - 210×248 抖完只有 6.5KB，比一张同尺寸 JPEG 还小。
//
// 位图布局：逐行、MSB 在前，每行 kDefaultPhotoStride 字节，
//          第 x 列的 bit 在 data[y*stride + (x>>3)] 的 (0x80 >> (x&7)) 位上。
// 1 = 白，0 = 黑。
//
// 换图：python tools/gen-default-photo.py <图片> --out-c <本文件同名 .c>
//      （需要先改掉 .c 顶部的注释由脚本自动写）
#ifdef __cplusplus
extern "C" {
#endif

extern const int kDefaultPhotoW;
extern const int kDefaultPhotoH;
extern const int kDefaultPhotoStride;
extern const uint8_t kDefaultPhotoBits[];

#ifdef __cplusplus
}
#endif

#endif
