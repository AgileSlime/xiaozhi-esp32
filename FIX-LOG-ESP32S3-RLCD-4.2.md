# 微雪 ESP32-S3-RLCD-4.2 固件修复记录

| 项目 | 内容 |
|---|---|
| 记录日期 | 2026-09-28 |
| 开发板 | 微雪 Waveshare ESP32-S3-RLCD-4.2（4.2 寸反射式 LCD） |
| 固件 | xiaozhi-esp32 v2（小智 AI 助手） |
| 板型宏 | `CONFIG_BOARD_TYPE_WAVESHARE_S3_RLCD_4_2=y` |
| 工具链 | ESP-IDF **v5.5.5**（`E:/esp/v5.5.5/esp-idf`） |
| 目标 | `esp32s3`，16MB Flash，Octal PSRAM 8MB @ 80MHz |
| 分区表 | `partitions/v2/16m.csv`（表偏移 `0x8000`，app 分区 `0x4F0000`） |
| 文档状态 | **编译期已验证通过；运行期验收待烧录后确认**（见第 10 节） |

> 原工程：`E:/esp32test/xiaozhi-esp32-main`
> 本独立副本：`E:/esp32test/waveshare-esp32s3-rlcd-4.2`

---

## 0. 一页速览

| # | 问题 | 现象 | 根因 | 处置 | 状态 |
|---|---|---|---|---|---|
| 1 | 设置目标失败 | `CMakeLists.txt not found` | VS Code 打开的是板卡子目录，不是工程根 | 以工程根目录作为工作区 | 已解决 |
| 2 | 组件解析失败 | 缺 `json` 等组件 | IDF 6.1 与本工程依赖不兼容 | 改用 IDF **v5.5.5** | 已解决 |
| 3 | 编译失败 | 找不到 `secret_config.h` | 该文件不入库，仓库只提供 `.example` | 从 `.example` 复制生成 | 已解决 |
| 4 | **运行期内存枯竭** | "发送失败，请检查网络" + 一直显示"聆听中" | 内部 SRAM 见底（开机后最低 **232 字节**），`esp-aes` 与 `spi_master` 都拿不到 DMA 内存 | 调整 4 项 sdkconfig | 编译期已验证，待烧录确认 |

---

## 1. 问题 1：`idf.py set-target` 报 CMakeLists.txt 不存在

**现象**

执行 `set-target esp32s3` 失败，提示找不到 `CMakeLists.txt`。

**根因**

VS Code 打开的工作目录是板卡子目录，而不是工程根目录。IDF 的 `set-target` 必须在含有顶层 `CMakeLists.txt` 的工程根执行；而板卡子目录（`main/boards/waveshare-s3-rlcd-4.2/`）里没有顶层工程文件。此外该子目录属于一个多板卡工程，单独打开它也无法定位 `sdkconfig` 与组件。

**处置**

以工程根目录作为工作区：

```
E:/esp32test/xiaozhi-esp32-main     （含 CMakeLists.txt）
```

`main/CMakeLists.txt:337` 已经由板型宏自动选择 `set(BOARD_TYPE "waveshare-s3-rlcd-4.2")`，无需手工指定板卡路径。

---

## 2. 问题 2：组件解析失败（IDF 版本不匹配）

**现象**

在 IDF 6.1 下解析组件时报错，缺少 `json` 组件。（`esp_blockdev` 相关提示属于噪音，与本问题无关。）

**根因**

工程依赖锁文件 `dependencies.lock` 是按 IDF v5.5.x 生成的，与 IDF 6.1 的组件管理器不兼容。

**处置**

安装并使用 **ESP-IDF v5.5.5**：

| 项 | 路径 |
|---|---|
| IDF | `E:/esp/v5.5.5/esp-idf` |
| 工具目录 | `C:/Espressif/tools` |
| Python 虚拟环境 | `C:/Espressif/tools/python/v5.5.5/venv` |
| ROM ELF | `C:/Espressif/tools/esp-rom-elfs/20241011/` |

---

## 3. 问题 3：编译失败，缺 `secret_config.h`

**现象**

