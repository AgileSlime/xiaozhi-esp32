package com.xiaozhi.console.service;

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
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * 板子代理层：把前端的动作翻译成板子的 HTTP 请求。
 *
 * <p>板子那边的接口（固件 {@code photo_web_server.cc}）：
 * <ul>
 *   <li>{@code GET /cmd?c=status} 一句话状态</li>
 *   <li>{@code GET /cmd?c=play 晴天} 点歌（板子自己去找中转，不阻塞）</li>
 *   <li>{@code POST /upload} 原始 JPEG，直接在内存里解码显示并写进 NVS</li>
 *   <li>{@code POST /reset} 恢复内置壁纸</li>
 * </ul>
 */
@Service
public class DeviceService {

    private static final Logger log = LoggerFactory.getLogger(DeviceService.class);
    private static final Pattern STATUS_IP = Pattern.compile("IP=([^\\s]+)");
    private static final Pattern STATUS_RELAY = Pattern.compile("中转=([^\\s]+)");
    private static final Pattern STATUS_PLAYING = Pattern.compile("在放歌=([是否])");
    private static final Pattern STATUS_SRAM = Pattern.compile("内部RAM=(\\d+)");

    private final HttpClient http = HttpClient.newBuilder()
            // 板子空闲时进入 modem sleep（MAX_MODEM 省电），射频每秒才醒一次，
            // 弱网下第一次请求可能要等 1~3 秒才连上，6 秒太短会误判超时。
            .connectTimeout(Duration.ofSeconds(20))
            // 板子的 ESP-IDF httpd 不认识 HTTP/2 升级头（Upgrade: h2c），
            // 默认 h2c 会让它返回 "Bad request syntax"，必须锁死在 HTTP/1.1。
            .version(HttpClient.Version.HTTP_1_1)
            .build();

    private final EventBus events;

    @Value("${xiaozhi.device-port:80}")
    private int devicePort;

    /** 板子 IP，靠 UDP 广播回包自动填；也可以在网页上手动指定 */
    private volatile String deviceIp = "";
    private volatile long lastSeenMs = 0;

    public DeviceService(EventBus events) {
        this.events = events;
    }

    @PostConstruct
    void banner() {
        log.info("设备代理就绪，板子 IP 靠广播自动发现（UDP 8801/8802）");
    }

    // ---------------------------------------------------------------- 状态

    public void updateDeviceIp(String ip) {
        if (ip == null || ip.isBlank()) {
            return;
        }
        boolean changed = !ip.equals(deviceIp);
        deviceIp = ip;
        lastSeenMs = System.currentTimeMillis();
        if (changed) {
            events.ok("★ 板子已上线: http://" + ip + "/     （图片网页就是这个地址）");
            log.info("板子 IP: {}", ip);
        }
    }

    public String getDeviceIp() {
        return deviceIp;
    }

    public boolean isOnline() {
        return !deviceIp.isEmpty() && System.currentTimeMillis() - lastSeenMs < 60_000;
    }

    public long getLastSeenMs() {
        return lastSeenMs;
    }

    public String deviceWebUrl() {
        return deviceIp.isEmpty() ? "" : "http://" + deviceIp + "/";
    }

    // ---------------------------------------------------------------- 命令

    /** 透传一条串口命令（status / page memo / play 晴天 / reboot …） */
    public String cmd(String command) {
        String base = requireDevice();
        String url = base + "/cmd?c=" + URLEncoder.encode(command, StandardCharsets.UTF_8);
        return plain(url, Duration.ofSeconds(15));
    }

    /** 点歌：板子自己会去问中转要直链，这里只负责把命令丢过去 */
    public String play(String keyword) {
        return cmd("play " + keyword);
    }

    /** 解析 status 输出成结构化数据，前端直接渲染 */
    public Map<String, Object> status() {
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("online", isOnline());
        out.put("ip", deviceIp);
        out.put("webUrl", deviceWebUrl());
        if (deviceIp.isEmpty()) {
            out.put("error", "还没发现板子：确认板子和电脑连同一个 WiFi，并且本服务在运行");
            return out;
        }
        try {
            String raw = cmd("status");
            out.put("raw", raw);
            out.put("ip", firstGroup(STATUS_IP, raw, deviceIp));
            out.put("relay", firstGroup(STATUS_RELAY, raw, "(未设置)"));
            out.put("playing", "是".equals(firstGroup(STATUS_PLAYING, raw, "否")));
            String sram = firstGroup(STATUS_SRAM, raw, "0");
            out.put("sramKb", Integer.parseInt(sram) / 1024);
            out.put("ok", raw.contains("IP="));
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", "连不上板子（" + e.getMessage() + "）：IP 变了或者板子掉线，等下一次广播即可");
        }
        return out;
    }

