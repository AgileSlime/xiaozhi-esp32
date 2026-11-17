# 二创 v2 —— 不依赖 UI 设计的部分（已完成）

> 项目目录：`E:/esp32test/waveshare-rlcd-v2`
> 原始工程（可回退）：`E:/esp32test/waveshare-esp32s3-rlcd-4.2`
> 本文只记录**与主页 UI 版式无关**的改动。主页美图的视觉设计等你的稿子，UI 层暂不动。

---

## 0. 这轮做了什么（一句话版）

把原来"一个 1 秒大循环干所有事"的 `DataUpdateTask` 拆成三条职责单一的任务
（`clock_task` / `reminder_task` / `ui_refresh_task`），并把音乐从"只能放网络 URL"
扩展成"能放 SD 卡本地音乐 + 自动配同名歌词"，同时补齐了语音可控的备忘/闹钟工具链。

---

## 1. FreeRTOS 任务表（改完后的真实分工）

| 任务名 | 优先级 | 栈 | 触发方式 | 只做什么 | 绝对不做什么 |
|---|---|---|---|---|---|
| `clock_task` | 2 | 4 KB | `vTaskDelayUntil` 1 s | NTP 同步 / 退避重试 / 24h 校准 / RTC 跳变恢复 / 派发节拍 | 不碰 LVGL、不写 NVS |
| `reminder_task` | 3 | 5 KB | `ulTaskNotifyTake`（睡到下一个 due，最长 60 s 醒一次） | 备忘到点判断、触发回调、NVS 落盘 | 不碰 LVGL、不直接调 `Alert()` |
| `ui_refresh_task` | 2 | 8 KB | 时钟节拍通知（仅分钟变化）+ 1/5 s 超时 | 把时间/温湿度/天气/电池/WiFi/AI 状态画到屏幕 | 不再做 NTP、不再扫备忘 |
| `pomodoro_task` | 3 | 原有 | 原有 | 番茄钟倒计时 | 不变 |
| `white_noise_task` | — | 原有 | 原有 | SD 卡白噪音循环 | 不变 |
| `music_playback` | 3 | 12 KB | `xTaskCreate`（播放时创建） | MP3 解码 + I2S 输出 | 不变（新增本地文件分支） |

关键点：**只有 `ui_refresh_task` 和 Application 主任务会拿 LVGL 锁**。
另外两条任务通过"回调 + 任务通知"把活交给它们，不再出现"数据任务里同步等 UI 锁"。

### 1.1 原来 7 个冲突点，现在如何处理

| # | 原问题 | 现在 |
|---|---|---|
| 1 | NTP 退避 1→2→4→8→16 s 的 `vTaskDelay` 塞在 1 s 循环里，整条 UI 刷新被卡住 | 退避改成"下次尝试时间戳"（`next_sync_at_ms_`），只跳过同步，节拍照常 1 s |
| 2 | 持锁期间做 cJSON 解析 + NVS 写入 | 备忘录的 JSON/NVS 全部移到 `reminder_task`，且**在锁外**完成 |
| 3 | 数据任务直接 `app.Alert()` 抢 UI 锁 | 提醒任务只回调 `SetAlertCallback`，board 里用 `app.Schedule()` 抛回主任务执行 |
| 4 | MCP 工具回调跑在 Application 主任务 | 未改协议层（MCP 必须同步返回），但工具内部不再做重活：只调 `ReminderManager::Add()`（一次小锁 + 一次 NVS 写） |
| 5 | `DisplayLockGuard` 锁超时 30 秒 | 持有时间缩短：备忘录/时间同步都不再在锁内做 IO |
| 6 | `showing_system_info_` 手工原子标志 | 保留（长按看系统信息时暂停 UI 刷新），语义未变 |
| 7 | 番茄钟任务结束时也调 `app.Alert()` → 第四个执行上下文 | 番茄钟逻辑未改，但它已经不在 `DataUpdateTask` 里，不再和 UI 刷新互相等锁 |

---

## 2. 新增模块

### 2.1 `managers/clock_service.{h,cc}` —— 时钟服务