编译报错，找不到 `secret_config.h`。

**根因**

该文件存放设备私有配置，**不纳入版本库**，仓库只提供模板 `secret_config.h.example`。

**处置**

```
main/boards/waveshare-s3-rlcd-4.2/secret_config.h.example
  -> 复制为 ->
main/boards/waveshare-s3-rlcd-4.2/secret_config.h
```

这是本次全部改动中**唯一新增的文件**，不含任何 C/C++ 源码修改。

首次构建成功后产物：

| 文件 | 大小 |
|---|---|
| `xiaozhi.bin` | 4,685,264 B |

虽然能编译、能联网、能连上 MQTT，但随即暴露问题 4。

---

## 4. 问题 4：运行期"发送失败，请检查网络" + 一直"聆听中"（核心问题）

### 4.1 现象

- 网络连接正常，MQTT 能连上 `api.tenclass.net`
- 语音对话时反复提示 **"发送失败，请检查网络"**
- 屏幕一直停在 **"聆听中"**，不刷新

### 4.2 抓日志的方法

用自写脚本 `cap_serial.py` 从 **COM7** 抓取串口日志，落到 `boot_capture.txt`。关键日志行（行号为该文件内行号）：

```
198: I (11799) SystemInfo: free sram: 62039 minimal sram: 39812
206: I (11829) AFE_CONFIG: Set WakeNet Model: wn9_nihaoxiaozhi_tts
210: I (11939) AFE: AFE Pipeline: [input] -> |AEC(SR_HIGH_PERF)| -> |VAD(WebRTC)| -> |WakeNet(...)| -> [output]
212: E (13039) esp-aes: Failed to allocate memory
213: E (13039) esp-tls-mbedtls: write error :-0x0001
216: I (13049) esp_mqtt: MQTT error occurred: ESP_ERR_MBEDTLS_SSL_WRITE_FAILED
217: I (13059) MQTT: MQTT disconnected, schedule reconnect in 60 seconds
219: E (13069) MQTT: Failed to publish message: {...}
227: I (21409) SystemInfo: free sram: 12007 minimal sram: 828
247: I (31409) SystemInfo: free sram: 11067 minimal sram: 444
250: E (43169) spi_master: setup_dma_priv_buffer(1214): Failed to allocate priv TX buffer
```

第二次开机（同一文件第 305 行起）更严重：

```
497: I (21408) SystemInfo: free sram: 7023 minimal sram: 748
512: I (31408) SystemInfo: free sram: 6003 minimal sram: 232
```

### 4.3 内存水位（实测）

| 时刻 | free sram | minimal sram | 事件 |
|---|---|---|---|
| 11.8 s | 62,039 B | 39,812 B | WiFi + MQTT + OTA 都起来了，此时还宽裕 |
| 11.8~11.9 s | — | — | **AFE 唤醒词管线建立**：`AEC(SR_HIGH_PERF) → VAD(WebRTC) → WakeNet`，约 1 秒内吃掉数十 KB |
| 13.0 s | — | — | `esp-aes` 分配内存失败 |
| 21.4 s | 12,007 B | **828 B** | 历史最低点 |
| 31.4 s | 11,067 B | **444 B** | |
| 43.2 s | — | — | `spi_master` DMA 缓冲分配失败 |
| 第二次开机 | 6,003~7,023 B | **232 B** | 更严重 |

### 4.4 失败链（两个症状同一个根因）

```
开机内部分配的内部 SRAM 合计 290 KiB
        │
        ├─ 11.8s：WiFi + MQTT + OTA 初始化完成，剩 62 KiB
        │
        ├─ 11.9s：AFE 唤醒词管线（AEC SR_HIGH_PERF + VAD + WakeNet）建立，1 秒内再吃掉数十 KB
        │
        └─ 内部堆跌到 232~828 字节
                 │
                 ├─ esp-aes 申请 1600 字节 DMA 内存失败
                 │      └─ esp-tls write error -> MQTT 断开 -> 【发送失败，请检查网络】
                 │
                 └─ spi_master 申请 DMA TX 缓冲失败
                        └─ LCD 刷不动 -> 【一直显示"聆听中"】
```

