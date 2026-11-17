#!/usr/bin/env python3
"""
小智相册 - 网页上传服务本地模拟器

目的：不接板子的情况下，验证 photo_web_server.cc 里那套东西的行为。
      - 网页 HTML 直接从 .cc 源码里提取，改固件后这里自动同步，不会对不上
      - 四个接口（/ /list /upload /delete）逐条复刻，包括 SanitizeName、
        UniquePath、扩展名白名单、8MB 上限、各错误状态码
      - "SD 卡目录" 用本地文件夹代替

用法：
    python server.py                 # 默认 http://127.0.0.1:8088
    python server.py --port 9000
    python server.py --dir D:/tmp/photos
    python server.py --no-seed       # 不预置示例图片
"""

import argparse
import json
import os
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

kMaxUploadBytes = 8 * 1024 * 1024   # 与固件 kMaxUploadBytes 一致
kChunk = 4096                       # 与固件 4KB 接收缓冲一致

HERE = Path(__file__).resolve().parent
CC_PATH = HERE.parent.parent / "main/boards/waveshare-s3-rlcd-4.2/managers/photo_web_server.cc"

BOOT = time.time()


def log(tag, msg):
    """模仿 ESP_LOGI 的串口输出格式，方便和真机日志对照。"""
    ms = int((time.time() - BOOT) * 1000)
    print("I (%d) %s: %s" % (ms, tag, msg), flush=True)


# ---------------------------------------------------------------- 提取 HTML

def load_index_html():
    """从 photo_web_server.cc 里抓 R"HTML( ... )HTML" 原文。"""
    src = CC_PATH.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'R"HTML\((.*?)\)HTML"', src, re.S)
    if not m:
        sys.exit("在 %s 里找不到 R\"HTML( )HTML\" 原文，是不是改名了？" % CC_PATH)
    return m.group(1)


# ---------------------------------------------------------------- 固件同款逻辑

def sanitize_name(raw):
    """只保留字母数字、下划线、短横线、点；非 ASCII 换 _；去开头点。"""
    base = re.split(r"[/\\]", raw)[-1]
    out = "".join(c if (c.isascii() and (c.isalnum() or c in "_-.")) else "_" for c in base)
    out = out.lstrip(".")
    return out or "photo.jpg"


def has_jpeg_ext(name):
    return name.endswith((".jpg", ".jpeg", ".JPG", ".JPEG"))


def unique_path(directory, name):
    """重名追加 _1 _2 ...，最多试 999 次，与固件一致。"""
    p = directory / name
    if not p.exists():
        return p
    stem, dot, ext = name.rpartition(".")
    if not dot:
        stem, ext = name, ""
    for i in range(1, 1000):
        p = directory / ("%s_%d%s" % (stem, i, ("." + ext) if dot else ""))
        if not p.exists():
            return p
    return p