- 唯一时间源。对外只有四个动作：`Start()` / `RequestSync()` / `Snapshot()` / `AddListener()`。
- `ClockTick` 快照字段：`epoch`、`tm_local`、`synced`、`minute_changed`、`day_changed`。
- 同步策略（沿用原逻辑，但不再阻塞别人）：
  - 网络就绪 + 连续 idle ≥ 5 s 才允许发 NTP；
  - 首同步最多 5 次，失败退避 1/2/4/8/16 s，之后停手（可用按键/工具重新 `RequestSync()`）；
  - 成功后每 24 h 校准一次；
  - 系统时间跳变超过 ±2 h → 从 PCF85063 硬件 RTC 恢复。
- 启动时先 `PublishTick(true)` 发一拍，避免 UI 在 NTP 之前显示 `00:00`。

### 2.2 `managers/reminder_manager.{h,cc}` —— 备忘 / 闹钟

- 兼容旧数据：仍写 NVS `namespace=memo`、`key=items`，每条仍有 `t`（HH:MM）和 `c`（内容）字段，
  所以现有 UI 的 `RefreshMemoDisplayInternal()` **一行都不用改**。
- 新增字段：`id`、`r`（1=每天重复）、`due`（下次触发的 epoch）。
- 能力：
  - `Add(content, "HH:MM", daily, &err)` 新增（最多 10 条）；
  - `RemoveByIndex(1-based)` / `Clear()` / `List()` / `ListText()`；
  - `Snooze(minutes)` 稍后提醒（把刚响过的那条顺延）；
  - 单次提醒触发后自动删除，每日提醒自动滚到明天同一时刻；
  - 设备离线很久后开机：单次提醒会补触发一次（不丢），每日提醒直接滚到下一个周期。
- 时间未同步（年份 < 2024）时不触发，等 NTP 成功后再按 `HH:MM` 重算全部 `due`。

### 2.3 `managers/music_library.{h,cc}` —— SD 卡本地曲库

- 扫描 `/sdcard/music` 下的 `.mp3` / `.wav`，按文件名排序并去重。
- 关键字搜索（大小写不敏感、子串匹配）。
- 目录约定：

```
/sdcard/music/晴天.mp3
/sdcard/music/晴天.lrc      ← 可选，播放时自动加载，不需要联网
```

### 2.4 `application.cc` 的本地播放分支

`Application::PlayMusicFromUrl()` 现在接受两种 URL：

| 传入 | 行为 |
|---|---|
| `https://...mp3` | 原逻辑：HTTP 流 + `esp_audio_simple_dec` 解码 |
| `/sdcard/music/晴天.mp3` 或 `file:///sdcard/...` | 新增：`fopen` + `fread`，同一个解码器、同一套重采样/音量逻辑 |

本地文件还会自动找同名 `.lrc`（把 `.mp3` 换成 `.lrc`），歌词解析/滚动显示完全复用原链路。

---

## 3. MCP 工具清单（语音能调到的）

### 3.1 备忘 / 提醒

| 工具 | 参数 | 说明 |
|---|---|---|
| `self.memo.add` | `content`, `time`(HH:MM), `repeat`(bool) | 新增备忘；有时刻就会响铃，`repeat=true` 每天重复 |
| `self.memo.list` | — | 列出全部备忘（带序号、时间、"每天"标记） |
| `self.memo.done` | `index`(1..10) | 按序号删掉一条 |
| `self.memo.clear` | — | 清空全部 |
| `self.reminder.snooze` | `minutes`(1..120) | 把刚响过的提醒顺延 N 分钟 |

语音典型链路：
「十分钟后提醒我喝水」→ 模型换算成 `HH:MM` → `self.memo.add(content=喝水, time=21:43, repeat=false)`
→ 到点 `reminder_task` 触发 → `app.Alert()` 播提示音 + 屏幕弹提醒 → 自动从列表删除并刷新右下角。

### 3.2 音乐

| 工具 | 参数 | 说明 |
|---|---|---|
| `self.music.search` | `keyword` | 搜 SD 卡 `/sdcard/music`，返回序号 + 歌名 |
| `self.music.play_local` | `keyword` 或 `index` | 播放本地歌曲，自动加载同名 `.lrc` |
| `self.music.play_url` | `url`, `title`, `artist`, `lyric`, `lyric_url` | 原有：播放网络 MP3 流 |
| `self.music.stop` | — | 停止播放 |
| `self.music.status` | — | 当前歌名 / 已播放时间 |

> "搜索"目前是**本地曲库搜索**（离线可用）。网络音乐搜索依赖服务端把 URL 下发给
> `self.music.play_url`，设备端不做 HTTP 搜索——这是上一轮已经定下的取舍，避免设备端
> 再维护一套第三方音乐 API、还要处理版权和反爬。

