package com.xiaozhi.console.service;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import jakarta.annotation.PostConstruct;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.net.URI;
import java.net.URLEncoder;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 网易云接口封装。
 *
 * <p>逻辑和原来的 relay.py 一一对应，只是换成了 Java：
 * 搜索 → 打分挑最像"原唱"的那首 → 取播放直链 → 取歌词。
 *
 * <p>两个坑（和 Python 版踩的一样，注释留着免得以后又改回去）：
 * <ul>
 *   <li>新版 {@code /api/song/enhance/player/url/v1} 对匿名请求常返回非 JSON（风控），
 *       老版 {@code /api/song/enhance/player/url} 最稳，所以老版排前面；</li>
 *   <li>没 cookie 时网易云把正式版藏起来，搜"晴天"前十条全是翻唱，所以必须打分。</li>
 * </ul>
 */
@Service
public class NeteaseService {

    private static final Logger log = LoggerFactory.getLogger(NeteaseService.class);
    private static final String API = "https://music.163.com";
    private static final String UA =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    + "(KHTML, like Gecko) Chrome/120.0 Safari/537.36";

    /** 翻唱 / 伴奏 / 铃声这类结果先扣分，否则搜"晴天"第一条经常是别人的翻唱 */
    private static final String[] BAD_WORDS = {
            "原唱", "翻唱", "cover", "Cover", "伴奏", "降调", "升调",
            "铃声", "remix", "Remix", "DJ", "钢琴版", "纯音乐", "吉他版", "女声版", "男声版"
    };

    private final ObjectMapper mapper = new ObjectMapper();

    // 用 CookieManager 自动收集所有响应的 Set-Cookie。
    // 原因：扫码登录 803 时，cookie 可能挂在中间的重定向响应上，
    // 直接读最终响应的 header 会拿不到（这就是"确认了但没登录"的原因）。
    // CookieManager 会把链路里所有 Set-Cookie 都收进来，从这里取最可靠。
    private final java.net.CookieManager cookieManager = new java.net.CookieManager();

    private final HttpClient http = HttpClient.newBuilder()
            .connectTimeout(Duration.ofSeconds(8))
            .cookieHandler(cookieManager)
            .followRedirects(HttpClient.Redirect.NORMAL)
            .build();

    @Value("${xiaozhi.cookie-file:cookie.txt}")
    private String cookieFile;

    private volatile String cookie = "";

    @PostConstruct
    void loadCookie() {
        for (String candidate : new String[]{cookieFile, "tools/netease-relay/cookie.txt"}) {
            try {
                Path path = Path.of(candidate);
                if (Files.isReadable(path)) {
                    String text = Files.readString(path, StandardCharsets.UTF_8).trim();
                    if (!text.isEmpty()) {
                        // 容错：很多人只复制了 MUSIC_U 的"值"（一长串以 00 开头的字符），
                        // 漏掉前面的 "MUSIC_U="。这里自动补上，免得到手就是 WARN。
                        if (!text.contains("=") && text.length() >= 60) {
                            text = "MUSIC_U=" + text;
                            log.info("cookie 文件只有值没有字段名，已自动补成 MUSIC_U=...");
                        }
                        cookie = text;

                        // 关键校验：没有 MUSIC_U 的 cookie 等于未登录。
                        // 很多人从 document.cookie 里复制，而 MUSIC_U 是 HttpOnly，
                        // 那里根本看不到 —— 结果贴了一大串却没登录态。
                        if (!cookie.contains("MUSIC_U=")) {
                            log.warn("cookie 里没有 MUSIC_U（当前 {} 字符只有其他字段），"
                                    + "这等于未登录，原唱会被网易云藏起来，只能搜到翻唱。"
                                    + "正确取法：F12 → Application → Cookies → music.163.com，"
                                    + "找 MUSIC_U 那一行复制它的 Value",
                                    cookie.length());
                        } else {
                            log.info("已加载网易云 cookie（{} 字符，MUSIC_U=有）", cookie.length());
                        }
                        return;
                    }
                }
            } catch (Exception e) {
                log.debug("读取 cookie 失败: {}", e.getMessage());
            }
        }
        log.warn("没有 cookie：只能搜到免费曲库（多是翻唱），VIP 歌放不了。"
                + " 浏览器登录 music.163.com 后把 Cookie 存到 {}", cookieFile);
    }