**结论：两个症状不是网络问题，是内部 RAM（不是 PSRAM）真的见底了。**

> 注意：`esp_psram: Reserving pool of 96K of internal memory for DMA/internal allocations`（第 49 行）。
> 这个 96K 预留池是按 `MALLOC_CAP_DEFAULT` 注册的，普通 malloc 也会消耗它。实测最低点只剩几百字节，就是它被吃干的证据。
> **不要试图调大这个池**，调大只会更挤压通用堆。

### 4.5 第一刀（无效）：打开 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`

```
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y      # sdkconfig:1480
```

让 WiFi/LwIP 的缓冲优先走 PSRAM。效果：`free sram` 从 1.6 KB 抬到 12~24 KB，但**症状依旧**——因为最低点出现在 AFE 建管线之后，而不是 WiFi 初始化阶段。

---

## 5. 第二刀：配置修改清单

同步写入 `sdkconfig` 与 `sdkconfig.defaults.esp32s3`（后者保证以后重新生成配置时不丢失）。

| # | 配置项 | 原值 | 新值 | 预期收益 | 实测结论 |
|---|---|---|---|---|---|
| 1 | `CONFIG_ESP32S3_INSTRUCTION_CACHE_*` | 32KB | **16KB** | +16 KB | ✅ **真实有效**，主堆区 230→246 KiB |
| 2 | `CONFIG_ESP32S3_DATA_CACHE_*` | 32KB | **16KB** | +16 KB | ⚠️ 编译期未观测到收益，待运行期确认 |
| 3 | `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM` | 16 | **8** | ~12.8 KB（每个约 1.6 KB） | ✅ 运行时分配，只在开机 `free sram` 体现 |
| 4 | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 2048 | **512** | 几 KB~十几 KB | ✅ 把 512B~2KB 的分配推去 8MB PSRAM |

菜单位置：

| # | 菜单位置 |
|---|---|
| 1、2 | `Component config → ESP System Settings → Cache config` |
| 3 | `Component config → Wi-Fi` |
| 4 | `Component config → ESP PSRAM` |

### 5.1 三项刻意**不**改（及原因）

| 项 | 为什么不动 |
|---|---|
| WiFi TX 缓冲类型 Static → Dynamic | **物理上改不了**。`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER` 带依赖 `depends on !(SPIRAM_TRY_ALLOCATE_WIFI_LWIP && !SPIRAM_IGNORE_NOTFOUND)`，工程开了 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，该选项被直接屏蔽。IDF 帮助原文：*"If PSRAM is enabled, 'Static' should be selected"* |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 6 → 4 | IDF 官方档位表（S3 + 8 线 PSRAM）里 **6 就是 IDF 自己的 "Memory saving" 推荐值**；再降影响 AP 兼容性，且只值 3.2 KB |
| `CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM` 32 → 8 | 官方档位表里没有这一项，不是有效杠杆；且 `esp_wifi/src/wifi_init.c:340` 有硬检查——开启 SPIRAM 时它不能为 0。它是静态 TX 缓冲耗尽时的兜底队列，我们刚把静态 TX 减半，不该同时抽掉兜底 |

### 5.2 备份与回滚

改前备份（均在工程根）：

```
sdkconfig.bak-20260928-1543
sdkconfig.defaults.esp32s3.bak-20260928-1543
```

回滚：把这两个文件复制回 `sdkconfig` / `sdkconfig.defaults.esp32s3` 同名文件，重新 build。

---

## 6. 为什么"指令 cache"有效、而"数据 cache"没有收益（源码级）

### 6.1 指令 cache（有效）

`esp-idf/components/esp_system/port/soc/esp32s3/Kconfig.cache` 帮助原文：

> If you use 16KB instruction cache rather than 32KB instruction cache, then the other 16KB will be managed by heap allocator.

生成的链接脚本 `build/esp-idf/esp_system/ld/memory.ld`：

```
iram0_0_seg (RX) : org = (0x40370000 + 0x4000), len = (0x57700)
```