---

## 4. 落地清单（文件级）

新增：

```
main/boards/waveshare-s3-rlcd-4.2/managers/clock_service.h
main/boards/waveshare-s3-rlcd-4.2/managers/clock_service.cc
main/boards/waveshare-s3-rlcd-4.2/managers/reminder_manager.h
main/boards/waveshare-s3-rlcd-4.2/managers/reminder_manager.cc
main/boards/waveshare-s3-rlcd-4.2/managers/music_library.h
main/boards/waveshare-s3-rlcd-4.2/managers/music_library.cc
```

修改：

```
main/boards/waveshare-s3-rlcd-4.2/data_update_task.cc     ← 去掉 NTP + 备忘，改任务通知唤醒
main/boards/waveshare-s3-rlcd-4.2/custom_lcd_display.h    ← last_valid_epoch_ → last_hour_
main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc ← 启动服务、换掉备忘工具、加音乐工具
main/application.cc                                        ← 本地文件播放分支 + 本地 .lrc
```

未动：`weather_ui.cc` / `music_ui.cc` / `pomodoro_ui.cc` 的所有布局代码、`rlcd_driver.*`、
`sdkconfig`（沿用上一轮的内存修复）。

---

## 5. 上机验证点（烧录后按顺序看日志）

| 日志关键字 | 期望 |
|---|---|
| `ClockService: 时钟任务已启动` | 时钟任务起来了 |
| `ClockService: NTP 同步成功` | 网络通、时间对 |
| `ReminderManager: 提醒任务已启动` / `备忘已加载: N 条` | 备忘从 NVS 恢复 |
| `ReminderManager: 触发提醒` | 到点响铃 |
| `UiRefresh: 时间已更新` | 每分钟一次（不再是每秒刷屏） |
| `MusicLibrary: 本地曲库扫描完成` | SD 卡识别到歌 |
| `Application: 正在拉取歌词` / `LRC 歌词解析完成` | 歌词链路通 |
| `Application: 本地音乐文件` → `音乐播放结束` | 本地播放链路通 |

---

## 6. 还没做（等 UI 设计稿）

1. **主页美图**：图片解码成 400×300 1-bit 的服务层可以先做（`esp_new_jpeg` 已在依赖里），
   但"整屏铺 / 上方卡片留白 / 半透明反白"这些取决于你的排版，属于 UI 层，暂不动。
2. **主页新布局**：日期/时间/温度的位置、字号、是否保留 AI 卡片，同样等稿。
3. **多页导航**：现在仍是 USER 键循环切换天气/音乐/番茄钟，新页面怎么进、要不要触摸，等稿定。

---

## 7. 构建验证结果（本机已跑通）

| 项 | 值 |
|---|---|
| 工具链 | ESP-IDF v5.5.5 / xtensa-esp-elf 14.2.0_20260121 |
| 结果 | `Project build complete`，无 error、无我们文件的 warning |
| `xiaozhi.bin` | 4,706,080 B（原版 4,685,664 B，**+20,416 B**） |
| app 分区 | 0x4F0000，剩余 9% |
| `bootloader.bin` | 16,240 B |
| `generated_assets.bin` | 2,822,210 B（与原版一致，assets 未变） |
| DIRAM 静态占用 | 147,467 / 341,760 B（43.15%，原 147,035 / 43.02%，**仅 +432 B**） |
| IRAM | 16,384 / 16,384（100%，仍是上一轮 icache 减半的指纹，正常） |

运行期新增的两块栈（`clock_task` 4 KB + `reminder_task` 5 KB）从堆上分配，
约 9 KB，对 246 KiB 主堆没有压力。

### 7.1 构建时踩到的一个坑（已修）

`scripts/build_default_assets.py` 在打包完 assets 后会 `shutil.rmtree(build/temp_build)`。
本机的"安全删除"审计会拦截这种批量删除（>50 个文件），**进程被直接终止**，
于是出现"assets.bin 明明生成成功，ninja 却报 FAILED"的怪现象。

处理：把脚本里的删除改成 `_safe_rmtree()`，不再删临时目录（`os.makedirs(..., exist_ok=True)`）。
残留的临时文件不会被打进 assets.bin——打包清单由 `index.json` 决定，不是扫目录。
原版工程里是另一个绕法（python 垫片），v2 里直接用这个更干净的版本。