def list_photos(directory):
    """/list 的顺序也要像固件：先 .jpg 一批，再 .jpeg 一批。"""
    if not directory.is_dir():
        return []
    names = [p.name for p in directory.iterdir() if p.is_file()]
    jpg = sorted(n for n in names if n.lower().endswith(".jpg"))
    jpeg = sorted(n for n in names if n.lower().endswith(".jpeg"))
    return jpg + jpeg


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "PhotoWebMock/1.0"
    protocol_version = "HTTP/1.1"
    html = ""
    root = Path(".")

    # 默认的 request log 太吵，换成固件那种一行日志
    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            self._send(200, self.html, "text/html; charset=utf-8")
            log("PhotoWebMock", "GET / 200（%d 字节）" % len(self.html.encode()))
        elif u.path == "/list":
            arr = list_photos(self.root)
            self._send(200, json.dumps(arr, ensure_ascii=False), "application/json")
            log("PhotoWebMock", "GET /list 200，%d 个文件" % len(arr))
        elif u.path == "/favicon.ico":
            self._send(404, "")
        else:
            self._send(404, "找不到")

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        raw_name = (q.get("name") or [""])[0]

        if u.path == "/upload":
            self._upload(raw_name)
        elif u.path == "/delete":
            self._delete(raw_name)
        else:
            self._send(404, "找不到")

    def _upload(self, raw_name):
        name = sanitize_name(raw_name) if raw_name else ""
        if not name or not has_jpeg_ext(name):
            self._send(400, "文件名无效，仅接受 .jpg / .jpeg")
            log("PhotoWebMock", "POST /upload 400：%r" % raw_name)
            return

        length = self.headers.get("Content-Length")
        if length is None:
            self._send(411, "缺少 Content-Length")
            return
        remaining = int(length)

        self.root.mkdir(parents=True, exist_ok=True)
        path = unique_path(self.root, name)
        total = 0
        too_big = False

        with open(path, "wb") as f:
            while remaining > 0:
                chunk = self.rfile.read(min(kChunk, remaining))
                if not chunk:
                    break
                remaining -= len(chunk)
                if total + len(chunk) > kMaxUploadBytes:
                    too_big = True
                    break
                f.write(chunk)
                total += len(chunk)
            # 超限时把剩余数据读掉，否则浏览器会看到连接被重置
            if too_big and remaining > 0:
                while remaining > 0:
                    skip = self.rfile.read(min(65536, remaining))
                    if not skip:
                        break
                    remaining -= len(skip)

        if too_big or total == 0:
            path.unlink(missing_ok=True)
            self._send(413 if too_big else 400, "图片超过 8MB" if too_big else "空文件")
            log("PhotoWebMock", "POST /upload %s（%d 字节）" % ("413 超限" if too_big else "400 空文件", total))
            return

        log("PhotoWeb", "收到图片 %s（%d 字节）" % (path, total))
        log("PhotoWebMock", "→ 真机上这里会触发 HomePhoto::Rescan 并立刻刷新主页")
        self._send(200, '{"ok":true}', "application/json")

    def _delete(self, raw_name):
        name = sanitize_name(raw_name) if raw_name else ""
        if not name or not has_jpeg_ext(name):
            self._send(400, "文件名无效")
            log("PhotoWebMock", "POST /delete 400：%r" % raw_name)
            return
        p = self.root / name
        if not p.is_file():
            self._send(404, "文件不存在")
            log("PhotoWebMock", "POST /delete 404：%s" % name)
            return
        p.unlink()
        log("PhotoWeb", "删除图片 %s/%s" % (self.root, name))
        self._send(200, '{"ok":true}', "application/json")


# ---------------------------------------------------------------- 示例图

def seed_photos(directory):
    """首次运行放两张示例图，省得列表空着看不出效果。"""
    if directory.is_dir() and any(directory.iterdir()):
        return
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        log("PhotoWebMock", "没装 PIL，跳过示例图（列表会是空的，不影响功能）")
        return

    directory.mkdir(parents=True, exist_ok=True)
    for idx, (name, c1, c2) in enumerate([
        ("sample-dawn.jpg", (255, 183, 94), (120, 60, 140)),
        ("sample-shore.jpg", (60, 150, 200), (10, 30, 60)),
    ]):
        w, h = 800, 600
        img = Image.new("RGB", (w, h))
        d = ImageDraw.Draw(img)
        for y in range(h):
            t = y / (h - 1)
            d.line([(0, y), (w, y)], fill=tuple(int(c1[i] * (1 - t) + c2[i] * t) for i in range(3)))
        d.ellipse([w - 220, 60, w - 100, 180], fill=(255, 250, 230))
        for y in range(360, h):
            d.line([(0, y), (w, y)], fill=(y % 7 * 3 + 20, 40 + idx * 20, 70))
        img.save(directory / name, quality=88)
    log("PhotoWebMock", "已放 2 张示例图到 %s" % directory)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--dir", default=str(HERE / "photos"))
    ap.add_argument("--no-seed", action="store_true")
    args = ap.parse_args()

    root = Path(args.dir).resolve()
    root.mkdir(parents=True, exist_ok=True)
    if not args.no_seed:
        seed_photos(root)

    Handler.html = load_index_html()
    Handler.root = root

    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    srv.daemon_threads = True
    log("PhotoWeb", "图片上传服务已启动，地址: http://127.0.0.1:%d" % args.port)
    log("PhotoWebMock", "模拟目录（替代 SD 卡 /sdcard/photos）: %s" % root)
    print("  按 Ctrl+C 停止\n", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("PhotoWeb", "图片上传服务已停止")


if __name__ == "__main__":
    main()