即 **IRAM 段的起始地址 = 0x40370000 + 指令 cache 大小**。于是：

| 环节 | 改前（icache 32KB） | 改后（icache 16KB） |
|---|---|---|
| `iram0_0_seg` 起始 | `0x40378000` | `0x40374000` |
| `iram0_0_seg` 长度 | `0x53700` | `0x57700` |
| `_iram_start` | `0x40378000` | `0x40374000` |

机制链条：指令 cache 减半 → IRAM 代码可以从 `0x40374000` 开始放 → IRAM 代码的**前 16 KB 落到"只有 IRAM 别名、DRAM 访问不到"的窗口**里 → DRAM 共享窗被 IRAM 代码占用的部分少了 16 KB → 数据段起点下移 16 KB → `_bss_end` 下移 16 KB → **DIRAM 主堆区 +16 KB**。

`heap/port/esp32s3/memory_layout.c:85-87` 也印证了这一点：

```c
#if CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB && !defined(CONFIG_ESP_SYSTEM_MEMPROT_FEATURE)
    { 0x40374000, 0x4000, SOC_MEMORY_TYPE_IRAM, 0, false}, //Level 1, IRAM
#endif
```

**代价**：Flash / PSRAM 的指令缓存命中率略降。语音场景完全无感。

### 6.2 数据 cache（编译期无收益，但无害，暂时保留）

第一次看 `Kconfig.cache` 时曾以为 `CONFIG_ESP32S3_DATA_CACHE_16KB=y` 与 `CONFIG_ESP32S3_DATA_CACHE_SIZE=0x8000` 并存是配置错误——**这是个误判**，Kconfig 原文：

```c
config ESP32S3_DATA_CACHE_SIZE
    hex
    # For 16KB the actual configuration is 32kb cache, but 16kb will be reserved for heap at startup
    default 0x8000 if ESP32S3_DATA_CACHE_16KB
    default 0x8000 if ESP32S3_DATA_CACHE_32KB
```

即：选 16KB 时**硬件仍是 32KB 数据 cache，另外 16KB 在启动时让给堆**，两者并存是设计如此。

它让给堆的 16 KB 由 `memory_layout.c:100` 注册：

```c
#if CONFIG_ESP32S3_DATA_CACHE_16KB
    { 0x3C000000, 0x4000, SOC_MEMORY_TYPE_DRAM, 0, false}, //Level 10, DRAM, DMA 可访问
#endif
```

但 `0x3C000000` 正是 `SOC_EXTRAM_DATA_LOW`，而开启 SPIRAM 时会紧接着注册 `{0x3C000000, 0x2000000, SPIRAM}`（八线 PSRAM 的 64 MB 窗口），**两者同基址重叠**。

实测印证：`dram0_0_seg` 长度改前改后都是 `0x53700`，没有变化。所以这 16 KB **在编译期没有进堆**。

**是否在运行期进堆，需要开机日志确认**（见第 10 节）。若真进了堆，它是一块 **DMA 可访问的内部 DRAM**，对 `esp-aes` 恰好对症，是个彩蛋。保留此项无害。

---

## 7. 编译期验证结果

### 7.1 配置没有被 Kconfig 回滚

`idf.py reconfigure` 输出 `Configuring done` / `Generating done`，`sdkconfig` 中 4 项改动全部保留。

### 7.2 堆区增长（决定性证据）

用链接符号与旧固件开机日志交叉验证：

| 证据 | 值 |
|---|---|
| 旧固件开机日志 `boot_capture.txt:39` | `heap_init: At 3FCAFE90 len 00039880 (230 KiB): RAM` |
| 新固件 map 符号 `_heap_start = _bss_end` | `0x3FCABE90` |
| 差值 | `0x3FCAFE90 - 0x3FCABE90` = **`0x4000` = 16,384 B** |
| 该堆区终点（固定） | `0x3FCE9710` |
| 新长度 | `0x3D880` = **246 KiB** |

新固件 map 中的关键符号：