---

## 8. 主页 UI 改版（第二轮）

### 8.1 版式（400×300，与设计稿一致）

| 区域 | x | y | w | h | 说明 |
|---|---|---|---|---|---|
| 温湿度 | 10 | 8 | ~112 | 20 | 16px 白字，直接贴黑板 |
| 状态栏胶囊 | 260 | 4 | 132 | 26 | 白底，WiFi + 电池图标 + 电量 |
| 图片区 | 10 | 38 | 214 | 252 | 白边框 2px，内部 210×248 抖动图 |
| 时间卡 | 232 | 38 | 158 | 102 | 白底圆角 12，`11:42` 48px + `11月17日 周一` 16px |
| 备忘/语音卡 | 232 | 148 | 158 | 142 | 白底圆角 12，双态复用 |

四周留 10px，右列固定 158px（`11:42` 用 48 号字约占 118px，再窄就溢出）。

### 8.2 右下角双态卡片

| 状态 | 触发条件 | 内容 |
|---|---|---|
| 提醒态 | Idle / Starting / 配网 / 激活 / 出错 | 「提醒」+「共 N 条」+ 3 行 `HH:MM 内容`，最近一条反白 |
| 语音态 | Connecting / Listening / Speaking | 「聆听中」胶囊 + 计时 + 用户识别文字（反白块）+ AI 回复 |
| 提示态 | 任意 system 消息 | 临时占用语音卡，10 秒后自动收回提醒列表 |

切换在 `data_update_task` 的每轮循环里调 `UpdateHomeBottomCard(ds)`，内部用 `last_bottom_mode_` 去重，
模式没变就只更新文字，不重绘整块（1-bit 屏每次 flush 都是整屏 15,000 字节 SPI，能省则省）。

提醒列表的数据源从「NVS 里的 JSON 原文」换成了 `ReminderManager::List()`——原文只有 `t`/`c`，
排不出「哪条最先响」；`ReminderManager` 有 `due` 字段，还带每日重复的下次触发时间。

### 8.3 语音文字是整句出现，不是逐字浮现

`application.cc:570` 处理服务端 `stt` 消息，识别完成后整句下发一次 `SetChatMessage("user", ...)`。
协议层没有 partial 流式字幕，所以「聆听中」阶段只能显示静态胶囊 + 计时，说完才弹出整句文字。
要逐字浮现需要服务端下发 partial，设备端无法自行实现。

### 8.4 照片为什么必须先抖动

flush 回调（`custom_lcd_display.cc:37`）只做一次固定阈值二值化：

```cpp
uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
```

照片的中间调会被这一刀切掉，只剩黑白剪影。所以 `managers/photo_loader.{h,cc}` 在送进 LVGL 之前先做：

```
JPEG 解码(RGB888) → 中心裁剪 cover → 灰度(BT.601)
→ 直方图 1%/99% 自动对比度 → Bayer 8×8 有序抖动 → RGB565 0x0000/0xFFFF
```

几个关键取舍：

- **输出用 RGB565**（210×248×2 = 102 KB，PSRAM）。不用 I1/A8 是因为这两者在 LVGL 9 里
  的调色板行为要额外验证，而 RGB565 只有 0x0000/0xFFFF 两个值，过阈值时 100% 保真。
- **1:1 像素对齐，绝不缩放**。`lv_image` 的宽高必须等于 `lv_image_dsc_t` 的 `header.w/h`，
  一旦缩放，抗锯齿会把抖动颗粒糊成灰块。
- **大图先缩一半再解码**：源图 > 640×640 时用 `jpeg_dec_config_t.scale` 减半，控制峰值内存
  （全尺寸 4000×3000 解码要 36 MB，PSRAM 直接爆）。
- **采样尺寸用输出字节数反推**：`scale` 只保证 8 的倍数，实际输出和请求值可能差几个像素，
  所以用 `out_len/3` + 原图宽高比反推真实宽高，避免采样踩到行尾。

目录约定：

```
/sdcard/photos/任意名字.jpg      （也认 .jpeg）
```

按「今天是一年第几天 % 图片数」选图，同一天重启不换、隔天自动换；单张坏图会顺延试下一张。

### 8.5 天气暂时下线