    // ---------------------------------------------------------------- 图片

    /** 播放器页唱片中心头像（52×52），板子端走 /avatar，存独立 NVS */
    public String uploadAvatar(byte[] jpeg) {
        String base = requireDevice();
        HttpRequest request = HttpRequest.newBuilder(URI.create(base + "/avatar"))
                .timeout(Duration.ofSeconds(40))
                .header("Content-Type", "image/jpeg")
                .POST(HttpRequest.BodyPublishers.ofByteArray(jpeg))
                .build();
        try {
            HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
            if (response.statusCode() != 200) {
                throw new IllegalStateException("板子拒绝了这张头像（HTTP " + response.statusCode()
                        + "）：" + response.body());
            }
            events.ok("头像已上传并保存到板子（重启后仍是这张）");
            return response.body();
        } catch (IllegalStateException e) {
            throw e;
        } catch (Exception e) {
            String msg = e.getMessage();
            if (msg == null || msg.isBlank()) {
                msg = e.getClass().getSimpleName();
            }
            throw new IllegalStateException("头像上传失败：" + msg + "（板子可能掉线或 WiFi 弱，稍等重试）", e);
        }
    }

    /** 前端已经裁好并压成 JPEG（210×248，屏幕原生尺寸），这里原样转给板子 */
    public String uploadJpeg(byte[] jpeg) {
        String base = requireDevice();
        HttpRequest request = HttpRequest.newBuilder(URI.create(base + "/upload"))
                .timeout(Duration.ofSeconds(40))
                .header("Content-Type", "image/jpeg")
                .POST(HttpRequest.BodyPublishers.ofByteArray(jpeg))
                .build();
        try {
            HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
            if (response.statusCode() != 200) {
                throw new IllegalStateException("板子拒绝了这张图（HTTP " + response.statusCode()
                        + "）：" + response.body());
            }
            events.ok("图片已上传并保存到板子（重启后仍是这张）");
            return response.body();
        } catch (IllegalStateException e) {
            throw e;
        } catch (Exception e) {
            String msg = e.getMessage();
            if (msg == null || msg.isBlank()) {
                msg = e.getClass().getSimpleName();
            }
            throw new IllegalStateException("上传失败：" + msg + "（板子可能掉线或 WiFi 弱，稍等重试）", e);
        }
    }

    /** 恢复内置壁纸（板子 NVS 里那份一起删） */
    public String resetPhoto() {
        String base = requireDevice();
        HttpRequest request = HttpRequest.newBuilder(URI.create(base + "/reset"))
                .timeout(Duration.ofSeconds(15))
                .POST(HttpRequest.BodyPublishers.noBody())
                .build();
        try {
            HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
            events.warn("已让板子恢复内置壁纸");
            return response.body();
        } catch (Exception e) {
            throw new IllegalStateException("恢复失败：" + e.getMessage(), e);
        }
    }

    // ---------------------------------------------------------------- 内部

    private String requireDevice() {
        if (deviceIp.isEmpty()) {
            throw new IllegalStateException("还没发现板子，等几秒或者手动填 IP");
        }
        return "http://" + deviceIp + (devicePort == 80 ? "" : ":" + devicePort);
    }

    private String plain(String url, Duration timeout) {
        HttpRequest request = HttpRequest.newBuilder(URI.create(url))
                .timeout(timeout)
                .GET()
                .build();
        try {
            HttpResponse<String> response = http.send(request, HttpResponse.BodyHandlers.ofString());
            return response.body() == null ? "" : response.body().trim();
        } catch (Exception e) {
            // e.getMessage() 可能是 null（比如底层 ConnectException 不带消息），
            // 直接传 null 会让前端显示"命令失败: null"，这里兜底成类型名 + 明确提示。
            String msg = e.getMessage();
            if (msg == null || msg.isBlank()) {
                msg = e.getClass().getSimpleName();
            }
            // 给用户一个可操作的提示，而不是只报异常类型
            String hint = "连不上板子（板子可能掉线/重启/WiFi 弱，等几秒它自动回包重连后再试）";
            throw new IllegalStateException(msg + "：" + hint, e);
        }
    }

    private static String firstGroup(Pattern pattern, String text, String fallback) {
        Matcher matcher = pattern.matcher(text == null ? "" : text);
        return matcher.find() ? matcher.group(1) : fallback;
    }
}