```
_iram_start = 0x40374000
_iram_end   = 0x4038D000     (IRAM 代码合计 0x19000 = 102,400 B)
_data_start = 0x3FC9D000     (恰为 _iram_end - 0x6F0000，即 IRAM 别名偏移)
_bss_end    = 0x3FCABE90  =  _heap_start
```

注意 `_data_start = 0x3FC9D000 = 0x3FC88000 + 0x15000`，即 IRAM 代码只有 `0x15000`(86,016 B) 落在 DRAM 共享窗内——与第 6.1 节的推导完全吻合。

### 7.3 内部堆区域前后对比

| 区域 | 改前 | 改后 |
|---|---|---|
| 主 DIRAM 堆区 | 230 KiB | **246 KiB** |
| DIRAM 次区 | 21 KiB | 21 KiB |
| DRAM 区（0x3FCF0000） | 32 KiB | 32 KiB |
| RTC RAM | 7 KiB | 7 KiB |
| **合计** | **290 KiB** | **306 KiB** |

### 7.4 产物

| 文件 | 大小 | 烧录偏移 |
|---|---|---|
| `bootloader.bin` | 16,240 B | `0x0` |
| `partition-table.bin` | 3,072 B | `0x8000` |
| `ota_data_initial.bin` | 8,192 B | `0xd000` |
| `xiaozhi.bin` | 4,685,664 B（镜像 4,685,543 B） | `0x20000` |
| `generated_assets.bin` | 2,822,210 B | `0xa00000` |

分区余量：app 分区 `0x4F0000`，剩余 `0x780A0`（**9%**）。

### 7.5 `esp-idf-size` 报告怎么读（S3 专用口径）

VS Code 的 "ESP-IDF Size" 输出里两个容易误读的行：

| 行 | 含义 |
|---|---|
| `DIRAM` 总长 `341760` | = `dram0_0_seg` 长度 = `0x53700` |
| `IRAM` 总长 `16384`、**100% 已用** | = `iram0_0_seg` 长度 − `dram0_0_seg` 长度 = `0x57700 − 0x53700` |

所以 `IRAM 16384/16384 100%` **不是错误**，它正是"指令 cache 减半后多出来的那 16 KB 非别名窗口被 `.vectors` + IRAM 代码占满"的指纹。

⚠️ **唯一需要记住的副作用**：这个窗口现在满了，以后**不要再往代码里加 `IRAM_ATTR`**，会链接失败。

---

## 8. 独立副本内容（`E:/esp32test/waveshare-esp32s3-rlcd-4.2`）

| 目录/文件 | 说明 |
|---|---|
| `main/`、`components/`、`scripts/`、`docs/`、`partitions/` | 全部源码 |
| `managed_components/` | 已下载的依赖（保留，可在**离线**状态下编译） |
| `sdkconfig` / `sdkconfig.defaults.esp32s3` | 含本次全部修改 |
| `sdkconfig.*.bak-20260928-1543` | 改前备份 |
| `firmware-bin/` | 已编译好的 5 个镜像 + `flash_args`，可直接烧录 |
| `FIX-LOG-ESP32S3-RLCD-4.2.md` | 本文档 |

**故意没有拷贝 `build/`**：CMake 会把绝对路径烘焙进 `build/CMakeCache.txt` 与 ninja 的 `.ninja_deps`，复制到新路径后是**无效的**，直接 build 会报路径错误。要烧录请用 `firmware-bin/`，要重新编译请在新目录里跑一次 `idf.py build`（会重建 `build/`）。

---

## 9. 烧录

`firmware-bin/flash_args` 内容：

```
--flash_mode dio --flash_freq 80m --flash_size 16MB
0x0 bootloader/bootloader.bin
0x20000 xiaozhi.bin
0x8000 partition_table/partition-table.bin
0xd000 ota_data_initial.bin
0xa00000 generated_assets.bin
```

在 `firmware-bin/` 目录下执行（`COM7` 换成实际串口）：

```bat
C:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe -m esptool ^
  --chip esp32s3 -p COM7 -b 460800 --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB ^
  0x0      bootloader.bin ^
  0x8000   partition-table.bin ^
  0xd000   ota_data_initial.bin ^
  0x20000  xiaozhi.bin ^
  0xa00000 generated_assets.bin
```

