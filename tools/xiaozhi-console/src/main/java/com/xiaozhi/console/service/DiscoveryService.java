package com.xiaozhi.console.service;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Service;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 双向自动发现。
 *
 * <p>电脑每隔几秒广播"我的中转在哪"，板子收到后记进 NVS，同时回一个包说"我在这，我的 IP 是 x.x.x.x"。
 * 于是两边都不需要人念 IP：
 * <ul>
 *   <li>板子→中转：音乐能放；</li>
 *   <li>电脑→板子：网页上传、点歌、切页都能直接下发。</li>
 * </ul>
 *
 * <p>包格式与固件 {@code managers/music_relay_discovery.cc} 严格对应，改一处必须改另一处：
 * <pre>
 * 电脑 → 板子 (UDP 8801): {"svc":"xiaozhi-music-relay","ip":"10.76.94.160","port":8800,"ack":8802}
 * 板子 → 电脑 (UDP 8802): {"svc":"xiaozhi-device","ip":"10.76.94.120","web":80}
 * </pre>
 */
@Service
public class DiscoveryService {

    private static final Logger log = LoggerFactory.getLogger(DiscoveryService.class);
    private static final String SERVICE_TAG = "xiaozhi-music-relay";
    private static final String DEVICE_TAG = "xiaozhi-device";

    private final ObjectMapper mapper = new ObjectMapper();
    private final DeviceService deviceService;
    private final EventBus events;

    @Value("${server.port:8800}")
    private int appPort;

    @Value("${xiaozhi.broadcast-port:8801}")
    private int broadcastPort;

    @Value("${xiaozhi.ack-port:8802}")
    private int ackPort;

    private final List<String> localIps = new ArrayList<>();
    private volatile boolean running = true;
    private Thread listener;
    private DatagramSocket ackSocket;

    public DiscoveryService(DeviceService deviceService, EventBus events) {
        this.deviceService = deviceService;
        this.events = events;
    }

    @PostConstruct
    void start() {
        localIps.addAll(scanLocalIps());
        log.info("本机局域网地址: {}", localIps);
        events.info("控制台已启动，地址 http://localhost:" + appPort
                + "    正在广播中转地址（本机 " + String.join(", ", localIps) + "）");

        listener = new Thread(this::listenAck, "device-ack-listener");
        listener.setDaemon(true);
        listener.start();
    }

    @PreDestroy
    void stop() {
        running = false;
        if (ackSocket != null) {
            ackSocket.close();
        }
        if (listener != null) {
            listener.interrupt();
        }
    }

    // ---------------------------------------------------------------- 广播

    @Scheduled(fixedDelayString = "${xiaozhi.broadcast-interval-ms:3000}", initialDelay = 1000)
    public void broadcast() {
        if (localIps.isEmpty()) {
            localIps.clear();
            localIps.addAll(scanLocalIps());
        }
        if (localIps.isEmpty()) {
            return;
        }

        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setBroadcast(true);
            for (String ip : localIps) {
                Map<String, Object> payload = new LinkedHashMap<>();
                payload.put("svc", SERVICE_TAG);
                payload.put("ip", ip);
                payload.put("port", appPort);
                payload.put("ack", ackPort);
                byte[] data = mapper.writeValueAsBytes(payload);

                // 全球广播 + 网段定向广播各来一发：
                // 有些路由器会拦 255.255.255.255，定向广播通常能过。
                for (String target : broadcastTargets(ip)) {
                    try {
                        socket.send(new DatagramPacket(data, data.length,
                                InetAddress.getByName(target), broadcastPort));
                    } catch (Exception ignored) {
                        // 某块网卡（虚拟网卡/断开的网卡）发不出去，跳过
                    }
                }
            }
        } catch (Exception e) {
            log.debug("广播失败: {}", e.getMessage());
        }
    }

    private List<String> broadcastTargets(String localIp) {
        List<String> targets = new ArrayList<>();
        targets.add("255.255.255.255");
        int dot = localIp.lastIndexOf('.');
        if (dot > 0) {
            targets.add(localIp.substring(0, dot) + ".255");
        }
        return targets;
    }

    // ---------------------------------------------------------------- 回包

    private void listenAck() {
        try {
            ackSocket = new DatagramSocket(null);
            ackSocket.setReuseAddress(true);
            ackSocket.bind(new java.net.InetSocketAddress(ackPort));
            ackSocket.setSoTimeout(2000);
            log.info("已监听 UDP {}，等板子回包上报自己的 IP", ackPort);

            byte[] buffer = new byte[512];
            while (running) {
                DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
                try {
                    ackSocket.receive(packet);
                } catch (java.net.SocketTimeoutException e) {
                    continue;
                }
                String text = new String(packet.getData(), 0, packet.getLength(), StandardCharsets.UTF_8);
                try {
                    JsonNode node = mapper.readTree(text);
                    if (!DEVICE_TAG.equals(node.path("svc").asText())) {
                        continue;
                    }
                    String ip = node.path("ip").asText("");
                    if (ip.isEmpty()) {
                        // 老固件可能没带 ip 字段，就用包来源地址
                        ip = packet.getAddress().getHostAddress();
                    }
                    deviceService.updateDeviceIp(ip);
                } catch (Exception e) {
                    log.debug("收到无法解析的包: {}", text);
                }
            }
        } catch (Exception e) {
            if (running) {
                log.warn("监听 UDP {} 失败（端口被占用？）: {}", ackPort, e.getMessage());
                events.warn("监听 UDP " + ackPort + " 失败：" + e.getMessage() + "（可能是旧 relay.py 还开着）");
            }
        }
    }

    // ---------------------------------------------------------------- 本机地址

    public List<String> getLocalIps() {
        return Collections.unmodifiableList(localIps);
    }

    /** 枚举本机所有可用的 IPv4（跳过回环、169.254、虚拟机常用的 192.168.56.x 不跳过） */
    private static List<String> scanLocalIps() {
        List<String> result = new ArrayList<>();
        try {
            for (NetworkInterface nic : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nic.isUp() || nic.isLoopback() || nic.isVirtual() && nic.getDisplayName() != null
                        && nic.getDisplayName().toLowerCase().contains("vmware")) {
                    continue;
                }
                for (InterfaceAddress address : nic.getInterfaceAddresses()) {
                    if (address.getAddress() instanceof Inet4Address inet4 && !inet4.isLoopbackAddress()
                            && !inet4.isLinkLocalAddress()) {
                        result.add(inet4.getHostAddress());
                    }
                }
            }
        } catch (Exception e) {
            log.debug("枚举网卡失败: {}", e.getMessage());
        }
        return result;
    }
}