新主页没有天气的位置。`weather_label_` 已从类里移除，`data_update_task` 里对应的写入也一并删掉，
`WeatherManager` 后台拉取不受影响。以后要显示（比如塞进状态栏或做第三页）再把成员加回来即可。

### 8.6 编译结果

| 项 | 值 |
|---|---|
| 结果 | `Project build complete`，改动文件零 warning |
| `xiaozhi.bin` | 4,776,912 B（上轮 4,706,080，+70,832，主要是 JPEG 解码器） |
| app 分区剩余 | 400,541 B（7.7%） |
| IRAM | 16,384 / 16,384（仍是 icache 减半的指纹） |

> 注意：`main/CMakeLists.txt` 用 `file(GLOB_RECURSE ... managers/*.cc)` 收集源文件，
> 新增的 `photo_loader.cc` 需要跑一次 `idf.py reconfigure` 才会被收录，否则链接期报
> `undefined reference to HomePhoto::GetInstance()`。

### 8.7 上机验证点

| 日志 | 期望 |
|---|---|
| `HomeUI: 主页 UI 创建完成` | 布局起来了 |
| `HomePhoto: 图片目录扫描完成: N 张` | SD 卡 /sdcard/photos 被读到 |
| `HomePhoto: 图片已抖动渲染 ... 输出 210x248` | 解码 + 抖动成功 |
| `CustomDisplay: 提醒列表已刷新，共 N 条` | 备忘卡有数据 |
| 说话时 | 右下角切到「聆听中」，说完弹出整句识别文字 |
| 对话结束 3 秒后 | 自动切回提醒列表 |

## 9. 播放器页 & 备忘录页（第三轮 UI）

### 9.1 统一的三页视觉语言

| 元素 | 规则 |
|---|---|
| 底色 | 主页 / 备忘录 = 黑底白卡；播放器 = 白纸面 |
| 正文 | `font_puhui_16_4`（16px），次级信息 `font_puhui_14_1` |
| 大号数字 | `alibaba_puhui_48`（主页时钟）、`alibaba_puhui_24`（备忘录行内时间、番茄钟） |
| 强调 | 1-bit 没有浅灰和字重，只用「整块反白」表达（主页提醒卡首条、备忘录首条） |
| 状态栏 | 播放器/备忘录共用 66×24 胶囊几何；主页用 132×26 白胶囊装图标 |

### 9.2 播放器页（`music_ui.cc` 整体重写）

| 区域 | 实现 |
|---|---|
| 唱片 | 148px 黑胶 + 9 圈白纹 + 白心标签，外圈 3 个白点 120° 公转（150ms/10°，约 6.7s 一圈），暂停即停 |
| 歌词 | 4 行，当前句 16px + 圆点，上下句 14px，换句时整块从下方滑回（180ms） |
| 控制 | `\|◀ (❚❚) ▶\|` 用 ARGB8888 画布逐像素画，不依赖图标字体；纯状态指示（本机无触摸） |
| 进度 | 1px 轨道 + 4px 已播 + 9px 圆点，左右 `01:24` / `04:32` |
| 时长 | 本地 mp3/wav 新增时长探测（Xing 帧数优先，退化 CBR 按码率），没 `.lrc` 也有总时长 |

语音控制补齐：`PauseMusicPlayback` / `ResumeMusicPlayback` / `SeekMusic(±秒)`（解码丢弃而非 fseek）/ `PlayNextMusic` / `PlayPrevMusic`。
按下说话时音乐从「停止」改为「暂停」——否则快进没有播放位置可续。

### 9.3 备忘录页（新增 `memo_ui.cc`，本质是闹钟目录）

```
┌──────────────────────────────────────────────┐
│ ╭──────────────────────────────────────────╮ │  白色胶囊头：时间 · 标题 · 电量
│ │ 18:07 ▂▃   备忘录           86% ▮▮      │ │
│ ╰──────────────────────────────────────────╯ │
│ ┌──────────────────────────────────────────┐ │
│ │▛▛ 和设计组过一遍 UI 稿        ▛12:07▛   │ │  首条反白 = 下一条要响的
│ │  回复微雪的技术邮件            15:40     │ │  时间是 24px 大号数字
│ │  给 RLCD 板子刷一版固件        18:20     │ │
│ │  每天 整理本周的排期           21:00     │ │
│ └──────────────────────────────────────────┘ │
│ 共 4 项待办                          01│03   │  页脚：统计 + 页码
└──────────────────────────────────────────────┘
```

