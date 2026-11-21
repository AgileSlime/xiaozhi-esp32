package com.xiaozhi.console.web;

import com.xiaozhi.console.service.NeteaseService;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 给板子用的中转接口。路径和参数刻意与原来的 relay.py 完全一致，
 * 所以固件里的 MusicStream 一行都不用改。
 *
 * <pre>
 *   GET /health                → {"ok":true,"logged_in":false}
 *   GET /search?kw=晴天&limit=8 → 搜索结果
 *   GET /play?kw=晴天           → {"ok":true,"url":"...","lyric":"..."}
 * </pre>
 */
@RestController
public class RelayController {

    private static final Logger log = LoggerFactory.getLogger(RelayController.class);

    private final NeteaseService netease;

    public RelayController(NeteaseService netease) {
        this.netease = netease;
    }

    @GetMapping("/health")
    public Map<String, Object> health() {
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("ok", true);
        body.put("logged_in", netease.hasCookie());
        body.put("service", "xiaozhi-console");
        return body;
    }

    @GetMapping("/search")
    public Map<String, Object> search(@RequestParam("kw") String keyword,
                                      @RequestParam(value = "limit", defaultValue = "8") int limit) {
        Map<String, Object> body = new LinkedHashMap<>();
        try {
            List<Map<String, Object>> songs = netease.search(keyword, Math.min(Math.max(limit, 1), 30));
            body.put("ok", true);
            body.put("songs", songs);
        } catch (Exception e) {
            log.warn("搜索失败: {}", e.getMessage());
            body.put("ok", false);
            body.put("reason", e.getMessage());
        }
        return body;
    }

    @GetMapping("/play")
    public Map<String, Object> play(@RequestParam("kw") String keyword) {
        long start = System.currentTimeMillis();
        try {
            Map<String, Object> result = netease.play(keyword);
            long cost = System.currentTimeMillis() - start;
            if (Boolean.TRUE.equals(result.get("ok"))) {
                log.info("点歌成功《{}》- {}（{} ms）", result.get("title"), result.get("artist"), cost);
            } else {
                log.warn("点歌失败《{}》: {}", keyword, result.get("reason"));
            }
            return result;
        } catch (Exception e) {
            log.warn("点歌异常: {}", e.getMessage());
            Map<String, Object> body = new LinkedHashMap<>();
            body.put("ok", false);
            body.put("reason", "中转出错：" + e.getMessage());
            return body;
        }
    }

    /** 兼容原来 relay.py 的固定书签入口：永远指向板子当前网页，见 DevicePageController */
}