    public boolean hasCookie() {
        return !cookie.isEmpty();
    }

    // ---------------------------------------------------------------- 扫码登录
    //
    // 网易云的 qrcode 接口不需要 weapi 加密，直接 GET 就能用：
    //   1) unikey        → 拿到一个 key
    //   2) 二维码内容     → https://music.163.com/login?codekey=<key>
    //   3) client/login  → 轮询：801 等待 / 802 已扫码待确认 / 803 成功 / 800 过期
    // 803 时响应头会带 MUSIC_U，把 Set-Cookie 拼成一行存下来即可。

    private static final String QR_UA =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                    + "(KHTML, like Gecko) Chrome/120.0 Safari/537.36";

    /** 上一次的扫码状态码，只用于避免重复打日志 */
    private int lastQrCode_ = -1;

    /**
     * 当前唯一有效的二维码 key。
     *
     * 为什么需要：浏览器可能同时开着多个标签页，每个都在轮询自己那个 key。
     * 旧 key 早就失效，继续请求只会不断触发网易云风控（8821），
     * 连带着把新 key 的登录也搅黄（实测：802 之后拿不到 803）。
     *
     * 所以后端只认最新一个：旧 key 直接返回 800，一个请求都不发给网易云。
     */
    private volatile String currentQrKey_ = "";

    /** 申请二维码：返回 key 和要编码进二维码的 URL */
    public Map<String, Object> qrNew() {
        Map<String, Object> out = new LinkedHashMap<>();
        try {
            HttpRequest req = HttpRequest.newBuilder(
                            URI.create(API + "/api/login/qrcode/unikey?type=1"))
                    .timeout(Duration.ofSeconds(10))
                    .header("Referer", API + "/")
                    .header("User-Agent", QR_UA)
                    .GET()
                    .build();
            HttpResponse<String> resp = http.send(req, HttpResponse.BodyHandlers.ofString());
            JsonNode n = mapper.readTree(resp.body());
            if (n.path("code").asInt() != 200) {
                out.put("ok", false);
                out.put("error", "申请二维码失败：" + resp.body());
                return out;
            }
            String key = n.path("unikey").asText();
            currentQrKey_ = key;   // 只有最新的 key 有效
            lastQrCode_ = -1;      // 新二维码，重置状态记录
            log.info("二维码已生成: key={}", key);
            out.put("ok", true);
            out.put("key", key);
            out.put("qrContent", API + "/login?codekey=" + key);
            return out;
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", "申请二维码失败：" + e.getMessage());
            return out;
        }
    }

