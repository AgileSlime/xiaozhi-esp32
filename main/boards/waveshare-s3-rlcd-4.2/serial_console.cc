#include "serial_console.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <driver/uart.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "custom_lcd_display.h"
#include "managers/music_stream.h"
#include "managers/photo_loader.h"
#include "wifi_manager.h"

static const char* TAG = "SerialCmd";

SerialConsole& SerialConsole::GetInstance() {
    static SerialConsole instance;
    return instance;
}

void SerialConsole::Reply(const char* fmt, ...) {
    char body[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    auto& self = GetInstance();
    std::lock_guard<std::mutex> lock(self.reply_mutex_);

    // 远程调用（WiFi 的 /cmd）时结果要一路带回去，不能只吐在串口上——
    // 那种情况下人根本没插线，写了也没人看。
    if (self.capture_) {
        self.reply_buf_ += body;
        self.reply_buf_ += '\n';
        return;
    }

    // 前后各来一个换行：日志是异步打的，不加的话命令回显会被日志粘在同一行里，
    // 电脑端脚本按 "CMD> " 前缀抓取时会连日志一起抓进去。
    char out[320];
    const int n = snprintf(out, sizeof(out), "\r\nCMD> %s\r\n", body);
    if (n > 0) {
        uart_write_bytes(UART_NUM_0, out, static_cast<size_t>(n < static_cast<int>(sizeof(out)) ? n : sizeof(out) - 1));
    }
}

std::string SerialConsole::Execute(const std::string& cmd) {
    // 串口和 HTTP 共用一套命令解析，这里加锁保证同一时刻只有一个在执行
    std::lock_guard<std::mutex> lock(cmd_mutex_);

    {
        std::lock_guard<std::mutex> rk(reply_mutex_);
        capture_ = true;
        reply_buf_.clear();
    }

    std::string copy = cmd;
    copy.push_back('\0');           // HandleLine 会就地改字符串，给个可写的结尾
    HandleLine(&copy[0]);

    std::lock_guard<std::mutex> rk(reply_mutex_);
    capture_ = false;
    if (reply_buf_.empty()) {
        return "OK\n";              // 命令执行了但没输出（比如空行）
    }
    return reply_buf_;
}

void SerialConsole::Start(CustomLcdDisplay* display) {
    if (task_ != nullptr) {
        return;
    }
    display_ = display;
    // 栈只能放内部 RAM：relay 命令会写 NVS，play 会拉起播放任务，
    // 写 Flash 期间 cache 关闭，PSRAM 栈会直接 assert。
    xTaskCreate(TaskEntry, "serial_cmd", 8192, this, 2, &task_);
}

void SerialConsole::TaskEntry(void* arg) {
    static_cast<SerialConsole*>(arg)->Run();
}

void SerialConsole::Run() {
    // 等日志刷完、系统起来，避免把开机日志吃掉
    vTaskDelay(pdMS_TO_TICKS(6000));

    // console 用的是 UART0（GPIO43/44，就是 USB 那个口）。
    // 固件里没开交互式 REPL，所以这里直接装驱动来收字节。
    // 已经装过（比如别的模块先装了）就沿用。
    if (!uart_is_driver_installed(UART_NUM_0)) {
        const esp_err_t err = uart_driver_install(UART_NUM_0, 4096, 0, 0, nullptr, 0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "UART0 驱动安装失败: %s，串口命令不可用", esp_err_to_name(err));
            return;
        }
    }

    // 清掉缓冲区里残留的启动噪音（有些串口工具上电会发几个 0x00 / 乱码）
    uart_flush_input(UART_NUM_0);

    ESP_LOGI(TAG, "串口命令通道就绪（电脑上发 help 看用法）");

    char line[192];
    int len = 0;

    while (true) {
        uint8_t c = 0;
        const int n = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(200));
        if (n <= 0) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                // 和 HTTP 的 /cmd 抢同一套解析器，串行一下
                {
                    std::lock_guard<std::mutex> lock(cmd_mutex_);
                    HandleLine(line);
                }
                len = 0;
            }
            continue;
        }

        // 退格 / DEL：让串口工具里能看到自己退掉了
        if (c == 0x08 || c == 0x7f) {
            if (len > 0) {
                len--;
            }
            continue;
        }

        if (len < static_cast<int>(sizeof(line)) - 1) {
            line[len++] = static_cast<char>(c);
        } else {
            // 行太长，丢弃并提示，别把后面的命令拼进来
            len = 0;
            Reply("ERR 命令太长（最多 190 字节）");
        }
    }
}

