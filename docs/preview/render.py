# -*- coding: utf-8 -*-
# 复刻固件里的渲染管线，用于在电脑上预览 1-bit 屏的实际观感
from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageFilter
import math, os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))
FONT = "C:/Windows/Fonts/msyh.ttc"
FONT_B = "C:/Windows/Fonts/msyhbd.ttc"

BAYER = [
    [0,32,8,40,2,34,10,42],[48,16,56,24,50,18,58,26],
    [12,44,4,36,14,46,6,38],[60,28,52,20,62,30,54,22],
    [3,35,11,43,1,33,9,41],[51,19,59,27,49,17,57,25],
    [15,47,7,39,13,45,5,37],[63,31,55,23,61,29,53,21],
]

def make_test_photo(w=620, h=760):
    """合成一张层次丰富的测试照（天空渐变 + 太阳 + 山影 + 水面反光）"""
    sky = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(sky)
    for y in range(h):
        t = y / h
        if t < 0.55:
            v = 26 + (t / 0.55) * 190
        else:
            v = 216 - ((t - 0.55) / 0.45) * 120
        d.line([(0, y), (w, y)], fill=int(max(0, min(255, v))))
    sun = Image.new('L', (w, h), 0)
    sd = ImageDraw.Draw(sun)
    sd.ellipse([w*0.55, h*0.18, w*0.55+140, h*0.18+140], fill=255)
    sun = sun.filter(ImageFilter.GaussianBlur(34))
    sky = ImageChops.lighter(sky, sun)
    d = ImageDraw.Draw(sky)
    d.polygon([(-40, h*0.62), (w*0.22, h*0.38), (w*0.42, h*0.60),
               (w*0.66, h*0.33), (w*0.88, h*0.58), (w+40, h*0.45),
               (w+40, h), (-40, h)], fill=38)
    d.polygon([(-40, h*0.70), (w*0.30, h*0.52), (w*0.58, h*0.72),
               (w*0.80, h*0.60), (w+40, h*0.76), (w+40, h), (-40, h)], fill=22)
    top = sky.crop((0, 0, w, int(h*0.72)))
    refl = ImageOps_mirror(top).resize((w, int(h*0.28))).point(lambda p: int(p*0.55))
    sky.paste(refl, (0, int(h*0.72)))
    d = ImageDraw.Draw(sky)
    for y in range(int(h*0.72), h, 7):
        d.line([(0, y), (w, y)], fill=30)
    return sky

def ImageOps_mirror(img):
    return img.transpose(Image.FLIP_TOP_BOTTOM)

def dither_cover(src, tw, th):
    """与 photo_loader.cc 完全一致：cover 裁剪 → 灰度 → 1%/99% 拉伸 → Bayer 抖动"""
    sw, sh = src.size
    dst_ratio = tw / th
    src_ratio = sw / sh
    if src_ratio > dst_ratio:
        ch, cw = sh, int(sh * dst_ratio)
        ox, oy = (sw - cw)//2, 0
    else:
        cw, ch = sw, int(sw / dst_ratio)
        ox, oy = 0, (sh - ch)//2
    crop = src.crop((ox, oy, ox+cw, oy+ch)).resize((tw, th), Image.LANCZOS)
    px = crop.load()
    total = tw*th
    hist = [0]*256
    for y in range(th):
        for x in range(tw):
            g = px[x, y]
            hist[g] += 1
    lo, hi, acc = 0, 255, 0
    for i in range(256):
        acc += hist[i]
        if acc >= total*0.01: lo = i; break
    acc = 0
    for i in range(256):
        acc += hist[i]
        if acc >= total*0.99: hi = i; break
    span = hi - lo
    if span < 12:
        lo, hi, span = 0, 255, 255
    out = Image.new('1', (tw, th), 0)
    op = out.load()
    for y in range(th):
        brow = BAYER[y & 7]
        for x in range(tw):
            v = px[x, y]
            if span > 0:
                v = int((v - lo) * 255 / span)
                v = 0 if v < 0 else (255 if v > 255 else v)
            op[x, y] = 1 if v > brow[x & 7]*4 + 2 else 0
    return out