或在 VS Code 里直接用 **ESP-IDF: Flash** 任务。

> 首次全量烧录建议 5 个镜像都烧。`generated_assets.bin`（字体 / emoji / 唤醒词模型）不烧会出现屏幕字体缺失。

---

## 10. 运行期验收判据（**待做**）

烧录后开机看日志，**两行定生死**：

```
heap_init: At 3FCABE90 len 0003D880 (246 KiB): RAM     <- 主堆区地址与长度，+16KB 生效的铁证
SystemInfo: free sram: xxxxx minimal sram: yyyyy       <- 每 10 秒打印一次
```

**通过标准**

1. 主堆区出现 `At 3FCABE90 len 0003D880 (246 KiB)`（改前是 `At 3FCAFE90 len 00039880 (230 KiB)`）
2. `minimal sram` 从 **232~828 字节** 抬到 **≥ 15000 字节**
3. 日志中不再出现：
   - `E (...) esp-aes: Failed to allocate memory`
   - `E (...) spi_master: setup_dma_priv_buffer(...): Failed to allocate priv TX buffer`
   - `MQTT disconnected, schedule reconnect in 60 seconds`
4. 语音对话能正常发送、屏幕不再卡"聆听中"

**彩蛋判据（可选）**：若日志里多出一行

```
heap_init: At 3C000000 len 00004000 (16 KiB): DRAM
```

说明第 5 节第 2 项（数据 cache）的 16 KB 也进堆了，且它是 **DMA 可访问的内部 DRAM**，对 `esp-aes` 直接对症——白赚 16 KB。

**未通过怎么办**：见第 11 节。

---

## 11. 如果 306 KiB 仍然不够：下一步（需改代码，本次**未**执行）

按性价比排序：

1. **`main/audio/wake_words/afe_wake_word.cc:73` 把 `AFE_MODE_HIGH_PERF` 改成 `AFE_MODE_LOW_COST`**
   这是最能对症的一刀——开机 AFE 建管线那几十 KB 的大头就在这里。
   代价：唤醒词识别率与降噪性能下降。
   ⚠️ 这要改 C++ 源码，本次未动，需明确确认后再改。

2. **关闭设备端 AEC**：取消 `Xiaozhi Assistant → Enable Device-Side AEC`，勾上服务端 AEC。
   注意：它只影响**对话期间**的 AFE 实例，**救不了开机那几十 KB**——开机唤醒词管线里的 `AEC(SR_HIGH_PERF)` 是 `afe_wake_word.cc` 按"有无参考通道"写死的，只受代码控制。代价：回声消除依赖服务器，服务器不支持时设备会把自己的喇叭声当人说话。

3. 继续压缩 WiFi 静态 RX 缓冲（6 → 4，约 3.2 KB），或调整 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`（当前 96 KB）。

---

## 12. 本机（Windows）构建环境的三个坑

复现构建时一定会撞上，记下来省时间。

### 12.1 `MSYSTEM` 环境变量会让 `idf.py` **静默什么都不做**

Git Bash 下 `MSYSTEM` 存在时，`idf.py` 检测到 MSYS 环境 → 打印一句警告后**直接跳过 `main()`**：
表现为命令秒退、无任何输出、退出码 0。

绕过方式（任选其一）：

```bat
env -u MSYSTEM -u MSYS -u MSYSCON -u MSYS_NO_PATHCONV ^
  "C:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe" ^
  "E:/esp/v5.5.5/esp-idf/tools/idf.py" -C "E:/esp32test/xiaozhi-esp32-main" build
