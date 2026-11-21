# 小智桌面控制台（xiaozhi-console）

一个进程同时干三件事，浏览器打开 `http://localhost:8800` 就是完整可视化面板：

1. **网易云中转** —— 板子通过 `/play?kw=` 拿到可播放直链 + 歌词（接口与旧 relay.py 完全兼容，固件一行不用改）
2. **设备控制** —— 板子 IP 靠 UDP 广播自动发现，之后用 HTTP 下发命令（切页 / 点歌 / 传图 / 重启）
3. **壁纸上传（常驻）** —— 传图后自动缩到 420×496，写入板子 NVS，**关电源再开机仍是这张图**；「恢复内置壁纸」会同时清掉 NVS 缓存

## 技术栈

- 后端：Spring Boot 3.2 + Java 17（内嵌 Tomcat，无需外置容器）
- 前端：Vue 3 + Element Plus（CDN 方式，打进 jar，无需 npm 构建）
- 通信：HTTP REST + SSE（实时日志）+ UDP 广播（自动发现）

## 目录结构

```
src/main/java/com/xiaozhi/console/
├── ConsoleApplication.java      # 入口
├── service/
│   ├── NeteaseService.java      # 网易云搜索/打分/取直链/取歌词
│   ├── DeviceService.java       # 板子代理：命令透传 + 图片上传/重置
│   ├── DiscoveryService.java    # UDP 双向自动发现（8801 广播 / 8802 收包）
│   └── EventBus.java            # SSE 事件流（前端实时日志）
└── web/
    ├── RelayController.java     # 给板子用的中转接口（/health /search /play）
    ├── ConsoleController.java   # 给前端面板用的 REST 接口（/api/*）
    └── IndexController.java     # 根路径 → index.html
src/main/resources/
├── application.yml              # 端口、广播、cookie 等配置
└── static/index.html            # Vue 单页前端
```

## 一键启动

```bat
start.bat          # 直接运行（Windows）
```

或手动：

```bash
mvn -DskipTests package
java -jar target/xiaozhi-console.jar
```

## 关键端口

| 端口 | 用途 |
|------|------|
| 8800 | 控制台网页 + 网易云中转（板子要连它） |
| 8801 | 电脑 → 板子广播（UDP） |
| 8802 | 板子 → 电脑回包（UDP） |

## 配置网易云 Cookie

浏览器登录 `music.163.com`，复制整段 Cookie 存到工程根目录的 `cookie.txt`（也可在 `application.yml` 里改 `xiaozhi.cookie-file` 指向别处）。没有 Cookie 只能放免费曲库（多为翻唱），VIP 歌放不了。

## 与固件的对应关系

- `photo_loader.cc` 的 NVS 常驻壁纸：`/api/upload` → 板子 `/upload`（写 NVS），`/api/reset` → 板子 `/reset`（清 NVS）
- `music_relay_discovery.cc` 的 UDP 协议：见 `DiscoveryService.java` 顶部注释，改一处必须改另一处