void SerialConsole::HandleLine(char* line) {
    // 去掉首尾空白
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    char* end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    if (*line == '\0') {
        return;
    }

    // 拆出命令和参数（参数保留原样，中文关键词中间可能有空格）
    char* arg = strchr(line, ' ');
    if (arg != nullptr) {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    } else {
        arg = line + strlen(line);   // 指向末尾的空串，不是 nullptr，省得各分支判空
    }

    const char* cmd = line;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        Reply("可用命令:");
        Reply("  ip                    板子当前 IP");
        Reply("  relay [host:port]     查看/设置音乐中转地址");
        Reply("  play <关键词>          搜歌并播放（会切到播放器页）");
        Reply("  pause                 暂停播放");
        Reply("  resume                继续播放");
        Reply("  stop                  停止播放");
        Reply("  volume [0-100]        查看/设置音量");
        Reply("  page <home|weather|memo|music>  切页面");
        Reply("  photo [reset|save]     查看/清除常驻壁纸");
        Reply("  status                一句话看当前状态");
        Reply("  reboot                重启");
        return;
    }

    if (strcmp(cmd, "ip") == 0) {
        const std::string ip = WifiManager::GetInstance().GetIpAddress();
        Reply(ip.empty() ? "FAIL 还没连上 WiFi" : "OK %s", ip.c_str());
        return;
    }

    if (strcmp(cmd, "relay") == 0) {
        auto& ms = MusicStream::GetInstance();
        if (*arg == '\0') {
            const std::string base = ms.Base();
            Reply(base.empty() ? "FAIL 还没设置中转地址" : "OK %s", base.c_str());
            return;
        }
        // 顺手做一次连通性检查，免得设了个打不通的地址还以为配好了
        ms.SetBase(arg);
        bool logged_in = false;
        std::string err;
        if (ms.Health(&logged_in, &err)) {
            Reply("OK 已设置 %s，中转在线（cookie=%s）",
                  ms.Base().c_str(), logged_in ? "有" : "无");
        } else {
            Reply("WARN 已设置 %s，但连不上: %s", ms.Base().c_str(), err.c_str());
        }
        return;
    }

    if (strcmp(cmd, "play") == 0) {
        if (*arg == '\0') {
            Reply("ERR 用法: play <关键词>");
            return;
        }
        Reply("SEARCH 正在找《%s》...", arg);

        auto& ms = MusicStream::GetInstance();
        const auto r = ms.Play(arg);
        if (!r.ok) {
            Reply("FAIL %s", r.reason.c_str());
            return;
        }
        if (display_ != nullptr) {
            display_->SwitchToMusicPage();
        }
        const bool started = Application::GetInstance().PlayMusicFromUrl(
            r.url, r.song.title, r.song.artist, r.lyric, "",
            0, false, static_cast<uint32_t>(r.song.duration_ms), r.cover);
        if (!started) {
            Reply("FAIL 拿到直链了，但播放链路没起来（可能正在说话）");
            return;
        }
        Reply("OK 正在播放: %s - %s", r.song.title.c_str(), r.song.artist.c_str());
        return;
    }

    if (strcmp(cmd, "stop") == 0) {
        Application::GetInstance().StopMusicPlayback();
        Reply("OK 已停止");
        return;
    }

    if (strcmp(cmd, "pause") == 0) {
        bool ok = Application::GetInstance().PauseMusicPlayback();
        Reply(ok ? "OK 已暂停" : "ERR 当前没在放歌");
        return;
    }

    if (strcmp(cmd, "resume") == 0) {
        bool ok = Application::GetInstance().ResumeMusicPlayback();
        Reply(ok ? "OK 已继续" : "ERR 当前没在暂停");
        return;
    }

    if (strcmp(cmd, "volume") == 0) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            Reply("ERR 音频还没初始化");
            return;
        }
        if (*arg == '\0') {
            // 查看音量
            Reply("OK 当前音量=%d", codec->output_volume());
            return;
        }
        const int v = atoi(arg);
        if (v < 0 || v > 100) {
            Reply("ERR 音量范围 0-100");
            return;
        }
        codec->SetOutputVolume(v);
        Reply("OK 音量已设为 %d", v);
        return;
    }

    if (strcmp(cmd, "page") == 0) {
        if (display_ == nullptr) {
            Reply("ERR 显示还没初始化");
            return;
        }
        if (strcmp(arg, "home") == 0) {
            display_->SwitchToWeatherPage();   // 天气页 = 主页
        } else if (strcmp(arg, "weather") == 0) {
            display_->SwitchToWeatherPage();
        } else if (strcmp(arg, "memo") == 0) {
            display_->SwitchToMemoPage();
        } else if (strcmp(arg, "music") == 0) {
            display_->SwitchToMusicPage();
        } else {
            Reply("ERR 用法: page <home|weather|memo|music>");
            return;
        }
        Reply("OK 已切到 %s", arg);
        return;
    }

    if (strcmp(cmd, "status") == 0) {
        auto& app = Application::GetInstance();
        auto& ms = MusicStream::GetInstance();
        Reply("IP=%s  中转=%s  在放歌=%s  内部RAM=%u",
              WifiManager::GetInstance().GetIpAddress().c_str(),
              ms.Base().empty() ? "(未设置)" : ms.Base().c_str(),
              app.IsMusicPlaying() ? "是" : "否",
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        return;
    }

    if (strcmp(cmd, "photo") == 0) {
        auto& photo = HomePhoto::GetInstance();
        if (strcmp(arg, "reset") == 0) {
            photo.ForgetSavedOverride();
            if (display_ != nullptr) {
                display_->LoadHomePhoto(-1);
            }
            Reply("OK 已恢复内置壁纸（NVS 里那份也删了）");
            return;
        }
        if (strcmp(arg, "save") == 0) {
            Reply(photo.SaveOverride() ? "OK 已存入 NVS，重启后仍是这张" : "FAIL 没有可存的图，或写 NVS 失败");
            return;
        }
        Reply("壁纸=%s  常驻=%s  尺寸=%dx%d",
              photo.HasOverride() ? "上传的图" : "内置/SD",
              photo.HasSavedOverride() ? "是（重启保留）" : "否",
              photo.CurrentSizeW(), photo.CurrentSizeH());
        return;
    }

    if (strcmp(cmd, "reboot") == 0) {
        Reply("OK 3 秒后重启");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        return;
    }

    Reply("ERR 不认识的命令: %s（发 help 看用法）", cmd);
}