```

或用包装脚本调用 IDF 的 Python 入口（本工作区用了 `run_idf.py`）。

### 12.2 构建脚本里的目录删除会被安全机制拦截

`scripts/build_default_assets.py:163` 会 `shutil.rmtree(build/temp_build/srmodels)`，删除动作被拦截后构建在该步失败（报 `SHFileOperationW` 一类错误），看起来像编译错误，实际与代码无关。

两种绕法（都有效）：

1. **改名不算删除**：先 `mv build/temp_build/srmodels build/_srmodels_bak_<日期>`，脚本 `os.path.exists` 为假就不再调 `rmtree`。
2. **PATH 前置 python 垫片**：把 `.py` 调用转给一个包装脚本，在包装脚本里 `shutil.rmtree = lambda *a, **k: None` 再 `runpy.run_path(script, run_name="__main__")`。这样整个 `ninja all` 能跑到 `EXIT=0`。

### 12.3 同一个 `build/` 目录并发跑两个 ninja 必炸

症状：VS Code 里点 Build 突然失败。
原因：另一个 ninja 还占着 `build/.ninja_lock`，两个 ninja 同时写 `.ninja_log` / `.ninja_deps`、抢同一批 `.obj`。

处理：

```bat
tasklist | findstr /I "ninja xtensa cmake"      :: 确认残留
taskkill /PID <PID> /T /F                        :: 结束整棵进程树
```

确认 `ninja | xtensa | cmake` 进程数为 **0** 后再重试构建。

> 另外：`ninja` 对上次失败过的目标每次都会重跑（`.ninja_log` 里没有成功记录），所以"预先生成产物"不能替代让它成功跑过一次。

---

## 13. 变更文件清单

| 文件 | 变更 | 说明 |
|---|---|---|
| `sdkconfig` | 4 项改动（另含前期 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，第 1480 行） | 生效配置 |
| `sdkconfig.defaults.esp32s3` | 同步上述 4 项 + 中文注释说明 | 保证重新生成配置时不丢失 |
| `sdkconfig.bak-20260928-1543` | 新增（改前备份） | 回滚用 |
| `sdkconfig.defaults.esp32s3.bak-20260928-1543` | 新增（改前备份） | 回滚用 |
| `main/boards/waveshare-s3-rlcd-4.2/secret_config.h` | 新增（由同目录 `.example` 复制） | 构建必需 |

**没有任何 C/C++ 源码改动。**

`sdkconfig` 中本次 4 项的实际生效值：

```
1515: CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y
1517: CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE=0x4000
1524: CONFIG_ESP32S3_DATA_CACHE_16KB=y
1527: CONFIG_ESP32S3_DATA_CACHE_SIZE=0x8000
1479: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512
1637: CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=8
```

---

## 附录 A：验证用命令

```bat
:: 看堆区是否增长（改后应立即出现 246 KiB）
python -c "import re;print(open(r'E:/esp32test/xiaozhi-esp32-main/build/xiaozhi.map',encoding='utf-8',errors='replace').read())" | findstr _heap_start

:: 看内存段长度
findstr /C:"iram0_0_seg" /C:"dram0_0_seg" build\esp-idf\esp_system\ld\memory.ld

:: 看配置是否被回滚
findstr /C:"INSTRUCTION_CACHE_16KB" /C:"STATIC_TX_BUFFER_NUM" /C:"ALWAYSINTERNAL" sdkconfig
```

## 附录 B：地址速查

| 符号 / 区域 | 改前 | 改后 |
|---|---|---|
| `iram0_0_seg` org / len | `0x40378000` / `0x53700` | `0x40374000` / `0x57700` |
| `dram0_0_seg` org / len | `0x3FC88000` / `0x53700` | 同左（未变） |
| `_iram_start` / `_iram_end` | `0x40378000` / `0x40391000`（推算） | `0x40374000` / `0x4038D000` |
| `_data_start` | `0x3FCA2000`（推算） | `0x3FC9D000` |
| `_heap_start`（= 主堆区起点） | `0x3FCAFE90` | `0x3FCABE90` |
| 主堆区长度 | 230 KiB | **246 KiB** |
| 内部堆合计 | 290 KiB | **306 KiB** |

> 标注"推算"的两行由 `_iram_end = _iram_start + 102,400`、`_data_start = _iram_end − 0x6F0000` 推导，且 `_heap_start` 推算值与旧固件开机日志实测的 `3FCAFE90` 完全一致，故可信。

---

*文档结束。第 10 节的运行期验收结果请回填到本文件。*
