package com.xiaozhi.console.service;

import jakarta.annotation.PreDestroy;
import org.springframework.stereotype.Service;
import org.springframework.web.servlet.mvc.method.annotation.SseEmitter;

import java.io.IOException;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.concurrent.ConcurrentLinkedDeque;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/**
 * 前端日志流。浏览器用一条 SSE 长连接收所有事件（发现设备、上传、点歌结果……），
 * 不用前端轮询。手机上看网页时也省流量。
 */
@Service
public class EventBus {

    private static final int MAX_EVENTS = 200;
    private static final DateTimeFormatter TIME = DateTimeFormatter.ofPattern("HH:mm:ss");

    private final Deque<Event> history = new ConcurrentLinkedDeque<>();
    private final List<SseEmitter> emitters = new CopyOnWriteArrayList<>();
    private final ScheduledExecutorService heartbeat = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "sse-heartbeat");
        t.setDaemon(true);
        return t;
    });

    public record Event(String time, String level, String message) {
    }

    public EventBus() {
        // 心跳：防止中间的代理/防火墙把空闲长连接掐掉
        heartbeat.scheduleAtFixedRate(() -> {
            for (SseEmitter emitter : emitters) {
                try {
                    emitter.send(SseEmitter.event().comment("ping"));
                } catch (Exception e) {
                    emitters.remove(emitter);
                }
            }
        }, 20, 20, TimeUnit.SECONDS);
    }

    @PreDestroy
    public void shutdown() {
        heartbeat.shutdownNow();
    }

    public void info(String message) {
        push("info", message);
    }

    public void ok(String message) {
        push("ok", message);
    }

    public void warn(String message) {
        push("warn", message);
    }

    public void error(String message) {
        push("error", message);
    }

    public void push(String level, String message) {
        Event event = new Event(LocalTime.now().format(TIME), level, message);
        history.addLast(event);
        while (history.size() > MAX_EVENTS) {
            history.pollFirst();
        }
        for (SseEmitter emitter : emitters) {
            try {
                emitter.send(SseEmitter.event().name("log").data(event));
            } catch (IOException | IllegalStateException e) {
                emitters.remove(emitter);
            }
        }
    }

    public SseEmitter subscribe() {
        SseEmitter emitter = new SseEmitter(0L);   // 不超时
        emitters.add(emitter);
        emitter.onCompletion(() -> emitters.remove(emitter));
        emitter.onTimeout(() -> emitters.remove(emitter));
        emitter.onError(e -> emitters.remove(emitter));
        try {
            // 新连接先把最近的历史补上，页面一打开就有上下文
            List<Event> snapshot = new ArrayList<>(history);
            for (Event event : snapshot) {
                emitter.send(SseEmitter.event().name("log").data(event));
            }
        } catch (IOException e) {
            emitters.remove(emitter);
        }
        return emitter;
    }
}