    /** 轮询扫码状态；803 时自动把 cookie 存下来并立即生效 */
    public Map<String, Object> qrCheck(String key) {
        Map<String, Object> out = new LinkedHashMap<>();
        if (key == null || key.isBlank()) {
            out.put("ok", false);
            out.put("error", "缺少 key");
            return out;
        }
        // 只认最新那个 key。旧标签页/旧定时器拿着过期 key 来问，
        // 直接判失效，绝不转发给网易云——否则会触发风控（8821）拖累新 key。
        if (!key.equals(currentQrKey_)) {
            out.put("ok", true);
            out.put("code", 800);
            out.put("message", "二维码已失效（已有更新的二维码）");
            return out;
        }
        try {
            String url = API + "/api/login/qrcode/client/login?key="
                    + URLEncoder.encode(key, StandardCharsets.UTF_8) + "&type=1";
            HttpRequest req = HttpRequest.newBuilder(URI.create(url))
                    .timeout(Duration.ofSeconds(10))
                    .header("Referer", API + "/")
                    .header("User-Agent", QR_UA)
                    .GET()
                    .build();
            HttpResponse<String> resp = http.send(req, HttpResponse.BodyHandlers.ofString());
            JsonNode n = mapper.readTree(resp.body());
            int code = n.path("code").asInt();
            out.put("code", code);

            // 只在状态变化时打日志，避免每 2.5 秒刷屏
            if (code != lastQrCode_) {
                lastQrCode_ = code;
                log.info("扫码状态变化: code={} body={}", code, resp.body());
            }

            if (code == 803) {
                // 优先从 CookieManager 取（能覆盖重定向链路里的 Set-Cookie），
                // 拿不到再退回读最终响应头。
                String c = extractFromCookieManager();
                if (c == null || !c.contains("MUSIC_U")) {
                    String fromHeader = extractSetCookie(resp);
                    if (fromHeader != null && fromHeader.contains("MUSIC_U")) {
                        c = fromHeader;
                    }
                }
                log.info("扫码成功，CookieManager 里 cookie={}，最终响应 Set-Cookie {} 条，含 MUSIC_U={}",
                        c == null ? "空" : c.length() + "字符",
                        resp.headers().allValues("set-cookie").size(),
                        c != null && c.contains("MUSIC_U") ? "是" : "否");
                if (c != null && c.contains("MUSIC_U")) {
                    applyCookie(c);
                    out.put("ok", true);
                    out.put("message", "登录成功，cookie 已保存");
                    out.put("cookieLen", c.length());
                } else {
                    out.put("ok", false);
                    out.put("error", "登录成功但没拿到 MUSIC_U，请重试");
                }
            } else {
                out.put("ok", true);
                out.put("message", code == 800 ? "二维码已过期，请刷新"
                        : code == 802 ? "已扫码，请在手机上确认"
                        : "等待扫码");
            }
            return out;
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", "查询扫码状态失败：" + e.getMessage());
            return out;
        }
    }

    /** 从 CookieManager 取所有 cookie 拼成一行 */
    private String extractFromCookieManager() {
        List<java.net.HttpCookie> list = cookieManager.getCookieStore().getCookies();
        if (list.isEmpty()) {
            return null;
        }
        StringBuilder sb = new StringBuilder();
        for (java.net.HttpCookie ck : list) {
            if (sb.length() > 0) {
                sb.append("; ");
            }
            sb.append(ck.getName()).append("=").append(ck.getValue());
        }
        return sb.toString();
    }

    /** 把响应里的 Set-Cookie 拼成一行 cookie（只取 key=value） */
    private String extractSetCookie(HttpResponse<String> resp) {
        List<String> raw = resp.headers().allValues("set-cookie");
        StringBuilder sb = new StringBuilder();
        for (String sc : raw) {
            int semi = sc.indexOf(';');
            String kv = (semi > 0 ? sc.substring(0, semi) : sc).trim();
            if (kv.isEmpty()) {
                continue;
            }
            if (sb.length() > 0) {
                sb.append("; ");
            }
            sb.append(kv);
        }
        return sb.length() > 0 ? sb.toString() : null;
    }

    /** 更新内存里的 cookie 并写回文件，立即生效，不用重启 */
    public void applyCookie(String c) {
        this.cookie = c;
        try {
            Files.writeString(Path.of(cookieFile), c, StandardCharsets.UTF_8);
            log.info("网易云 cookie 已更新并写入 {}", cookieFile);
        } catch (Exception e) {
            log.warn("cookie 写入文件失败: {}", e.getMessage());
        }
    }

    // ---------------------------------------------------------------- 对外

    /** 搜索结果（给网页的列表用） */
    public List<Map<String, Object>> search(String keyword, int limit) {
        return toSongs(searchRaw(keyword, limit), keyword);
    }

    /** 归一化：小写、去空白、去括号内容（"(Live)"、"(翻唱版)" 这类） */
    private static String normText(String s) {
        if (s == null) {
            return "";
        }
        String t = s.toLowerCase();
        t = t.replaceAll("[（(\\[【].*?[)）\\]】]", "");
        t = t.replaceAll("[\\s\\u3000·・\\-—_]+", "");
        return t.trim();
    }

    /** 明显的"非原唱"标记 */
    private static final String[] COVER_MARKERS = {
            "翻唱", "cover", "dj", "remix", "伴奏", "纯音乐", "钢琴版", "吉他版",
            "女声版", "男声版", "童声", "钢琴", "片段", "试听", "铃声", "抖音",
            "live", "现场", "演唱会", "karaoke", "instrumental"
    };

