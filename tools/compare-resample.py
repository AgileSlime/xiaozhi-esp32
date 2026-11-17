#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
对比两种缩小方式的抖动结果，确认 photo_loader.cc 改动的收益。

旧：/2 解码后，每个目标像素只取源区域中点的那一个像素（点采样）
新：浏览器缩到 420x496，固件再做 2x2 区域平均

用法：
  python compare-resample.py in.jpg [out.png]
"""
import sys

from PIL import Image

W, H = 210, 248
UPW, UPH = 420, 496

BAYER = [
    [0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21],
]


def cover(im, w, h, do_resize=True):
    """按目标比例居中裁剪；do_resize=True 时再缩放到 (w, h)"""
    sw, sh = im.size
    r = w / h
    cw, ch = sw, round(sw / r)
    if ch > sh:
        ch, cw = sh, round(sh * r)
    ox, oy = (sw - cw) // 2, (sh - ch) // 2
    out = im.crop((ox, oy, ox + cw, oy + ch))
    return out.resize((w, h), Image.LANCZOS) if do_resize else out


def stretch(pix):
    """1%/99% 分位拉伸，和固件一致"""
    hist = [0] * 256
    for v in pix:
        hist[v] += 1
    total = len(pix)
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
    if hi - lo < 12:
        return pix
    return [max(0, min(255, (v - lo) * 255 // (hi - lo))) for v in pix]


def dither(pix):
    out = Image.new('1', (W, H))
    px = out.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = 1 if pix[y * W + x] > BAYER[y & 7][x & 7] * 4 + 2 else 0
    return out


def point_sample(path):
    """旧路径：直接按格子中点取一个像素"""
    im = cover(Image.open(path).convert('L'), W, H, do_resize=False)
    sw, sh = im.size
    src = list(im.getdata())
    pix = []
    for y in range(H):
        sy = (2 * y + 1) * sh // (2 * H)
        for x in range(W):
            sx = (2 * x + 1) * sw // (2 * W)
            pix.append(src[sy * sw + sx])
    return dither(stretch(pix))


def box_sample(path):
    """新路径：浏览器 LANCZOS 缩到 2x，再 2x2 平均"""
    im = cover(Image.open(path).convert('L'), UPW, UPH)
    src = list(im.getdata())
    pix = []
    for y in range(H):
        for x in range(W):
            s = (src[(2 * y) * UPW + 2 * x] + src[(2 * y) * UPW + 2 * x + 1] +
                 src[(2 * y + 1) * UPW + 2 * x] + src[(2 * y + 1) * UPW + 2 * x + 1])
            pix.append(s // 4)
    return dither(stretch(pix))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'docs/v1/movecall-cuican-esp32s3.jpg'
    out = sys.argv[2] if len(sys.argv) > 2 else 'docs/preview/photo-resample-compare.png'

    a = point_sample(src)
    b = box_sample(src)

    S = 3
    gap = 12
    canvas = Image.new('1', (W * S * 2 + gap, H * S + 26), 1)
    canvas.paste(a.resize((W * S, H * S), Image.NEAREST), (0, 26))
    canvas.paste(b.resize((W * S, H * S), Image.NEAREST), (W * S + gap, 26))
    canvas.save(out)

    da = sum(1 for v in list(a.getdata()) if v)
    db = sum(1 for v in list(b.getdata()) if v)
    print('已输出 %s' % out)
    print('左（旧·点采样）白点 %d / %d' % (da, W * H))
    print('右（新·区域平均）白点 %d / %d' % (db, W * H))


if __name__ == '__main__':
    main()