def f(sz, bold=False):
    return ImageFont.truetype(FONT_B if bold else FONT, sz)

def draw_home(photo_1bit, mode):
    S = 2
    W, H = 400, 300
    img = Image.new('RGB', (W*S, H*S), (11, 11, 11))
    d = ImageDraw.Draw(img)

    def R(x, y, w, h, fill, radius=0):
        box = [x*S, y*S, (x+w)*S-1, (y+h)*S-1]
        if radius:
            d.rounded_rectangle(box, radius=radius*S, fill=fill)
        else:
            d.rectangle(box, fill=fill)

    def T(x, y, s, font, fill, anchor='la'):
        d.text((x*S, y*S), s, font=font, fill=fill, anchor=anchor)

    WHITE = (255, 255, 255)
    BLACK = (11, 11, 11)

    # 状态栏
    T(10, 8, "23.5°C  48%", f(16), WHITE)
    R(260, 4, 132, 26, WHITE, 13)
    T(276, 11, "WiFi", f(13), BLACK)
    T(310, 11, "▮▮▮", f(13), BLACK)
    T(384, 9, "86%", f(16), BLACK, 'ra')

    # 图片区
    R(10, 38, 214, 252, (200, 200, 200))
    img.paste(photo_1bit.convert('RGB').resize((210*S, 248*S), Image.NEAREST), (12*S, 40*S))

    # 时间卡
    R(232, 38, 158, 102, WHITE, 12)
    T(311, 88, "11:42", f(46, True), BLACK, 'mm')
    T(311, 122, "11月17日 周一", f(16), BLACK, 'mm')

    if mode == 'idle':
        R(232, 148, 158, 142, WHITE, 12)
        T(240, 154, "提醒", f(16), BLACK)
        T(382, 154, "共 3 条", f(16), BLACK, 'ra')
        d.rectangle([240*S, 176*S, 382*S, 177*S], fill=BLACK)
        rows = [("07:30", "吃药"), ("15:00", "开会"), ("21:00", "跑步")]
        for i, (t1, t2) in enumerate(rows):
            y = 182 + i*33
            if i == 0:
                R(237, y, 148, 30, BLACK, 6)
                col = WHITE
            else:
                col = BLACK
            T(243, y+7, t1, f(16), col)
            T(310, y+7, t2, f(16), col)
    else:
        R(232, 148, 158, 142, WHITE, 12)
        R(240, 154, 92, 24, BLACK, 12)
        d.ellipse([250*S, 162*S, 257*S, 169*S], fill=WHITE)
        T(264, 157, "聆听中", f(15), WHITE)
        T(382, 157, "00:06", f(16), BLACK, 'ra')
        R(237, 180, 148, 62, BLACK, 8)
        T(245, 194, "提醒我明天早上", f(16), WHITE)
        T(245, 217, "七点半吃药…", f(16), WHITE)
        T(240, 252, "好的，已设好", f(16), BLACK)
        T(240, 272, "07:30 的提醒", f(16), BLACK)

    return img

# ============================================================
# 播放器页（400x300 / 1-bit）
# 与 music_ui.cc 的常量一一对应，改布局两边同步改
# ============================================================
INK = (0, 0, 0)
PAPER = (255, 255, 255)

PL = dict(
    # 白色胶囊头（与备忘录页同一条）
    head=(12, 4, 376, 30, 15),
    # 白卡
    card=(12, 40, 376, 176, 12),
    # 唱片（屏幕坐标）：卡片内 (14,22) + 卡片原点 (12,40)
    disc=(26, 62, 132),
    groove_r=(34, 39, 44, 49, 54, 59, 64),
    label_d=52,
    pip_r=56,
    # 右栏（屏幕坐标）
    col_x=168, col_w=208,
    title_y=52, artist_y=78, rule_y=102,
    lyric_x=164, lyric_y=(110, 138, 166), lyric_line_h=28,   # 3 行 * 28，当前句反白
    # 控制（黑底白线）
    ctrl_y=222, ctrl_h=34,
    # 进度
    bar_y=266, bar_x0=24, bar_x1=376,
    times_y=274,
)