    /**
     * 给一首歌打"像不像原唱"的分。
     *
     * 网易云默认搜索排序对翻唱非常友好（尤其没 cookie 时），
     * 所以必须自己重排：歌名精确匹配优先、歌手出现在搜索词里加分、
     * 带翻唱/DJ/伴奏字样的狠扣分、时长过短的扣分。
     */
    private int scoreSong(JsonNode song, String keyword) {
        int score = 0;
        String name = song.path("name").asText("");
        String album = song.path("album").path("name").asText("");
        String artist = artistsOf(song);
        long duration = song.path("duration").asLong(0);

        String kw = normText(keyword);
        String n = normText(name);
        String a = normText(artist);

        // 1. 歌名匹配度（权重最高，和 pickSong 保持一致）
        if (!kw.isEmpty() && !n.isEmpty()) {
            if (n.equals(kw)) {
                score += 200;                    // 完全一致
            } else if (n.contains(kw) || kw.contains(n)) {
                score += 100;                    // 互相包含
            } else {
                score -= 100;                    // 歌名不沾边
            }
        }

        // 2. 搜索词里带歌手名 → 说明用户想找这个人的版本
        //    同样要考虑中日文差异（"宇多田光" ↔ "宇多田ヒカル"），用前 3 字兜底
        if (!a.isEmpty() && !kw.isEmpty()) {
            if (kw.contains(a)) {
                score += 90;
            } else {
                String head = a.length() >= 3 ? a.substring(0, 3) : a;
                if (head.length() >= 2 && kw.contains(head)) {
                    score += 60;
                }
            }
        }
        // 歌手名本身出现在歌名里（如"周杰伦-晴天"）
        if (!a.isEmpty() && !n.isEmpty() && n.contains(a)) {
            score += 20;
        }

        // 3. 翻唱/DJ/伴奏等标记：名字或专辑里出现就重罚
        String hay = (name + " " + album).toLowerCase();
        for (String m : COVER_MARKERS) {
            if (hay.contains(m)) {
                score -= 70;
                break;
            }
        }

        // 4. 时长：正片一般 2~6 分钟，太短多半是片段/铃声
        if (duration > 0) {
            if (duration < 90_000) {
                score -= 50;
            } else if (duration >= 150_000 && duration <= 360_000) {
                score += 15;
            }
        }

        // 5. 有版权/可播（st >= 0 表示有版权）加分
        JsonNode priv = song.path("privilege");
        if (!priv.isMissingNode() && priv.path("st").asInt(-1) >= 0) {
            score += 10;
        }

        return score;
    }

    private List<Map<String, Object>> toSongs(JsonNode data, String keyword) {
        // 先收集再排序：网易云的默认顺序对翻唱太友好，必须重排
        List<Map<String, Object>> scored = new ArrayList<>();
        List<Integer> scores = new ArrayList<>();

        for (JsonNode song : data.path("result").path("songs")) {
            Map<String, Object> item = new LinkedHashMap<>();
            item.put("id", song.path("id").asLong());
            item.put("title", song.path("name").asText(""));
            item.put("artist", artistsOf(song));
            item.put("album", song.path("album").path("name").asText(""));
            item.put("duration_ms", song.path("duration").asLong(0));
            scored.add(item);
            scores.add(scoreSong(song, keyword));
        }

        // 稳定排序：分数高在前，同分保持网易云原顺序
        List<Integer> order = new ArrayList<>();
        for (int i = 0; i < scored.size(); i++) {
            order.add(i);
        }
        order.sort((x, y) -> Integer.compare(scores.get(y), scores.get(x)));

        List<Map<String, Object>> out = new ArrayList<>(scored.size());
        for (int idx : order) {
            out.add(scored.get(idx));
        }

        if (!scored.isEmpty()) {
            log.info("搜索「{}」：{} 条结果，Top1 = {} - {}（分 {}）",
                    keyword, scored.size(),
                    out.get(0).get("title"), out.get(0).get("artist"),
                    scores.get(order.get(0)));
        }
        return out;
    }

    private JsonNode searchRaw(String keyword, int limit) {
        return apiGet("/api/search/get", Map.of(
                "s", keyword,
                "type", "1",
                "limit", String.valueOf(limit),
                "offset", "0"));
    }

