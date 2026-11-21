# 小智 RLCD 桌面摆件

> 基于 ESP32-S3 的 4.2 英寸黑白反射屏 AI 桌面助手：语音对话、网易云音乐、歌词滚动、备忘录提醒、自定义壁纸与头像。

一块 400×300 的单色 RLCD 屏，不需要背光也能看清。开机是桌面摆件，说话时是语音助手，放歌时是歌词机。

---

## 功能

### 语音助手

- 离线唤醒词 **「你好小智」**（ESP-SR WakeNet9，不联网也能唤醒）
- 播放音乐时**同样可唤醒**（麦克风常开，不与音频输出抢通道）
- 流式 ASR → LLM → TTS，支持打断
- 按键强制对话（唤醒词失效时的兜底）

### 音乐播放

- 网易云搜索与播放，配置 cookie 后可播 VIP 曲目
- **原唱优先**：先按歌手硬过滤，再按歌名/时长/翻唱标记打分
- LRC 歌词三行滚动，当前句黑底白字反白
- 唱片中心头像支持自定义（1-bit 点阵，网页上传）
- 静态唱片（不做旋转动画，把 CPU 让给音频解码）

### 桌面

| 页面 | 内容 |
|---|---|
| 天气站 | 自定义壁纸、时间日期、室内温湿度、备忘录列表 |
| 备忘录 | 分页浏览、定时提醒 |
| 播放器 | 唱片 + 头像、曲名歌手、歌词、进度条、控制键 |

- 壁纸：网页上传，210×248 抖动成 1-bit，存 NVS，重启不丢
- 头像：网页上传，52×52 抖动成 1-bit，存独立 NVS
- 提醒：到点自动切回主页并**连响 3 次**

---

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-S3（16 MB Flash / 8 MB PSRAM） |
| 屏幕 | 4.2 英寸 RLCD，400×300，单色（1-bit） |
| 音频 | ES8311 编解码器 + 双麦克风 + 扬声器 |
| 存储 | 可选 microSD（壁纸目录 `/sdcard/photos`） |
| 供电 | USB-C |

开发板型号：`waveshare-s3-rlcd-4.2`

---

## 快速开始

### 1. 烧录固件

需要 [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/) 环境。

```bash
idf.py set-target esp32s3
idf.py build
```

仓库根目录提供了一键烧录脚本（Windows）：

```bat
flash.bat
```

### 2. 配网

首次开机自动进入配网模式（`78/xiaozhi-esp32` 配网协议），用手机连接设备热点后填写 WiFi 信息。

### 3. 启动控制台（可选，但强烈推荐）

控制台提供网易云登录、壁纸/头像上传、点歌等能力。

```bat
tools\xiaozhi-console\start.bat
```

- 浏览器自动打开 `http://localhost:8800`
- 需要 **Java 17 或更高**（脚本会自动扫描全盘寻找，不依赖 `JAVA_HOME`）
- 板子与电脑在同一局域网即可，控制台通过 UDP 广播自动发现

---

## 控制台

### 网易云登录

三种方式，任选其一：

1. **扫码登录**：控制台首页「网易云登录」→ 生成二维码 → 手机网易云 App 扫码确认
2. **手动贴 cookie**：浏览器登录 [music.163.com](https://music.163.com)，F12 → Application → Cookies → 复制 `MUSIC_U` 的值，写入 `tools/xiaozhi-console/cookie.txt`
3. **整行 Cookie**：F12 → Network → 任意请求 → Request Headers → 复制整行 `Cookie`

> `MUSIC_U` 是 **HttpOnly** cookie，`document.cookie` 读不到，必须从 Application 面板或请求头获取。

**没有 cookie 也能用**，只是搜索结果的正式版会被隐藏，以翻唱为主。

### 壁纸与头像

访问 `http://<板子IP>/`（板子自带网页），或控制台首页：

- **壁纸**：裁成 210×248，浏览器端转标准 JPEG 后上传
- **头像**：裁成 52×52，同样在浏览器端预处理

两者都在上传时完成裁剪与格式转换，板子只负责解码 + 抖动，因此手机相册里任意格式都能传。

---

## 目录结构

```
├── main/
│   ├── application.cc              # 主流程：音乐播放、对话状态机
│   ├── audio/                      # 编解码、AFE、音频服务
│   ├── display/                    # LVGL 显示抽象
│   └── boards/waveshare-s3-rlcd-4.2/
│       ├── custom_lcd_display.*    # 板级显示类
│       ├── weather_ui.cc           # 主页（壁纸 + 时间 + 提醒）
│       ├── music_ui.cc             # 播放器页
│       ├── memo_ui.cc              # 备忘录页
│       ├── data_update_task.cc     # 后台刷新（时间/电量/传感器）
│       └── managers/
│           ├── photo_loader.*      # 壁纸解码 + 抖动 + NVS
│           ├── avatar_loader.*     # 头像解码 + 抖动 + NVS
│           ├── photo_web_server.*  # 板子网页（上传/删除）
│           └── reminder_manager.*  # 备忘录与定时提醒
├── tools/xiaozhi-console/          # Java 控制台（Spring Boot）
│   └── src/main/java/com/xiaozhi/console/
│       ├── service/NeteaseService  # 网易云搜索/播放/扫码登录
│       ├── service/DeviceService   # 板子通信（HTTP + UDP 发现）
│       └── web/                    # REST 接口与前端页面
└── docs/                           # 开发文档、踩坑记录
```

---

## 常见问题

<details>
<summary><b>烧录后黑屏 / 白屏</b></summary>

确认屏幕排线插紧，且固件是 `waveshare-s3-rlcd-4.2` 板型。查看串口日志是否有 `LCD` 初始化失败。
</details>

<details>
<summary><b>播放音乐时喊唤醒词没反应</b></summary>

已在 v2 修复（播放期间保持麦克风开启 + 预留内部 RAM 给 AFE）。若仍失效，检查串口日志的 `free sram` 是否低于 20 KB。
</details>

<details>
<summary><b>控制台报 <code>UnsupportedClassVersionError</code></b></summary>

用了低于 Java 17 的运行时。`start.bat` 会自动扫描并选择最高版本的 Java 17+，如果机器上只有 Java 8 会提示下载地址。
</details>

<details>
<summary><b>搜出来的不是原唱</b></summary>

搜索时**带上歌手名**命中率最高（例：`放宇多田光的 One Last Kiss`）。系统会先用歌手名做硬过滤，再在结果里选正式版。
</details>

<details>
<summary><b>上传图片提示"解不开"</b></summary>

网页上传会自动转成标准 baseline JPEG。若仍失败，多半是图片本身损坏，换一张试试。
</details>

---

## 致谢

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) —— 语音助手框架
- [ESP-SR](https://github.com/espressif/esp-sr) —— 离线唤醒与语音前端
- [LVGL](https://github.com/lvgl/lvgl) —— 图形库
- [ZhouhaoJiang/xiaozhi-esp32](https://github.com/ZhouhaoJiang/xiaozhi-esp32) —— RLCD 板级适配参考

## 许可证

MIT
