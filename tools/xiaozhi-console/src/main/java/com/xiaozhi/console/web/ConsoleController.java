package com.xiaozhi.console.web;

import com.xiaozhi.console.service.DeviceService;
import com.xiaozhi.console.service.DiscoveryService;
import com.xiaozhi.console.service.EventBus;
import com.xiaozhi.console.service.NeteaseService;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.multipart.MultipartFile;
import org.springframework.web.servlet.mvc.method.annotation.SseEmitter;

import javax.imageio.ImageIO;
import java.awt.Graphics2D;
import java.awt.image.BufferedImage;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 给前端 Vue 面板用的 REST 接口。
 *
 * <pre>
 *   GET  /api/state        一次性拿到设备/中转/本机 IP，给页面首屏
 *   GET  /api/status       透传板子的 status 命令，结构化结果
 *   GET  /api/logs         SSE 长连接，所有事件（发现/上传/点歌）从这里推送
 *   POST /api/cmd?c=...    透传任意串口命令（page memo / reboot …）
 *   POST /api/play?kw=     点歌（板子自己去找中转）
 *   POST /api/upload       上传图片，前端不必裁剪，这里把 JPEG 缩到 210x248（屏幕原生尺寸）再丢给板子
 *   POST /api/reset        恢复内置壁纸（NVS 那份一起删）
 *   GET  /api/music/search 网易云搜索（网页里展示候选）
 * </pre>
 */
@RestController
@RequestMapping("/api")
public class ConsoleController {

    private static final Logger log = LoggerFactory.getLogger(ConsoleController.class);

    /**
     * 直接按屏幕照片位的原生像素尺寸出图（210×248）。
     *
     * 以前这里是 420×496（2 倍），让板子自己缩一半。问题是 esp_jpeg 缩放后的
     * 实际行宽不一定等于请求值，板子端只能靠"输出字节数反推宽高"来采样，
     * 一旦差几个像素整幅图就会错位（表现为"显示的是图片局部、还糊"）。
     *
     * 改成直接出 210×248：板子 1:1 解码，既不用缩放也不用反推，
     * 解码缓冲从 625KB 降到 156KB，画质还是像素对齐的。
     */
    private static final int PHOTO_W = 210;
    private static final int PHOTO_H = 248;
    /** 播放器页唱片中心头像：板子端 1:1 抖动，不缩放 */
    private static final int AVATAR_W = 52;
    private static final int AVATAR_H = 52;

    private final DeviceService deviceService;
    private final DiscoveryService discoveryService;
    private final NeteaseService neteaseService;
    private final EventBus events;

    public ConsoleController(DeviceService deviceService,
                             DiscoveryService discoveryService,
                             NeteaseService neteaseService,
                             EventBus events) {
        this.deviceService = deviceService;
        this.discoveryService = discoveryService;
        this.neteaseService = neteaseService;
        this.events = events;
    }

    // ---------------------------------------------------------------- 首屏

    @GetMapping("/state")
    public Map<String, Object> state() {
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("online", deviceService.isOnline());
        out.put("deviceIp", deviceService.getDeviceIp());
        out.put("deviceWebUrl", deviceService.deviceWebUrl());
        out.put("lastSeenMs", deviceService.getLastSeenMs());
        out.put("localIps", discoveryService.getLocalIps());
        out.put("hasCookie", neteaseService.hasCookie());
        return out;
    }

    @GetMapping("/status")
    public Map<String, Object> status() {
        return deviceService.status();
    }

    @GetMapping("/logs")
    public SseEmitter logs() {
        return events.subscribe();
    }

    // ---------------------------------------------------------------- 命令

    @PostMapping("/cmd")
    public Map<String, Object> cmd(@RequestParam("c") String command) {
        Map<String, Object> out = new LinkedHashMap<>();
        try {
            String result = deviceService.cmd(command);
            out.put("ok", true);
            out.put("output", result);
            events.info("命令: " + command + " → " + firstLine(result));
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", e.getMessage());
            events.warn("命令失败: " + e.getMessage());
        }
        return out;
    }

    @PostMapping("/play")
    public Map<String, Object> play(@RequestParam("kw") String keyword) {
        Map<String, Object> out = new LinkedHashMap<>();
        try {
            String result = deviceService.play(keyword);
            out.put("ok", true);
            out.put("output", result);
            events.ok("已点歌: " + keyword);
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", e.getMessage());
            events.warn("点歌失败: " + e.getMessage());
        }
        return out;
    }