    /** 点歌：返回 Firmware 需要的那份 JSON（url / title / artist / lyric / ok / reason） */
    public Map<String, Object> play(String keyword) {
        JsonNode raw = searchRaw(keyword, 12);
        JsonNode picked = pickSong(raw, keyword);

        Map<String, Object> result = new LinkedHashMap<>();
        if (picked == null) {
            result.put("ok", false);
            result.put("reason", "没搜到这首歌");
            return result;
        }

        long id = picked.path("id").asLong();
        String title = picked.path("name").asText("");
        String artist = artistsOf(picked);

        String[] urlAndBr = resolveUrl(id);
        if (urlAndBr[0].isEmpty()) {
            String reason = "这首歌拿不到播放地址（可能需要会员、或该地区没有版权）";
            if (!hasCookie()) {
                reason += "；当前没配置 cookie，很多正式版歌曲搜不到也放不了";
            }
            result.put("ok", false);
            result.put("id", id);
            result.put("title", title);
            result.put("artist", artist);
            result.put("reason", reason);
            return result;
        }

        result.put("ok", true);
        result.put("id", id);
        result.put("title", title);
        result.put("artist", artist);
        result.put("album", picked.path("album").path("name").asText(""));
        // 封面：搜索接口 /api/search/get 只给 picId 不给 picUrl，这里用歌曲 id
        // 单独调 /api/song/detail 拿完整封面 URL（兜底，失败就给空串，板子显示 SIDE A）。
        result.put("cover", fetchCover(id));
        result.put("duration_ms", picked.path("duration").asLong(0));
        result.put("url", urlAndBr[0]);
        result.put("br", Integer.parseInt(urlAndBr[1]));
        result.put("lyric", fetchLyric(id));
        result.put("reason", "");
        result.put("hint", hasCookie() ? "" : "未配置 cookie，结果可能是翻唱版");
        return result;
    }

    // ---------------------------------------------------------------- 内部