def draw_player(playing=True, lyric_idx=1, lyrics=None, pos=0.31,
                title="夜航 · Nocturne", artist="LUMEN FIELD / 2026",
                cur="01:24", total="04:32", pips_deg=0, S=3):
    W, H = 400, 300
    img = Image.new('RGB', (W*S, H*S), INK)      # 黑底
    d = ImageDraw.Draw(img)

    def R(x, y, w, h, fill=None, outline=None, radius=0, width=1):
        box = [x*S, y*S, (x+w)*S-1, (y+h)*S-1]
        if radius:
            d.rounded_rectangle(box, radius=radius*S, fill=fill, outline=outline, width=width*S)
        else:
            d.rectangle(box, fill=fill, outline=outline, width=width*S)

    def T(x, y, s, sz, fill=INK, anchor='la', bold=False):
        d.text((x*S, y*S), s, font=f(sz, bold), fill=fill, anchor=anchor)

    if lyrics is None:
        lyrics = ["把耳朵交给夜晚", "让风替你翻页", "所有声音都慢了下来"]
    lyrics = (list(lyrics) + ["", "", ""])[:3]

    # ---------- 白卡（唱片 + 曲目 + 歌词都装在里面）----------
    kx, ky, kw, kh, kr = PL['card']
    R(kx, ky, kw, kh, fill=PAPER, radius=kr)

    # ---------- 白色胶囊头（与备忘录页同一条）----------
    hx, hy, hw, hh, hr = PL['head']
    R(hx, hy, hw, hh, fill=PAPER, radius=hr)
    T(hx + 11, hy + 8, "18:07", 14, INK, bold=True)
    for i, wlen in enumerate((13, 9, 5)):
        d.rectangle([(hx + 52)*S, (hy + 10 + i*4)*S,
                     (hx + 52 + wlen)*S - 1, (hy + 10 + i*4 + 2)*S - 1], fill=INK)
    # 居中标题：用空格模拟 letter-spacing 4px
    T(hx + hw//2, hy + hh//2, "播 放 器", 20, INK, anchor='mm', bold=True)
    T(hx + hw - 34, hy + 8, "86%", 14, INK, anchor='ra', bold=True)
    bx, by = hx + hw - 30, hy + 11
    R(bx, by, 15, 8, outline=INK, width=1)
    R(bx + 2, by + 2, 9, 4, fill=INK)
    R(bx + 15, by + 2, 2, 4, fill=INK)

    # ---------- 唱片 ----------
    dx, dy, dd = PL['disc']
    ccx, ccy = dx + dd//2, dy + dd//2
    d.ellipse([dx*S, dy*S, (dx+dd)*S-1, (dy+dd)*S-1], fill=INK)
    for r in PL['groove_r']:
        d.ellipse([(ccx-r)*S, (ccy-r)*S, (ccx+r)*S-1, (ccy+r)*S-1], outline=PAPER, width=S)
    # 中心标签
    ld = PL['label_d']
    d.ellipse([(ccx-ld//2)*S, (ccy-ld//2)*S, (ccx+ld//2)*S-1, (ccy+ld//2)*S-1], fill=PAPER)
    T(ccx, ccy - 9, "SIDE A", 14, INK, anchor='ma')
    d.ellipse([(ccx-3)*S, (ccy+10)*S, (ccx+3)*S, (ccy+16)*S-1], fill=INK)
    # 旋转指示点（3 个，转起来才看得出唱片在转）
    if pips_deg is not None:
        for k in range(3):
            a = math.radians(pips_deg + k*120)
            px_ = ccx + PL['pip_r']*math.cos(a)
            py_ = ccy + PL['pip_r']*math.sin(a)
            # 黑边 + 白心：跨在白色纹路上也不会糊掉
            d.ellipse([(px_-3.5)*S, (py_-3.5)*S, (px_+3.5)*S-1, (py_+3.5)*S-1], fill=INK)
            d.ellipse([(px_-2.5)*S, (py_-2.5)*S, (px_+2.5)*S-1, (py_+2.5)*S-1], fill=PAPER)

    # ---------- 右栏 ----------
    x0, cw2 = PL['col_x'], PL['col_w']
    T(x0, PL['title_y'], title, 20, INK, bold=True)
    T(x0, PL['artist_y'], artist, 16, INK)
    d.rectangle([x0*S, PL['rule_y']*S, (x0+cw2)*S-1, PL['rule_y']*S+S-1], fill=INK)

    # ---------- 歌词：当前句反白（黑条白字，和备忘录首行反白同一语言）----------
    lx = PL['lyric_x']
    lw = cw2 + 4
    for i, y in enumerate(PL['lyric_y']):
        if i == lyric_idx:
            R(lx, y, lw, PL['lyric_line_h'], fill=INK, radius=8)
            T(lx + 8, y + 3, lyrics[i], 20, PAPER, bold=True)
        else:
            T(lx + 8, y + 5, lyrics[i], 16, INK)

    # ---------- 控制（黑底白线，纯状态指示，不接受触摸）----------
    ctop = PL['ctrl_y']
    cymid = ctop + PL['ctrl_h']//2
    # 上一首 |◀
    d.rectangle([140*S, (ctop+8)*S, 141*S, (ctop+26)*S-1], fill=PAPER)
    d.polygon([(144*S, (cymid-8)*S), (144*S, (cymid+8)*S), (160*S, cymid*S)], fill=PAPER)
    # 播放/暂停：白圆钮 + 黑图标
    pcx, pcy = 200, cymid
    d.ellipse([(pcx-17)*S, (pcy-17)*S, (pcx+17)*S-1, (pcy+17)*S-1], fill=PAPER)
    if playing:
        d.rectangle([(pcx-6)*S, (pcy-8)*S, (pcx-2)*S-1, (pcy+8)*S-1], fill=INK)
        d.rectangle([(pcx+2)*S, (pcy-8)*S, (pcx+6)*S-1, (pcy+8)*S-1], fill=INK)
    else:
        d.polygon([((pcx-5)*S, (pcy-8)*S), ((pcx-5)*S, (pcy+8)*S), ((pcx+8)*S, pcy*S)], fill=INK)
    # 下一首 ▶|
    d.polygon([(240*S, (cymid-8)*S), (240*S, (cymid+8)*S), (256*S, cymid*S)], fill=PAPER)
    d.rectangle([258*S, (ctop+8)*S, 259*S, (ctop+26)*S-1], fill=PAPER)

    # ---------- 进度（黑底白线）----------
    bx0, bx1, by = PL['bar_x0'], PL['bar_x1'], PL['bar_y']
    d.rectangle([bx0*S, by*S, bx1*S-1, by*S+S-1], fill=PAPER)          # 轨道 1px
    wpx = int((bx1-bx0)*pos)
    if wpx > 0:
        d.rectangle([bx0*S, (by-2)*S, (bx0+wpx)*S-1, (by+2)*S-1], fill=PAPER)  # 已播 4px
    kx2 = bx0 + wpx
    d.ellipse([(kx2-4.5)*S, (by-4.5)*S, (kx2+4.5)*S-1, (by+4.5)*S-1], fill=PAPER)
    T(bx0, PL['times_y'], cur, 14, PAPER, bold=True)
    T(bx1, PL['times_y'], total, 14, PAPER, anchor='ra')

    return img


photo = make_test_photo()
d216 = dither_cover(photo, 210, 248)

# 主页预览用固件里真正编进去的默认壁纸（assets/default_photo.c 就是它的 1bpp 版本），
# 这样 README 里的截图和真机第一屏是一致的；测试渐变图仍用于演示抖动层次。
_p = os.path.join(OUT_DIR, 'default-photo-1x.png')
home_photo = Image.open(_p).convert('1') if os.path.exists(_p) else d216

draw_home(home_photo, 'idle').save(os.path.join(OUT_DIR, 'home-idle.png'))
draw_home(home_photo, 'voice').save(os.path.join(OUT_DIR, 'home-voice.png'))
d216.convert('RGB').resize((210*3, 248*3), Image.NEAREST).save(os.path.join(OUT_DIR, 'photo-dither.png'))

draw_player(playing=True, pips_deg=-20).save(os.path.join(OUT_DIR, 'music-playing.png'))
draw_player(playing=False, pips_deg=None, pos=0.52,
            lyrics=["所有声音都慢了下来", "只剩心跳在打拍子", "路灯把影子拉得很长", "我把名字写进风里"],
            cur="00:02:21").save(os.path.join(OUT_DIR, 'music-paused.png'))
# ============================================================
# 备忘录页（与 memo_ui.cc 的常量一一对应：黑底 + 白卡 + 首行反白）
# ============================================================
ML = dict(
    head=(12, 4, 376, 30, 15),
    card=(12, 40, 376, 214, 12),
    row_h=53, rows=4, pad_l=16, pad_r=16, time_w=56,
    foot_y=262, foot_h=22, cell_w=34,
)

def draw_memo(rows, page=1, pages=1, total_items=None, empty=False, S=3):
    """rows: [(text, time_or_'', highlight_bool), ...] 最多 4 条；S=1 得到真实像素"""
    W, H = 400, 300
    img = Image.new('RGB', (W*S, H*S), INK)      # 黑底
    d = ImageDraw.Draw(img)

    def R(x, y, w, h, fill=None, outline=None, radius=0, width=1):
        box = [x*S, y*S, (x+w)*S-1, (y+h)*S-1]
        if radius:
            d.rounded_rectangle(box, radius=radius*S, fill=fill, outline=outline, width=width*S)
        else:
            d.rectangle(box, fill=fill, outline=outline, width=width*S)

    def T(x, y, s, sz, fill=PAPER, anchor='la', bold=False, spacing=0):
        fnt = f(sz, bold)
        if spacing:
            # 手动字间距时 anchor 会失效，先量总宽再回退一半，才是真居中
            total = sum(d.textlength(ch, font=fnt) / S + spacing for ch in s) - spacing
            cx = x - total / 2 if anchor in ('ma', 'mm', 'md') else x
            for ch in s:
                d.text((cx*S, y*S), ch, font=fnt, fill=fill, anchor='la')
                cx += d.textlength(ch, font=fnt) / S + spacing
        else:
            d.text((x*S, y*S), s, font=fnt, fill=fill, anchor=anchor)

    # ---- 顶栏：白色胶囊 ----
    hx, hy, hw, hh, hr = ML['head']
    R(hx, hy, hw, hh, fill=PAPER, radius=hr)
    T(hx + 11, hy + 7, "18:07", 14, INK)
    for i, wlen in enumerate((13, 9, 5)):
        d.line([(hx+52)*S, (hy+10+i*4)*S, (hx+52+wlen)*S, (hy+10+i*4)*S], fill=INK, width=2*S)
    T(200, hy + 5, "备忘录", 20, INK, anchor="ma", spacing=4)
    T(hx + hw - 34, hy + 4, "86%", 14, INK, anchor='ra')
    bx, by = hx + hw - 30, hy + 11
    R(bx, by, 15, 8, outline=INK, radius=1, width=1)
    R(bx + 2, by + 2, 10, 4, fill=INK)
    R(bx + 15, by + 2, 2, 4, fill=INK)

    # ---- 列表卡 ----
    kx, ky, kw, kh, kr = ML['card']
    if rows:
        kh = min(kh, len(rows) * ML['row_h'])
    R(kx, ky, kw, kh, fill=PAPER, radius=kr)

    for i in range(ML['rows']):
        ry = ky + i * ML['row_h']
        if i >= len(rows):
            continue
        text, tm, hl = rows[i]
        px0, py0 = kx + 4, ry + 4
        pw, ph = kw - 8, ML['row_h'] - 8
        if hl:
            R(px0, py0, pw, ph, fill=INK, radius=8)
        t = text
        while d.textlength(t, font=f(16)) / S > ML['row_h'] and False:
            pass
        maxw = pw - 32 - 72
        while d.textlength(t, font=f(16)) / S > maxw and len(t) > 1:
            t = t[:-1]
        if t != text:
            t = t[:-1] + "…"
        ty = py0 + (ph - 26) // 2 + 2
        T(px0 + 12, ty, t, 20, PAPER if hl else INK)
        if tm:
            T(px0 + pw - 12, py0 + (ph - 31) // 2 + 5, tm, 24, PAPER if hl else INK, anchor='ra')

    # 分隔线：只在下面还有一条时画
    for i in range(len(rows) - 1):
        yy = ky + (i + 1) * ML['row_h'] - 1
        d.line([(kx+16)*S, yy*S, (kx+kw-16)*S, yy*S], fill=INK, width=S)

    if empty:
        T(200, 116, "还没有闹钟", 20, INK, anchor='ma')
        T(200, 152, "说「七点叫我起床」试试", 20, INK, anchor='ma')

    # ---- 页脚 ----
    n = total_items if total_items is not None else len(rows)
    foot_y = min(ML['foot_y'], ky + kh + 10)   # 页脚跟随卡片，条目少时不钉在屏底
    T(kx, foot_y + 2, "共 %d 项待办" % n, 14, PAPER)

    if pages > 1:
        px = 400 - 12 - ML['cell_w']*2
        py = foot_y
        R(px, py, ML['cell_w']*2, ML['foot_h'], outline=PAPER, radius=3, width=1)
        R(px + 1, py + 1, ML['cell_w'] - 1, ML['foot_h'] - 2, fill=PAPER)
        T(px + ML['cell_w']//2, py + 3, "%02d" % page, 14, INK, anchor='ma')
        T(px + ML['cell_w'] + ML['cell_w']//2, py + 3, "%02d" % pages, 14, PAPER, anchor='ma')

    return img


MEMO_ROWS = [
    ("和设计组过一遍 UI 稿", "12:07", True),
    ("回复微雪的技术邮件", "15:40", False),
    ("给 RLCD 板子刷一版固件", "18:20", False),
    ("每天 整理本周的排期", "21:00", False),
]

draw_memo(MEMO_ROWS, page=1, pages=3, total_items=10).save(os.path.join(OUT_DIR, 'memo-list.png'))
draw_memo([], page=1, pages=1, total_items=0, empty=True).save(os.path.join(OUT_DIR, 'memo-empty.png'))
draw_memo([("吃药", "07:30", True), ("交房租", "09:00", False)], page=1, pages=1, total_items=2).save(os.path.join(OUT_DIR, 'memo-two.png'))

# 1x 真实尺寸：直接按 S=1 逐像素渲染再阈值化，
# 这才是屏幕上 1-bit 的真实观感（3x 降采样会吃掉细笔画，不能当依据）
def save_1x(img, name):
    img.convert('L').point(lambda v: 255 if v > 128 else 0, mode='1').save(
        os.path.join(OUT_DIR, name + '-1x.png'))

save_1x(draw_memo(MEMO_ROWS, page=1, pages=3, total_items=10, S=1), 'memo-list')
save_1x(draw_memo([], page=1, pages=1, total_items=0, empty=True, S=1), 'memo-empty')
save_1x(draw_memo([("吃药", "07:30", True), ("交房租", "09:00", False)], page=1, pages=1, total_items=2, S=1), 'memo-two')
save_1x(draw_player(S=1), 'music-playing')

print("saved", OUT_DIR)