    // ---------------------------------------------------------------- 图片

    @PostMapping("/upload")
    public Map<String, Object> upload(@RequestParam("file") MultipartFile file) {
        Map<String, Object> out = new LinkedHashMap<>();
        if (file == null || file.isEmpty()) {
            out.put("ok", false);
            out.put("error", "没有收到文件");
            return out;
        }
        String name = file.getOriginalFilename() == null ? "image" : file.getOriginalFilename();
        try {
            byte[] jpeg = normalizeToJpeg(file.getBytes(), name);
            String reply = deviceService.uploadJpeg(jpeg);
            out.put("ok", true);
            out.put("reply", reply);
            out.put("bytes", jpeg.length);
            events.ok("上传图片 " + name + "（" + jpeg.length + " B）→ 已写入板子 NVS，重启不丢");
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", e.getMessage());
            events.warn("上传失败: " + e.getMessage());
        }
        return out;
    }

    /**
     * 播放器页唱片中心的头像。
     * 和壁纸同一条链路，只是目标尺寸 52×52、板子端走 /avatar 存到独立 NVS。
     * 板子收到后直接 52×52 抖动成 1bpp，不做缩放，画什么就清晰什么。
     */
    @PostMapping("/upload-avatar")
    public Map<String, Object> uploadAvatar(@RequestParam(value = "file", required = false) MultipartFile file) {
        Map<String, Object> out = new LinkedHashMap<>();
        if (file == null || file.isEmpty()) {
            out.put("ok", false);
            out.put("error", "没有收到文件");
            return out;
        }
        String name = file.getOriginalFilename() == null ? "avatar" : file.getOriginalFilename();
        try {
            byte[] jpeg = normalizeToJpeg(file.getBytes(), name, AVATAR_W, AVATAR_H, true);
            String reply = deviceService.uploadAvatar(jpeg);
            out.put("ok", true);
            out.put("reply", reply);
            out.put("bytes", jpeg.length);
            events.ok("上传头像 " + name + "（" + jpeg.length + " B）→ 唱片中心已更新");
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", e.getMessage());
            events.warn("头像上传失败: " + e.getMessage());
        }
        return out;
    }

    // ---------------------------------------------------------------- 网易云扫码登录

    /** 申请登录二维码 */
    @GetMapping("/netease/qr/new")
    public Map<String, Object> neteaseQrNew() {
        return neteaseService.qrNew();
    }

    /** 轮询扫码状态；成功时后端自动保存 cookie 并立即生效 */
    @GetMapping("/netease/qr/check")
    public Map<String, Object> neteaseQrCheck(@RequestParam("key") String key) {
        return neteaseService.qrCheck(key);
    }

    /** 当前是否已登录（有 cookie） */
    @GetMapping("/netease/status")
    public Map<String, Object> neteaseStatus() {
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("ok", true);
        out.put("loggedIn", neteaseService.hasCookie());
        return out;
    }

    @PostMapping("/reset")
    public Map<String, Object> reset() {
        Map<String, Object> out = new LinkedHashMap<>();
        try {
            String reply = deviceService.resetPhoto();
            out.put("ok", true);
            out.put("reply", reply);
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", e.getMessage());
            events.warn("恢复失败: " + e.getMessage());
        }
        return out;
    }

    // ---------------------------------------------------------------- 网易云

    @GetMapping("/music/search")
    public Map<String, Object> musicSearch(@RequestParam("kw") String keyword,
                                           @RequestParam(value = "limit", defaultValue = "12") int limit) {
        Map<String, Object> out = new LinkedHashMap<>();
        try {
            List<Map<String, Object>> songs = neteaseService.search(keyword, Math.min(Math.max(limit, 1), 30));
            out.put("ok", true);
            out.put("songs", songs);
        } catch (Exception e) {
            out.put("ok", false);
            out.put("error", e.getMessage());
        }
        return out;
    }

    @GetMapping("/music/play")
    public Map<String, Object> musicPlay(@RequestParam("kw") String keyword) {
        // 这里是"网页预览"用的，直接返回直链；板子用 /api/play 走自己的链路
        return neteaseService.play(keyword);
    }

    // ---------------------------------------------------------------- 内部

    /** 任意格式图片 → 标准 baseline JPEG（板子 esp_jpeg 解码器只认这种） */
    private byte[] normalizeToJpeg(byte[] raw, String name) throws IOException {
        return normalizeToJpeg(raw, name, PHOTO_W, PHOTO_H);
    }