| 项 | 值 |
|---|---|
| 卡片 | (12,40) 376 × 214，圆角 12；**高度跟着条目数收缩**（`visible*53+8`，空列表用整高当提示面板） |
| 行 | 53px，内缩 4px 的圆角条（半径 8），首行刷黑反白 |
| 正文 | 16px，`LV_LABEL_LONG_DOT` 截断，宽 272 |
| 时间 | 24px `alibaba_puhui_24`，右对齐，宽 72 |
| 分隔线 | 1px，左右内缩 16px；**只在"下面还有一条"时显示**（否则空行被线切碎） |
| 页脚 | 左「共 N 项待办」14px；右侧页码框 68×22，当前页反白；只有 1 页时整块隐藏 |
| 分页 | 4 条/页，`ReminderManager` 上限 10 条 → 最多 3 页 |

**只显示还没提醒的闹钟**：`ReminderManager::List()` 本身就满足——单次提醒在 `FireDue()` 里 `items_.erase()`，每日重复的滚到下一次，所以页面不需要二次过滤。

### 9.4 交互闭环

| 触发 | 行为 |
|---|---|
| `self.memo.show` / `self.disp.switch(mode=memo)` | 切到备忘录页并刷新 |
| `self.memo.list` | 切页 + 返回清单文本 |
| `self.memo.add` | 新增后自动切页，用户看得见"确实记下了" |
| `self.memo.page(direction=next/prev)` | 翻页，返回「第 N 页，共 M 页」 |
| 闹钟到点 | 先 `SwitchToWeatherPage()` 再 `Alert()`，否则弹窗画在隐藏页上 |
| 按下说话（Connecting/Listening） | 若停在备忘录页则自动回主页，让识别文字可见；**Speaking 不切**，这样"打开备忘录/下一页"的结果能留在目录页 |

### 9.5 编译结果

| 项 | 值 |
|---|---|
| 结果 | `Project build complete`，改动文件零 warning（仅 esp_codec_dev 的 `ESP_IDF_VERSION` 环境提示） |
| `xiaozhi.bin` | 4,798,352 B（0x493b90） |
| app 分区剩余 | 378,480 B（0x5c470，7%） |
| 踩坑 | 新增 `memo_ui.cc` 必须 `idf.py reconfigure`；`self.memo.add` 的 lambda 要 `[this]` 才能用 `display_` |

> **字号上限**：`font_puhui_20_4` 的目标文件 1.25 MB，比 16px 版多约 375 KB，
> 而 app 分区只剩 378 KB —— 一上就是 0 余量。所以中文正文锁死 16px，
> "看起来大"靠 24px 数字 + 整块反白 + 黑白对比来做，而不是再引一套字号。

### 9.6 预览与验证

预览脚本 `docs/preview/render.py` 复刻固件坐标，产出：

- `memo-list.png` / `memo-list-1x.png`：4 条 + 3 页分页
- `memo-empty.png`：空列表提示
- `memo-two.png`：2 条时卡片自动收缩
- `music-playing.png` / `music-paused.png`：唱片、歌词、进度

上机看点：

| 日志 | 期望 |
|---|---|
| `MemoUI: 备忘录页创建完成` | 版面起来 |
| `MemoUI: 切换到备忘录页（N 条 / M 页）` | 语音"打开备忘录"生效 |
| `ReminderManager: 触发提醒: [07:30] 吃药` | 响铃 + 该条从目录消失 + 页面自动回主页 |
| `MemoUI: 备忘录翻到第 2/2 页` | 语音"下一页"生效 |

## 10. 大字号改造：20px 1bpp 子集 + 分区表重排

### 10.1 为什么不能直接用组件的 20px 字体

| 字体 | 目标文件 | 说明 |
|---|---|---|
| `font_puhui_16_4` | 858 KB | 现用正文，4bpp 抗锯齿，7415 字 |
| `font_puhui_20_4` | **1225 KB** | 组件自带全量 20px，比 16px 多 375 KB |
| `font_puhui_basic_20_4` | 130 KB | 只有 ASCII + 西文，**一个汉字都没有**，不能用 |

改造前 app 分区只剩 378 KB，20px 全量一上就是 0 余量。

### 10.2 解法：自建 20px 1bpp 子集

1-bit 屏最终要在 flush 回调里按 `*buffer < 0x7fff` 二值化，4bpp 的灰阶抗锯齿**本来就会被砍掉**。
既然如此，字体直接产出 1bpp 更省也更锐。