    private JsonNode pickSong(JsonNode data, String keyword) {
        JsonNode songs = data.path("result").path("songs");
        String kw = normalize(keyword);
        JsonNode best = null;
        int bestScore = Integer.MIN_VALUE;
        java.util.List<String> candidates = new java.util.ArrayList<>();

        // ---- 第一轮：硬过滤 ----
        //
        // 原唱和翻唱常常同名（"One Last Kiss" 都是这个名），只靠打分容易被
        // 翻唱反超（实测：翻唱 120 分 > 原唱 110 分）。所以先做一次硬过滤：
        // 搜索词里出现某歌手名的前 3 个字（"宇多田光" ↔ "宇多田ヒカル"），
        // 就只在"歌手对得上"的结果里挑，从根上排除同名翻唱。
        java.util.List<JsonNode> pool = new java.util.ArrayList<>();
        if (!kw.isEmpty()) {
            for (JsonNode s : songs) {
                String a = normalize(artistsOf(s));
                if (a.isEmpty()) {
                    continue;
                }
                String head = a.length() >= 3 ? a.substring(0, 3) : a;
                if (kw.contains(a) || (head.length() >= 2 && kw.contains(head))) {
                    pool.add(s);
                }
            }
        }
        if (pool.isEmpty()) {
            // 搜索词里没有歌手线索（只搜了歌名），用全部候选
            for (JsonNode s : songs) {
                pool.add(s);
            }
        } else {
            log.info("搜索词带歌手线索，候选从 {} 条缩到 {} 条", songs.size(), pool.size());
        }

        for (JsonNode song : pool) {
            String title = song.path("name").asText("");
            String album = song.path("album").path("name").asText("");
            String artists = artistsOf(song);
            long duration = song.path("duration").asLong(0);
            int score = 0;

            String n = normalize(title);
            String a = normalize(artists);

            // ---- 1. 歌名匹配（权重最高）----
            //
            // 歌名是"用户到底要哪首"的第一判据。歌手只用来在同名版本里挑原唱，
            // 不能让同专辑的其他歌靠歌手分反超 —— 之前就出过这问题：
            // 搜「宇多田光 one last kiss」结果选中了同专辑的
            // "Merry Christmas Mr. Lawrence - FYI"。
            if (n.equals(kw)) {
                score += 200;                      // 完全一致
            } else if (n.contains(kw) || kw.contains(n)) {
                score += 100;                      // 互相包含（搜索词里带了歌手名也算）
            } else {
                score -= 100;                      // 歌名完全不沾边，基本不可能是目标
            }

            // ---- 2. 歌手匹配：区分原唱和翻唱的关键 ----
            //
            // 原唱和翻唱的歌名往往一模一样（"One Last Kiss" 都是这个名），
            // 单看歌名永远打平。用户搜「宇多田 One Last Kiss」时，
            // 只有原唱的歌手名能匹配上搜索词，这一项要压过其他所有因素。
            //
            // 注意中日文差异：用户说"宇多田光"，网易云写"宇多田ヒカル"，
            // 整串匹配会失败，所以再用"歌手名前 3 字"做一次模糊匹配。
            if (!a.isEmpty() && !kw.isEmpty()) {
                if (kw.contains(a)) {
                    score += 120;          // 搜索词里带完整歌手名 → 几乎锁定
                } else {
                    String head = a.length() >= 3 ? a.substring(0, 3) : a;
                    if (head.length() >= 2 && kw.contains(head)) {
                        score += 90;       // "宇多田光" ↔ "宇多田ヒカル"，靠前 3 字认出来
                    } else if (a.contains(kw)) {
                        score += 40;       // 只搜了歌手名的一部分
                    }
                }
            }

            // ---- 3. 翻唱/改编标记（歌名和专辑一起查）----
            String hay = (title + " " + album).toLowerCase();
            for (String word : BAD_WORDS) {
                if (hay.contains(word.toLowerCase())) {
                    score -= 60;
                    break;
                }
            }
            if (title.contains("版")) {
                score -= 30;   // "弦乐版/深情版"这类改编版几乎不是用户想要的原曲
            }
            if (album.contains("翻唱") || album.toLowerCase().contains("cover")) {
                score -= 40;   // 翻唱合集专辑
            }

            // ---- 4. 时长：正片 2.5~6 分钟；太短是片段，太长是混音 ----
            if (duration > 0) {
                if (duration < 90_000) {
                    score -= 60;
                } else if (duration > 480_000) {
                    score -= 30;
                } else if (duration >= 150_000 && duration <= 360_000) {
                    score += 20;
                }
            }

            // ---- 5. 无版权直接否决 ----
            if (song.path("status").asInt(0) < 0) {
                score -= 100;
            }

            // 注意：这里**故意不再减 index**。
            // 原来有 `score -= index * 2`，等于把网易云的排序当相关性，
            // 而网易云对没版权的原唱会降权、把翻唱顶到前面 ——
            // 这一项恰恰在帮翻唱赢，已删除。

            candidates.add(score + "|" + title + "|" + artists);

            if (score > bestScore) {
                best = song;
                bestScore = score;
            }
        }

        if (best != null) {
            // 打印全部候选的得分，方便排查"为什么选了这首"
            java.util.List<String> sorted = new java.util.ArrayList<>(candidates);
            sorted.sort((x, y) -> Integer.compare(
                    Integer.parseInt(y.split("\\|")[0]),
                    Integer.parseInt(x.split("\\|")[0])));
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < Math.min(5, sorted.size()); i++) {
                sb.append("\n    ").append(i + 1).append(". ").append(sorted.get(i));
            }
            log.info("选曲「{}」→ 《{}》- {}（得分 {}，共 {} 条候选，前 5:{}）",
                    keyword,
                    best.path("name").asText(""),
                    artistsOf(best),
                    bestScore,
                    songs.size(),
                    sb);
        }
        return best;
    }

    private String[] resolveUrl(long songId) {
        // 128k 优先：64k/96k 的低码率流网易云会用 48000Hz 采样，板子的 MP3 解码器
        // 解不了（error:7），反而放不出来。128k 是 44100Hz，解码器能吃。
        // 320k 备选（44100Hz，更稳但流更大）。
        String[][] attempts = {
                {"/api/song/enhance/player/url", "128000"},
                {"/api/song/enhance/player/url", "320000"},
        };
        for (String[] attempt : attempts) {
            try {
                JsonNode data = apiGet(attempt[0], Map.of(
                        "id", String.valueOf(songId),
                        "ids", "[" + songId + "]",
                        "br", attempt[1]));
                for (JsonNode item : data.path("data")) {
                    String url = item.path("url").asText("");
                    if (!url.isEmpty()) {
                        return new String[]{url, String.valueOf(item.path("br").asInt(128000))};
                    }
                }
            } catch (Exception e) {
                log.warn("取直链失败({} br={}): {}", attempt[0], attempt[1], e.getMessage());
            }
        }
        // 最后再试 v1（有 cookie 时它反而好用）
        try {
            JsonNode data = apiGet("/api/song/enhance/player/url/v1", Map.of(
                    "id", String.valueOf(songId),
                    "ids", "[" + songId + "]",
                    "level", "standard"));
            for (JsonNode item : data.path("data")) {
                String url = item.path("url").asText("");
                if (!url.isEmpty()) {
                    return new String[]{url, String.valueOf(item.path("br").asInt(128000))};
                }
            }
        } catch (Exception e) {
            log.warn("取直链失败(v1): {}", e.getMessage());
        }
        return new String[]{"", "0"};
    }

    private String fetchLyric(long songId) {
        try {
            JsonNode data = apiGet("/api/song/lyric", Map.of(
                    "id", String.valueOf(songId), "lv", "-1", "kv", "-1", "tv", "-1"));
            return data.path("lrc").path("lyric").asText("").trim();
        } catch (Exception e) {
            log.warn("取歌词失败: {}", e.getMessage());
            return "";
        }
    }

    /** 专辑封面：搜索接口不给 picUrl，用歌曲 id 调 song/detail 拿完整封面 URL */
    private String fetchCover(long songId) {
        try {
            JsonNode data = apiGet("/api/song/detail", Map.of("ids", "[" + songId + "]"));
            JsonNode album = data.path("songs").get(0).path("album");
            String pic = album.path("picUrl").asText("");
            if (!pic.isEmpty()) {
                return pic;
            }
            // 兜底：blurPicUrl 也能用（模糊版封面，比没有强）
            return album.path("blurPicUrl").asText("");
        } catch (Exception e) {
            log.warn("取封面失败: {}", e.getMessage());
            return "";
        }
    }

    private JsonNode apiGet(String path, Map<String, String> params) {
        StringBuilder url = new StringBuilder(API).append(path);
        if (!params.isEmpty()) {
            url.append('?');
            boolean first = true;
            for (Map.Entry<String, String> e : params.entrySet()) {
                if (!first) {
                    url.append('&');
                }
                first = false;
                url.append(URLEncoder.encode(e.getKey(), StandardCharsets.UTF_8))
                        .append('=')
                        .append(URLEncoder.encode(e.getValue(), StandardCharsets.UTF_8));
            }
        }
        HttpRequest.Builder builder = HttpRequest.newBuilder(URI.create(url.toString()))
                .timeout(Duration.ofSeconds(12))
                .header("User-Agent", UA)
                .header("Referer", "https://music.163.com/")
                .header("Accept", "*/*")
                .GET();
        if (hasCookie()) {
            builder.header("Cookie", cookie);
        }
        try {
            HttpResponse<String> response = http.send(builder.build(),
                    HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
            JsonNode node = mapper.readTree(response.body());
            return node;
        } catch (Exception e) {
            throw new IllegalStateException("网易云接口异常(" + path + "): " + e.getMessage(), e);
        }
    }

    private static String artistsOf(JsonNode song) {
        List<String> names = new ArrayList<>();
        for (JsonNode artist : song.path("artists")) {
            names.add(artist.path("name").asText(""));
        }
        return String.join("/", names);
    }

    /** 去掉括号内容和空白，用来判断"标题是否就是这首歌" */
    private static String normalize(String text) {
        if (text == null) {
            return "";
        }
        return text.replaceAll("[（(\\[【].*?[）)\\]】]", "")
                .replaceAll("\\s+", "")
                .toLowerCase();
    }
}
