# 音乐页「歌词不显示 / 无封面」诊断结论与改造方案

> 结论日期：2026-11-17
> 适用：微雪 ESP32-S3-RLCD-4.2 + xiaozhi-esp32（板型 `waveshare-s3-rlcd-4.2`）
> 状态：**只做诊断与方案，未修改任何源码**

---

## 1. 结论速览

| 现象 | 是不是 bug | 根因 | 修在哪 |
|---|---|---|---|
| 不显示歌词 | **不是固件 bug** | 调用 `self.music.play_url` 的一端没传 `lyric` / `lyric_url` | 服务端 Agent 提示词 / 歌词 MCP，**零固件改动** |
| 没有封面 | **是功能缺失** | 上下游都从未实现 `cover_url` | 固件（5 处改动），或改用文字方块替代 |

---

## 2. 歌词：链路完整，只是没被喂数据

`main/mcp_server.cc:66` 的工具签名只有 5 个参数：

```cpp
AddTool("self.music.play_url",
    "Play music directly from a URL on device side. Supports MP3 stream playback. "
    "Pass lyric_url so the device can fetch lyrics automatically; fallback to lyric text if provided.",
    PropertyList({
        Property("url", ...), Property("title", ...), Property("artist", ...),
        Property("lyric",     kPropertyTypeString, std::string("")),
        Property("lyric_url", kPropertyTypeString, std::string(""))
    }),
```

固件侧的完整流程（都在 `main/application.cc`）：

| 阶段 | 位置 | 行为 |
|---|---|---|
| 参数分流 | 1188 行 | `lyric` 非空 → 直接显示；为空才把 `lyric_url` 交给播放任务 |
| 拉取 LRC | 1227~1302 行 | 独立 HTTP（`connect_id=4`），逐行解析 `[mm:ss.xx]`，多时间戳展开、按时间排序 |
| 时间轴推进 | 1531~1544 行 | 按播放毫秒数前进，拼成「上一句\n当前句\n下一句」 |
| 三行渲染 | `custom_lcd_display.cc:398` | 按 `\n` 拆三行，分别写 prev / current / next 三个 label |

**显示层是对的**，歌词不出来只有一个原因：`lyric` 和 `lyric_url` 都是空串。

### 一分钟确诊

播一首歌，看串口日志：

```
I (xxxx) Application: 正在拉取歌词: http...            <- 有这行 = 参数到位了
I (xxxx) Application: LRC 歌词解析完成，共 N 行有效歌词  <- N=0 说明 LRC 没有时间戳
```

**连第一行都没有** → 调用端没传参，问题不在设备。

### 修法（按优先级）

1. **改服务端智能体提示词**：明确要求"调用 `self.music.play_url` 时必须同时提供 `lyric_url`"。
   注意：大模型不会凭空知道歌词地址，所以这条通常需要配合第 2 条。
2. **给 Agent 接一个能返回 LRC 的歌词 MCP**，把返回的地址填进 `lyric_url`（最稳，零固件改动）。
3. 设备端自行搜索歌词：需要第三方歌词 API + 处理可用性/版权，**不推荐**。

---

## 3. 封面的四条硬约束（先看清楚再决定做不做）

| # | 约束 | 依据 |
|---|---|---|
| 1 | 屏幕是 **300×400、1bpp 纯黑白**，没有灰阶也没有彩色 | `main/boards/waveshare-s3-rlcd-4.2/rlcd_driver.h:9`「1-bit 单色屏，只有黑白两色」 |
| 2 | 尺寸只能做**抖动（dither）后的黑白缩略图**，观感接近传真件 | 1bpp 的物理上限 |
| 3 | 全帧刷新 = 300×400/8 = **15,000 字节**，SPI 全刷；只在换歌时更新一次可接受 | `rlcd_driver.cc` 的 `DispBuffer` + `RLCD_Sendbuffera` |
| 4 | 上游也没有封面字段，是**新增功能**而非修配置 | `self.music.play_url` 仅 5 参数；上游提交仅有「增加歌曲页面以及自动跳转歌词显示」 |

现状：`music_ui.cc:129~181` 的 `vinyl_card / vinyl_disc / vinyl_center / vinyl_hole` 是 **LVGL 画出来的矢量占位符**，不是待贴图的容器。

---

## 4. 已经具备的零件（省一半工作量）

