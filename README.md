# 小智 RLCD 桌面摆件

基于 ESP32-S3 的 4.2 英寸黑白反射屏 AI 桌面助手。开机是桌面摆件，说话时是语音助手，放歌时是歌词机。

无需背光也能清晰阅读，放在桌上一直亮着。

---

## 功能

### 语音

- 唤醒词「你好小智」，离线识别，不联网也能唤醒
- 播放音乐时同样可以唤醒
- 说话可随时打断回复
- 唤醒不灵时，按键可以直接进入对话

### 音乐

- 搜索并播放网易云音乐，登录后可听 VIP 曲目
- 自动优先选择原唱版本
- 歌词三行滚动，当前句反白突出
- 唱片中心可换成自己的头像

### 桌面

三个页面，随场景自动切换：

| 页面 | 内容 |
|---|---|
| 天气站 | 自定义壁纸、时间日期、室内温湿度、备忘录 |
| 备忘录 | 分页浏览、定时提醒 |
| 播放器 | 唱片与头像、曲名歌手、歌词、进度条、控制键 |

- 壁纸和头像都能在网页上传，重启后依然保留
- 备忘录到点会切回主页并响铃提醒

---

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-S3，16 MB Flash / 8 MB PSRAM |
| 屏幕 | 4.2 英寸 RLCD，400×300 单色 |
| 音频 | ES8311 编解码器，双麦克风，扬声器 |
| 存储 | 可选 microSD 卡 |
| 供电 | USB-C |

对应开发板型号：`waveshare-s3-rlcd-4.2`

---

## 快速开始

### 烧录固件

需要 ESP-IDF v5.5 开发环境。

```bash
idf.py set-target esp32s3
idf.py build
```

Windows 下也可以直接双击仓库根目录的脚本：

```
flash.bat
```

### 配网

首次开机会进入配网模式。手机连接设备发出的热点，填写 WiFi 名称和密码即可。

### 启动控制台

控制台用来登录网易云、上传壁纸头像、点歌，建议配合使用。

```
tools\xiaozhi-console\start.bat
```

启动后浏览器会自动打开 `http://localhost:8800`。

运行需要 Java 17 或更高版本。脚本会自动查找本机可用的 Java，不需要手动配置环境变量。

板子和电脑连在同一个 WiFi 下即可，控制台会自动发现设备。

---

## 控制台

### 登录网易云

两种方式，任选其一。

**扫码登录**

在控制台首页找到「网易云登录」，点击生成二维码，用手机网易云 App 扫码并在手机上确认。

**手动填写**

1. 浏览器登录 music.163.com
2. 按 F12 打开开发者工具，进入 Application 页面
3. 在 Cookies 里找到 `MUSIC_U`，复制它的值
4. 打开 `tools/xiaozhi-console/cookie.txt`，粘贴成 `MUSIC_U=你的值`

保存后重启控制台即可生效。

不登录也能正常使用，只是部分歌曲搜不到原版，播放也会受限。

### 上传壁纸和头像

两种入口都可以：控制台首页，或直接用手机访问板子的 IP 地址。

选好图片点上传就行，程序会自动裁剪尺寸。壁纸会铺满主页左侧，头像会显示在播放器的唱片中心。

### 点歌

控制台可以直接搜索并播放。搜索时带上歌手名，更容易选到想听的原唱。

---

## 目录结构

```
main/
├── application.cc                    主流程与播放控制
├── audio/                            音频编解码与语音处理
├── display/                          显示抽象层
└── boards/waveshare-s3-rlcd-4.2/
    ├── custom_lcd_display.cc         板级显示实现
    ├── weather_ui.cc                 天气站页
    ├── music_ui.cc                   播放器页
    ├── memo_ui.cc                    备忘录页
    ├── data_update_task.cc           后台数据刷新
    └── managers/
        ├── photo_loader.cc           壁纸处理
        ├── avatar_loader.cc          头像处理
        ├── photo_web_server.cc       板子网页服务
        └── reminder_manager.cc       备忘录与提醒

tools/xiaozhi-console/                Java 控制台
├── service/NeteaseService.java       网易云接口
├── service/DeviceService.java        与板子通信
└── web/                              接口与页面

docs/                                 开发文档
```

---

## 常见问题

**开机后屏幕不亮**

检查屏幕排线是否插紧，以及烧录的固件是否为 `waveshare-s3-rlcd-4.2` 板型。

**播放音乐时唤不醒**

请更新到最新固件。如果仍然出现，查看串口日志里的可用内存，正常应该在 25 KB 以上。

**控制台打不开，提示 Java 版本错误**

需要 Java 17 或更高版本。启动脚本会自动寻找本机可用的 Java，如果只装了 Java 8，会给出下载地址。

**搜不到原唱**

搜索时把歌手名一起说，例如「放宇多田光的 One Last Kiss」，比只搜歌名准确得多。

**上传图片提示解不开**

换一张图片试试。网页上传时会自动转换格式，大部分情况是图片文件本身有问题。

**忘记 Wi-Fi 密码或换了路由器**

长按按键重新进入配网模式。

---

## 致谢

本项目基于以下开源工作：

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — 语音助手框架
- [ESP-SR](https://github.com/espressif/esp-sr) — 离线唤醒与语音处理
- [LVGL](https://github.com/lvgl/lvgl) — 图形界面库
- [ZhouhaoJiang/xiaozhi-esp32](https://github.com/ZhouhaoJiang/xiaozhi-esp32) — RLCD 屏幕适配参考

## 许可证

MIT
