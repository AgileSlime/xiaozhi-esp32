#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把一张彩色照片转成"这块 1-bit 屏能直接显示的位图"，并生成固件用的 C 数组。

为什么要在 PC 上先抖动：
  屏是 400x300 单色 RLCD，flush 里只做一次固定阈值二值化。照片若交给设备
  自己解 JPEG 再抖动，要占 PSRAM 里 156KB 解码缓冲 + 几百毫秒 CPU，而默认图
  是开机第一帧就要显示的常量。事先抖成 1bpp 后，6.5KB 全在 Flash，
  固件里只需把 bit 展开成 RGB565。

处理和 photo_loader.cc 的 SD 卡照片走同一套语义：
  裁切(fit/cover 或手动窗口) -> 灰度 -> 锐化 -> 高光抬升 -> 1%/99% 拉伸 -> Bayer 8x8

用法：
  # 自动 cover 裁切
  python gen-default-photo.py in.jpg --out-c out.c
  # 指定裁切窗（源图像素坐标 x,y,w,h），并把高光抬起来让脸不糊
  python gen-default-photo.py in.jpg --crop 680,100,660,780 \
         --sharp 200 --lift 1.15 --knee 150 --out-c out.c
"""
import argparse
import os

from PIL import Image, ImageFilter

BAYER = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]


def cover_crop(src, tw, th):
    """按目标比例居中裁剪（cover），和 photo_loader.cc 一致"""
    sw, sh = src.size
    dst_ratio, src_ratio = tw / th, sw / sh
    if src_ratio > dst_ratio:
        ch, cw = sh, int(sh * dst_ratio)
        ox, oy = (sw - cw) // 2, 0
    else:
        cw, ch = sw, int(sw / dst_ratio)
        ox, oy = 0, (sh - ch) // 2
    return src.crop((ox, oy, ox + cw, oy + ch)).resize((tw, th), Image.LANCZOS)


def custom_crop(src, box, tw, th):
    """按手动窗口裁切并缩放到目标尺寸（窗口比例会被拉伸，所以请自己按 210:248 取）"""
    x, y, w, h = box
    return src.crop((x, y, x + w, y + h)).resize((tw, th), Image.LANCZOS)


def lift_highlights(img, lift, knee):
    """把 knee 以上的浅灰往白推。
    单色屏上人脸是"浅灰一片 + 几个深色五官"，不抬的话整片浅灰会抖成网点，
    五官淹没在噪点里；抬起来后脸接近纯白，只剩眼睛和轮廓是黑的。"""
    if lift <= 1.0:
        return img

    def f(v):
        if v <= knee:
            return v
        t = (v - knee) / (255.0 - knee)
        return min(255, int(knee + (t ** 0.55) * (255 - knee) * lift))

    return img.point([f(i) for i in range(256)])


def stretch(img, tw, th, contrast=1.0, brightness=0):
    """1%/99% 分位拉伸 + 可选对比度/亮度（与固件里那段直方图拉伸同义）"""
    px = img.load()
    total = tw * th
    hist = [0] * 256
    for y in range(th):
        for x in range(tw):
            hist[px[x, y]] += 1

    lo, hi, acc = 0, 255, 0
    for i in range(256):
        acc += hist[i]
        if acc >= total * 0.01:
            lo = i
            break
    acc = 0
    for i in range(256):
        acc += hist[i]
        if acc >= total * 0.99:
            hi = i
            break
    span = hi - lo
    if span < 12:
        lo, hi, span = 0, 255, 255

    out = [[0] * tw for _ in range(th)]
    for y in range(th):
        for x in range(tw):
            v = (px[x, y] - lo) * 255 // span
            v = 0 if v < 0 else (255 if v > 255 else v)
            if contrast != 1.0:
                v = int((v - 128) * contrast + 128)
            v += brightness
            out[y][x] = 0 if v < 0 else (255 if v > 255 else v)
    return out


def bayer(gray, tw, th):
    out = Image.new('1', (tw, th), 0)
    px = out.load()
    for y in range(th):
        row = gray[y]
        brow = BAYER[y & 7]
        for x in range(tw):
            px[x, y] = 1 if row[x] > brow[x & 7] * 4 + 2 else 0
    return out


def to_c_array(bits, tw, th, out_path, source_name):
    """按行打包成 1bpp，每行补到整字节（210 -> 27 字节）"""
    stride = (tw + 7) // 8
    data = bytearray(stride * th)
    px = bits.load()
    for y in range(th):
        base = y * stride
        for x in range(tw):
            if px[x, y]:
                data[base + (x >> 3)] |= 0x80 >> (x & 7)

    L = []
    L.append('// 自动生成，请勿手改。生成脚本：tools/gen-default-photo.py')
    L.append('// 源图：%s' % source_name)
    L.append('// 格式：1bpp 逐行、MSB 在前，每行 %d 字节（有效 %d 像素，补到 %d 位）'
             % (stride, tw, stride * 8))
    L.append('')
    L.append('#include "default_photo.h"')
    L.append('')
    L.append('const int kDefaultPhotoW = %d;' % tw)
    L.append('const int kDefaultPhotoH = %d;' % th)
    L.append('const int kDefaultPhotoStride = %d;' % stride)
    L.append('')
    L.append('const uint8_t kDefaultPhotoBits[%d] = {' % len(data))
    for i in range(0, len(data), 24):
        L.append('    ' + ' '.join('0x%02X,' % b for b in data[i:i + 24]))
    L.append('};')
    L.append('')

    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L))
    return len(data), stride


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('--width', type=int, default=210)
    ap.add_argument('--height', type=int, default=248)
    ap.add_argument('--crop', default=None, help='x,y,w,h（源图像素）；省略则自动 cover')
    ap.add_argument('--sharp', type=int, default=0, help='UnsharpMask percent，0=关')
    ap.add_argument('--lift', type=float, default=1.0, help='高光抬升倍数，1.0=关')
    ap.add_argument('--knee', type=int, default=150, help='高光抬升起点')
    ap.add_argument('--contrast', type=float, default=1.0)
    ap.add_argument('--brightness', type=int, default=0)
    ap.add_argument('--out-c', default=None)
    ap.add_argument('--out-png', default=None)
    ap.add_argument('--out-check', default=None, help='并排对比图（原灰度 vs 抖动，3x）')
    args = ap.parse_args()

    tw, th = args.width, args.height
    src = Image.open(args.src).convert('L')

    if args.crop:
        box = tuple(int(v) for v in args.crop.split(','))
        assert len(box) == 4, '--crop 需要 x,y,w,h'
        crop = custom_crop(src, box, tw, th)
    else:
        crop = cover_crop(src, tw, th)

    if args.sharp:
        crop = crop.filter(ImageFilter.UnsharpMask(radius=2, percent=args.sharp, threshold=2))
    crop = lift_highlights(crop, args.lift, args.knee)

    gray = stretch(crop, tw, th, args.contrast, args.brightness)
    bits = bayer(gray, tw, th)

    if args.out_png:
        bits.save(args.out_png)

    if args.out_check:
        g = Image.new('RGB', (tw * 2 + 8, th), (24, 24, 24))
        g.paste(crop.convert('RGB'), (0, 0))
        g.paste(bits.convert('RGB'), (tw + 8, 0))
        g.resize(((tw * 2 + 8) * 3, th * 3), Image.NEAREST).save(args.out_check)

    if args.out_c:
        size, stride = to_c_array(bits, tw, th, args.out_c, os.path.basename(args.src))
        print('C array: %d 字节（%dx%d，stride %d）-> %s' % (size, tw, th, stride, args.out_c))

    print('done %dx%d' % (tw, th))


if __name__ == '__main__':
    main()
