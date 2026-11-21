package com.xiaozhi.console;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.scheduling.annotation.EnableScheduling;

/**
 * 小智桌面控制台。
 *
 * <p>一个进程干三件事：
 * <ol>
 *   <li>网易云中转：板子通过 {@code /play?kw=} 拿到可播放的直链和歌词（和原来的 relay.py 接口兼容）；</li>
 *   <li>设备控制：板子 IP 靠 UDP 广播自动发现，之后用 HTTP 转发命令（切页 / 点歌 / 传图）；</li>
 *   <li>可视化前端：Vue 打包进 static，浏览器打开 {@code http://localhost:8800} 就是完整面板。</li>
 * </ol>
 */
@SpringBootApplication
@EnableScheduling
public class ConsoleApplication {

    public static void main(String[] args) {
        SpringApplication.run(ConsoleApplication.class, args);
    }
}
