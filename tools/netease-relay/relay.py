#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
网易云音乐中转服务（给 ESP32 小智用的极简版）

为什么需要它
------------
设备上没有二维码、也存不住登录 cookie，播放直链还有时效，所以"登录 + 换直链"这两件事
必须放在一台常开的机器上。设备只跟这个脚本说话，脚本再去跟网易云说话。

用法
----
1. 浏览器登录 https://music.163.com
2. F12 → Application/存储 → Cookie，复制整条（至少要有 MUSIC_U=...）
3. 在本目录建 cookie.txt，把 cookie 粘进去（一行，别加引号）
4. 运行：
       python relay.py --port 8800
   或者指定 cookie 文件：  python relay.py --cookie D:\\cookie.txt
5. 设备上说一句「音乐服务器设为 192.168.1.10:8800」，或者用 curl 验证：
       curl "http://127.0.0.1:8800/play?kw=晴天"

接口
----
GET /health                      自检（是否配了 cookie）
GET /search?kw=关键词&limit=8     搜索，只返回元信息
GET /play?kw=关键词               一次拿全：直链 + 标题 + 歌手 + 歌词 + 时长

说明
----
- 只用标准库，不装任何依赖。
- 返回的直链是明文 MP3（128k），有效期约几十分钟，所以不要缓存。
- 没有 cookie 也能跑，但只能放免费歌曲；VIP 歌会返回"需要会员"。
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API = "https://music.163.com"

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/122.0 Safari/537.36")

COOKIE = ""