| 零件 | 位置 | 说明 |
|---|---|---|
| JPEG 解码库 | `managed_components/espressif__esp_new_jpeg` | **已在 main 的依赖链里**（`jpeg_to_image.c` 已在用），不用联网新增依赖 |
| JPEG → RGB565 现成函数 | `main/display/lvgl_display/jpg/jpeg_to_image.c` | `jpeg_to_image(src, len, &out, &out_len, &w, &h, &stride)`，输出 RGB565 |
| **解码期降采样** | `esp_jpeg_dec.h:30,53` | 支持 `config.scale`，**倍率上限 1/8，宽高必须是 8 的倍数** → 500×500 可直解 128×128 = **32 KB** |
| RGB565 → 1bit 转换 | `rlcd_driver.cc` 的 `PixelIndexLUT` / `PixelBitLUT` | 驱动自带，**只要喂纯黑白 RGB565 就会自动变 1bpp** |

⚠️ 坑：LVGL 的 JPEG 解码器 `CONFIG_LV_USE_TJPGD` 是**关闭**的（`sdkconfig:3147`），所以不能用 `lv_image` 直接吃 JPEG，必须走 esp_jpeg 自己解。PNG 反而是开的（`CONFIG_LV_USE_LODEPNG=y`）。

---

## 5. 改动清单（5 处，全部在 `main/`，不碰 `components/`）

| # | 文件 | 改什么 |
|---|---|---|
| 1 | `main/mcp_server.cc:66` | `play_url` 增加 `Property("cover_url", kPropertyTypeString, std::string(""))` |
| 2 | `main/application.h:116` + 任务参数结构 | `PlayMusicFromUrl()` 增加 `cover_url` 形参；`MusicPlaybackTaskArgs` 增加字段 |
| 3 | `main/application.cc:1227` 之后 | **在歌词之后、打开音乐流之前**，串行复用 `connect_id=4` 下载封面（`limit ≤ 256 KB`），写入 PSRAM |
| 4 | 新增 ~50 行 | 用 `esp_new_jpeg` 解码并指定 `config.scale`（取 ≤160 且为 8 的倍数的目标尺寸）→ Bayer 8×8 有序抖动 → 输出纯黑白 RGB565 |
| 5 | `music_ui.cc:129` + `custom_lcd_display.cc` | `vinyl_disc` 一组改成 `lv_image`；新增 `SetMusicCover(const uint16_t* bmp, int w, int h)`，无图时保留原唱片样式 |

关键实现要点：

- 解码输出缓冲用 `heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，**别用默认堆**（一张 128×128 是 32 KB，内部堆禁不起反复分配）
- 抖动只需 1 字节/像素的阈值表（Bayer 8×8 共 64 项），无需额外大缓冲
- 目标尺寸记得向下取整到 **8 的倍数**，否则 `config.scale` 会被解码器拒绝

---

## 6. 内存与并发预算

| 项目 | 大小 | 放哪 |
|---|---|---|
| 封面 JPEG 原始数据 | ≤ 256 KB（上限保护） | PSRAM |
| 解码输出 RGB565（128×128） | 32 KB | PSRAM |
| LVGL `lv_image` 数据 | 32 KB（可与上一项复用） | PSRAM |
| LVGL 图像缓存 | 0（`CONFIG_LV_CACHE_DEF_SIZE=0`） | — |

⚠️ **必须遵守**：**不要并发开第三条 TLS**。每次 HTTPS 握手要 ~30–45 KB **内部** RAM，而这板子内部堆只有 306 KiB（2026-09 那次 OOM 最低只剩 232 字节）。
所以封面必须与歌词**串行复用 `connect_id=4`**，绝不能另起任务并行抓。代价：播放启动慢约 1 秒（两次顺序 HTTP）。

---

## 7. 验收方法

```
I (xxxx) Application: 开始播放音乐: 晴天 - 周杰伦
I (xxxx) Application: 正在拉取歌词: http...
I (xxxx) Application: LRC 歌词解析完成，共 NN 行有效歌词
I (xxxx) Application: 正在拉取封面: http...          <- 新增
I (xxxx) Application: 封面解码完成 128x128            <- 新增
SystemInfo: free sram: ... minimal sram: ...          <- 不得低于 8000
```

判定：歌词逐句滚动 + 左侧出现抖动封面；`minimal sram` 不掉到危险区（< 5000 就该回退）。

---

## 8. 未做 / 未验证

- 本方案**未编译验证**，也**未上机验证**；所有尺寸/内存数字是按 API 定义推算的。
- 灰度：ST7305 数据手册里若有 2bpp 灰度模式，封面观感会好很多，但当前驱动是按 1bpp 写的，需要改驱动的打包位序——属于另一件事。
- 若服务端始终不提供 `cover_url`，封面链路做了也不会生效（和歌词同一个前提）。