    /**
     * 同上，但可指定目标尺寸。
     * 播放器头像走 52×52：这个尺寸在板子上是直接 1:1 抖动成 1bpp 的，
     * 传多大就裁多大，不做二次缩放，保证五官清晰。
     */
    private byte[] normalizeToJpeg(byte[] raw, String name, int targetW, int targetH) throws IOException {
        return normalizeToJpeg(raw, name, targetW, targetH, false);
    }

    /**
     * @param cover true = 填满目标框、居中裁掉多余（头像用，不留黑边）
     *              false = 等比缩放、多余留黑边（壁纸用，保留完整构图）
     */
    private byte[] normalizeToJpeg(byte[] raw, String name, int targetW, int targetH, boolean cover) throws IOException {
        java.io.ByteArrayInputStream in = new java.io.ByteArrayInputStream(raw);
        BufferedImage src = ImageIO.read(in);
        if (src == null) {
            throw new IOException("无法解析图片（" + name + "），确认是 JPG/PNG/GIF/BMP/WebP");
        }

        // 统一先转成 8bit RGB：去掉 alpha、CMYK、16bit 等板子解码器不认的格式。
        // 这是"图传上去解不开"最常见的根因——手机截图/HEIC/带透明通道的 PNG 都会踩。
        BufferedImage rgb = new BufferedImage(src.getWidth(), src.getHeight(), BufferedImage.TYPE_INT_RGB);
        Graphics2D g0 = rgb.createGraphics();
        try {
            g0.setBackground(java.awt.Color.WHITE);
            g0.clearRect(0, 0, rgb.getWidth(), rgb.getHeight());
            g0.drawImage(src, 0, 0, null);
        } finally {
            g0.dispose();
        }

        BufferedImage dst = new BufferedImage(targetW, targetH, BufferedImage.TYPE_INT_RGB);
        Graphics2D g = dst.createGraphics();
        try {
            g.setBackground(java.awt.Color.BLACK);
            g.clearRect(0, 0, targetW, targetH);
            // 等比缩放；cover 时取较大比例（铺满后裁边），否则取较小比例（留黑边）
            int sw = rgb.getWidth();
            int sh = rgb.getHeight();
            float scale = cover
                    ? Math.max((float) targetW / sw, (float) targetH / sh)
                    : Math.min((float) targetW / sw, (float) targetH / sh);
            int dw = Math.max(1, Math.round(sw * scale));
            int dh = Math.max(1, Math.round(sh * scale));
            int dx = (targetW - dw) / 2;
            int dy = (targetH - dh) / 2;
            // 双三次插值，缩放质量更好、边缘更干净
            g.setRenderingHint(java.awt.RenderingHints.KEY_INTERPOLATION,
                               java.awt.RenderingHints.VALUE_INTERPOLATION_BICUBIC);
            g.drawImage(rgb, dx, dy, dw, dh, null);
        } finally {
            g.dispose();
        }

        // 显式用 JPEG ImageWriter 输出：质量 0.85，强制基线（baseline）编码。
        // Java 默认 JPEG 编码器有时会输出 4:4:4 采样或渐进式，esp_jpeg 不认 → "解不开"。
        java.util.Iterator<javax.imageio.ImageWriter> writers =
                javax.imageio.ImageIO.getImageWritersByFormatName("jpg");
        if (!writers.hasNext()) {
            throw new IOException("本机缺少 JPEG 编码器");
        }
        javax.imageio.ImageWriter writer = writers.next();
        try (ByteArrayOutputStream out = new ByteArrayOutputStream(32 * 1024);
             javax.imageio.stream.ImageOutputStream ios =
                     javax.imageio.ImageIO.createImageOutputStream(out)) {
            writer.setOutput(ios);
            javax.imageio.ImageWriteParam param = writer.getDefaultWriteParam();
            if (param.canWriteCompressed()) {
                param.setCompressionMode(javax.imageio.ImageWriteParam.MODE_EXPLICIT);
                param.setCompressionQuality(0.85f);
            }
            // 用空元数据，去掉 ICC/EXIF 等可能干扰解码的头
            javax.imageio.IIOImage iio = new javax.imageio.IIOImage(dst, null, null);
            writer.write(null, iio, param);
            ios.flush();
            return out.toByteArray();
        } finally {
            writer.dispose();
        }
    }

    private static String firstLine(String text) {
        if (text == null) {
            return "";
        }
        int n = text.indexOf('\n');
        return n < 0 ? text : text.substring(0, n);
    }
}