def log(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def api_get(path, params, timeout=10):
    """GET 网易云接口，返回 dict。失败抛异常。"""
    url = API + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    headers = {
        "User-Agent": UA,
        "Referer": "https://music.163.com/",
        "Accept": "*/*",
    }
    if COOKIE:
        headers["Cookie"] = COOKIE
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
    return json.loads(raw.decode("utf-8", "replace"))


# 翻唱 / 伴奏 / 铃声这类结果先扣分，否则搜"晴天"第一条经常是别人的翻唱
BAD_WORDS = ("原唱", "翻唱", "cover", "Cover", "伴奏", "降调", "升调",
             "铃声", "remix", "Remix", "DJ", "钢琴版", "纯音乐", "吉他版", "女声版", "男声版")


def _norm(text):
    """去掉括号内容和空白，用来判断"标题是否就是这首歌" """
    text = re.sub(r"[（(\[【].*?[）)\]】]", "", text or "")
    return re.sub(r"\s+", "", text).lower()


def pick_song(songs, keyword):
    """在搜索结果里挑一个最像用户想听的那首"""
    if not songs:
        return None
    kw = _norm(keyword)
    best, best_score = None, -10 ** 9
    for idx, s in enumerate(songs):
        title = s.get("name") or ""
        artists = "/".join(a.get("name", "") for a in (s.get("artists") or []))
        score = 0
        n = _norm(title)
        if n == kw:
            score += 50
        elif n.startswith(kw) or kw in n:
            score += 20
        if kw and kw in artists.lower():
            score += 10          # "周杰伦 晴天" 这种把歌手也打进去的
        for w in BAD_WORDS:
            if w in title:
                score -= 40
        # "弦乐版/深情版/R&B版"这类改编版几乎都不是用户想要的原曲，轻罚一下
        if "版" in title:
            score -= 15
        # 网易云自己的排序也带相关性，越靠前越可信
        score -= idx * 2
        # fee=1/4 是付费/会员曲，但那是用户自己的账号能放的，不扣分
        if s.get("status", 0) < 0:
            score -= 100
        if score > best_score:
            best, best_score = s, score
    return best


def resolve_url(song_id):
    """拿播放直链。依次试几个档位，第一个非空的就用。

    注意顺序：新版 /url/v1 接口对匿名请求会返回非 JSON（风控），
    老版 /api/song/enhance/player/url 反而最稳，所以老版排前面。
    """
    attempts = [
        ("/api/song/enhance/player/url", {"id": song_id, "ids": "[%s]" % song_id, "br": 128000}),
        ("/api/song/enhance/player/url", {"id": song_id, "ids": "[%s]" % song_id, "br": 320000}),
        ("/api/song/enhance/player/url/v1", {"id": song_id, "ids": "[%s]" % song_id, "level": "standard"}),
    ]
    for path, params in attempts:
        try:
            data = api_get(path, params)
        except Exception as e:  # noqa: BLE001
            log("  url 接口异常(%s): %s" % (path, e))
            continue
        for item in (data.get("data") or []):
            url = item.get("url")
            if url:
                return url, item.get("br") or 0
    return "", 0


def fetch_lyric(song_id):
    try:
        data = api_get("/api/song/lyric", {"id": song_id, "lv": -1, "kv": -1, "tv": -1})
    except Exception as e:  # noqa: BLE001
        log("  歌词接口异常: %s" % e)
        return ""
    return ((data.get("lrc") or {}).get("lyric") or "").strip()


def do_search(keyword, limit):
    data = api_get("/api/search/get", {
        "s": keyword, "type": 1, "limit": limit, "offset": 0,
    })
    result = data.get("result") or {}
    songs = result.get("songs") or []
    out = []
    for s in songs:
        out.append({
            "id": s.get("id"),
            "title": s.get("name") or "",
            "artist": "/".join(a.get("name", "") for a in (s.get("artists") or [])),
            "duration_ms": s.get("duration") or 0,
            "album": ((s.get("album") or {}).get("name") or ""),
        })
    return out


def do_play(keyword):
    songs = do_search(keyword, 12)
    song = pick_song(songs, keyword)
    if not song:
        return {"ok": False, "reason": "没搜到这首歌"}

    song_id = song["id"]
    url, br = resolve_url(song_id)
    if not url:
        reason = "这首歌拿不到播放地址（可能需要会员、或该地区没有版权）"
        if not COOKIE:
            reason += "；当前没配置 cookie，很多正式版歌曲搜不到也放不了"
        return {
            "ok": False,
            "reason": reason,
            "id": song_id,
            "title": song["title"],
            "artist": song["artist"],
        }

    # 没 cookie 时网易云会把正式版藏起来，搜"晴天"只能搜到翻唱，
    # 这里明确告诉调用方，免得用户以为设备坏了
    hint = ""
    if not COOKIE:
        hint = "未配置 cookie，结果可能是翻唱版"

    # 直链经常是 http 的，设备端解码只要明文 MP3，不管 http/https
    return {
        "ok": True,
        "id": song_id,
        "title": song["title"],
        "artist": song["artist"],
        "album": song["album"],
        "duration_ms": song["duration_ms"],
        "url": url,
        "br": br,
        "lyric": fetch_lyric(song_id),
        "reason": "",
        "hint": hint,
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 少打点日志，免得刷屏
        log("%s - %s" % (self.address_string(), fmt % args))

    def _send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"
        qs = urllib.parse.parse_qs(parsed.query)
        kw = (qs.get("kw") or qs.get("s") or [""])[0].strip()

        try:
            if path == "/health":
                self._send_json({"ok": True, "logged_in": bool(COOKIE), "api": API})
                return

            if path == "/search":
                if not kw:
                    self._send_json({"ok": False, "reason": "缺少 kw 参数"}, 400)
                    return
                limit = int((qs.get("limit") or ["8"])[0])
                limit = max(1, min(limit, 20))
                songs = do_search(kw, limit)
                self._send_json({"ok": True, "kw": kw, "songs": songs})
                return

            if path == "/play":
                if not kw:
                    self._send_json({"ok": False, "reason": "缺少 kw 参数"}, 400)
                    return
                log("播放请求: %s" % kw)
                result = do_play(kw)
                log("  -> %s %s" % (result.get("title", ""), "OK" if result.get("ok") else result.get("reason")))
                self._send_json(result)
                return

            if path == "/":
                self._send_json({
                    "ok": True,
                    "usage": ["/health", "/search?kw=晴天&limit=8", "/play?kw=晴天"],
                })
                return

            self._send_json({"ok": False, "reason": "未知路径"}, 404)

        except urllib.error.HTTPError as e:
            self._send_json({"ok": False, "reason": "网易云返回 HTTP %s" % e.code}, 502)
        except Exception as e:  # noqa: BLE001
            log("处理失败: %r" % e)
            self._send_json({"ok": False, "reason": "中转服务异常: %s" % e}, 500)


def load_cookie(arg_cookie):
    """优先命令行，其次同目录 cookie.txt，最后环境变量"""
    if arg_cookie and os.path.isfile(arg_cookie):
        return open(arg_cookie, encoding="utf-8").read().strip()
    if arg_cookie:
        return arg_cookie.strip()

    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.join(here, "cookie.txt")
    if os.path.isfile(default):
        return open(default, encoding="utf-8").read().strip()

    return os.environ.get("NETEASE_COOKIE", "").strip()


def main():
    global COOKIE
    ap = argparse.ArgumentParser(description="网易云音乐中转服务（给 ESP32 用）")
    ap.add_argument("--port", type=int, default=8800)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--cookie", default="", help="cookie 文件路径，或直接给 cookie 字符串")
    args = ap.parse_args()

    COOKIE = load_cookie(args.cookie)
    if COOKIE:
        # 只打印长度，别把 token 打到日志里
        has_music_u = "MUSIC_U=" in COOKIE
        log("已加载 cookie（%d 字符，MUSIC_U=%s）" % (len(COOKIE), "有" if has_music_u else "无"))
    else:
        log("⚠ 没有 cookie：只能播放免费歌曲，VIP 歌会失败。见文件头的说明。")

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    log("中转服务已启动: http://%s:%d" % (args.host, args.port))
    log("设备上可以说：音乐服务器设为 <本机局域网IP>:%d" % args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("\n已停止")


if __name__ == "__main__":
    main()