```
lv_font_conv --font managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf \
  --size 20 --bpp 1 --format lvgl --no-compress \
  -r 0x20-0x7F -r 0x3000-0x303F -r 0xFF00-0xFF5E \
  --symbols <GB2312 一级汉字 3755 字> --symbols "…—·" \
  -o main/boards/waveshare-s3-rlcd-4.2/assets/fonts/font_puhui_20_1.c \
  --lv-include lvgl.h --force-fast-kern-format
```

| 项 | 值 |
|---|---|
| 产物 | `assets/fonts/font_puhui_20_1.c`（1437 KB 源码 / **约 222 KB 目标码**） |
| 字符集 | GB2312 一级汉字 3755 + ASCII + 中文标点 + 全角符号 |
| 度量 | `line_height = 21`、`base_line = 4`（16px 版是 20/5） |
| 缺字兜底 | `.fallback = &font_puhui_16_4`，生僻字自动落到 16px 全量字库，不留空白方块 |

> **RLE 压缩无效**：`CONFIG_LV_USE_FONT_COMPRESSED=y` 后 1bpp 字体仍输出 `.bitmap_format = 0`。
> 汉字位图熵太高，lv_font_conv 判定不可压缩，所以最终把该开关改回 `n`。

二值化观感验证（`font-compare.png`）：16px 的「微/雪/期」等密笔画字已经粘连，20px 干脆不糊。

### 10.3 分区表重排（必须整片重烧）

原表把 6 MB 给了 assets，实际 `generated_assets.bin` 只有 **2.7 MB**，3.3 MB 是空的。

```
# 改前                                  改后
ota_0  0x20000  0x4F0000 (5.17 MB)   →  0x20000  0x5F0000 (5.94 MB)
ota_1  0x510000 0x4F0000             →  0x610000 0x5F0000
assets 0xA00000 6M                   →  0xC00000 4M
```

- **NVS 偏移（0x9000）不变** → WiFi 配置、备忘条目不会丢
- assets 偏移变了 → 必须连 assets 分区一起烧，`idf.py flash` 会自动处理
- 备份在 `partitions/v2/16m.csv.bak-assets6m`

### 10.4 版式调整

| 页面 | 改动 |
|---|---|
| 备忘录 | 正文 16 → **20px**；标题同步 20px，头部胶囊 26 → **30px 高**（20px 标题在 26px 里只剩 3px 呼吸）；卡片高改为 `行数 × 53`（上下各留 4px 对称）；**页脚跟随卡片**，2 条时不把「共 N 项待办」钉在屏底 |
| 播放器 | 歌词 4 行 → **3 行**（协议本身只给 上/当前/下 三句，第 4 行一直是空的）；行高 22 → 30；当前句 20px、上下句 16px；曲名 20px、歌手 14 → 16px |
| 主页 | 未动（时钟已是 48px，备忘卡仅 158px 宽，20px 只放得下 7 个字） |

### 10.5 编译结果

| 项 | 改前 | 改后 |
|---|---|---|
| `xiaozhi.bin` | 4,798,352 B | **5,022,608 B** |
| app 分区 | 5,177,344 B | **6,225,920 B** |
| 剩余 | 378,480 B（7%） | **1,202,288 B（19%）** |

### 10.6 踩坑

| 现象 | 原因 |
|---|---|
| `LV_FONT_DECLARE` 报 `implicit-int` | 声明被插到了 `#include "lvgl.h"` 之前，宏还没定义 |
| `'font_puhui_16_4' undeclared` | 同上，fallback 引用了还没声明的符号 |
| 字体头注释几 KB 长 | lv_font_conv 把整个 `--symbols` 汉字串写进 Opts 注释，手工截短 |
| 1x 预览糊成一团 | 从 3x 用 NEAREST 降采样会吃掉 1px 笔画，改成按 `S=1` 逐像素直出再阈值化 |
| 预览标题偏右 | 手写字间距的分支强制 `anchor='la'`，且调用漏传 `anchor='ma'` |

### 10.7 重新生成字体的方法

`build/fontgen/l1.txt` 是 GB2312 一级字表（3755 字），配合 10.2 的命令即可重建。
生成后需要手动补两处：符号名 `f20_l1` → `font_puhui_20_1`、`.fallback = &font_puhui_16_4`。
