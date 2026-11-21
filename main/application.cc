#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <utility>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_ae_rate_cvt.h"

#define TAG "Application"

namespace {
struct MusicPlaybackTaskArgs {
    Application* app = nullptr;
    std::string url;
    std::string title;
    std::string artist;
    std::string lyric_url;      // 歌词 URL（为空则用 lyric_text）
    std::string lyric_text;     // 直接给的 LRC 歌词文本（为空则用 lyric_url 拉取）
    std::string cover;          // 专辑封面图 URL（可为空）
    uint32_t start_ms = 0;      // 从该位置开始播放（快进用），0 = 从头
    uint32_t duration_ms = 0;   // 中转给的歌曲总时长（进度条用），0 = 未知
};

// ===== 本地音频时长探测 =====
//
// 没有 .lrc 歌词时就没有总时长（原来的实现只能用歌词末尾时间戳近似），
// 播放器页的进度条会一直是空的。这里在打开文件时读一下头部算出真实时长：
//   MP3：优先用 Xing/Info 帧里的总帧数（VBR 也准），否则按帧头的码率做 CBR 估算
//   WAV：按 RIFF 头里的采样率/声道/位深算
// 只读文件前几十 KB，调用前后文件指针复位，不影响后续解码。

uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t EstimateWavDurationMs(FILE* f, long file_size) {
    uint8_t hdr[64] = {0};
    rewind(f);
    const size_t got = fread(hdr, 1, sizeof(hdr), f);
    if (got < 44 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return 0;
    }
    // 标准 44 字节头的字段位置；带扩展块的文件这里会偏，但至少能给出量级
    const uint32_t channels = hdr[22] | (hdr[23] << 8);
    const uint32_t rate = Le32(hdr + 24);
    const uint32_t bits = hdr[34] | (hdr[35] << 8);
    if (channels == 0 || rate == 0 || bits == 0) {
        return 0;
    }
    const uint32_t bytes_per_sec = rate * channels * (bits / 8);
    if (bytes_per_sec == 0 || file_size <= 44) {
        return 0;
    }
    return static_cast<uint32_t>(static_cast<uint64_t>(file_size - 44) * 1000 / bytes_per_sec);
}

uint32_t EstimateMp3DurationMs(FILE* f, long file_size) {
    uint8_t hdr[10] = {0};
    rewind(f);
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0) {
        // 没有 ID3 标签，从头开始找帧同步
    }
    long audio_start = 0;
    if (memcmp(hdr, "ID3", 3) == 0) {
        // ID3v2 长度是 synchsafe 整数（每字节只用低 7 位）
        const long id3 = (static_cast<long>(hdr[6] & 0x7f) << 21) |
                         (static_cast<long>(hdr[7] & 0x7f) << 14) |
                         (static_cast<long>(hdr[8] & 0x7f) << 7) |
                         static_cast<long>(hdr[9] & 0x7f);
        audio_start = 10 + id3;
        if ((hdr[5] & 0x10) != 0) {
            audio_start += 10;   // 有 footer
        }
    }
    if (audio_start >= file_size) {
        return 0;
    }

    // 找第一个帧同步（0xFF 0xEx），最多翻 128KB，防止极端文件卡住
    uint8_t buf[4096];
    long pos = audio_start;
    long frame_pos = -1;
    uint8_t fh[4] = {0};
    const long scan_end = audio_start + 128 * 1024;
    const long limit = (file_size < scan_end) ? file_size : scan_end;
    while (pos < limit) {
        if (fseek(f, pos, SEEK_SET) != 0) {
            return 0;
        }
        const size_t chunk = fread(buf, 1, sizeof(buf), f);
        if (chunk < 4) {
            break;
        }
        for (size_t i = 0; i + 4 <= chunk; i++) {
            if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
                frame_pos = pos + static_cast<long>(i);
                memcpy(fh, &buf[i], 4);
                break;
            }
        }
        if (frame_pos >= 0 || chunk < sizeof(buf)) {
            break;
        }
        pos += static_cast<long>(chunk) - 3;
    }
    if (frame_pos < 0) {
        return 0;
    }

    const int ver_bits = (fh[1] >> 3) & 0x03;   // 3=MPEG1, 2=MPEG2, 0=MPEG2.5
    const int layer = (fh[1] >> 1) & 0x03;      // 1=Layer III, 2=Layer II, 3=Layer I
    const int br_idx = (fh[2] >> 4) & 0x0F;
    const int sr_idx = (fh[2] >> 2) & 0x03;
    if (br_idx == 0 || br_idx == 15 || sr_idx == 3 || layer == 0) {
        return 0;
    }

    static const int kBrV1L3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBrV1L1L2[] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
    static const int kBrV2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kSrV1[] = {44100, 48000, 32000, 0};
    static const int kSrV2[] = {22050, 24000, 16000, 0};
    static const int kSrV25[] = {11025, 12000, 8000, 0};

    const int bitrate_kbps = (ver_bits == 3)
                                 ? ((layer == 1) ? kBrV1L3[br_idx] : kBrV1L1L2[br_idx])
                                 : kBrV2[br_idx];
    const int *sr_tab = (ver_bits == 3) ? kSrV1 : ((ver_bits == 2) ? kSrV2 : kSrV25);
    const int sample_rate = sr_tab[sr_idx];
    if (bitrate_kbps <= 0 || sample_rate <= 0) {
        return 0;
    }
    // Layer I 每帧 384 个采样；Layer II 恒 1152；Layer III 在 MPEG1 是 1152、MPEG2/2.5 是 576
    int samples_per_frame = 1152;
    if (layer == 3) {
        samples_per_frame = 384;
    } else if (layer == 1 && ver_bits != 3) {
        samples_per_frame = 576;
    }

    // 先看这一帧里有没有 Xing/Info（VBR 的帧数表就在里面）
    uint8_t xing[48] = {0};
    if (fseek(f, frame_pos + 4, SEEK_SET) == 0) {
        const size_t got = fread(xing, 1, sizeof(xing), f);
        for (size_t i = 0; i + 12 <= got && i < 40; i++) {
            if (memcmp(&xing[i], "Xing", 4) == 0 || memcmp(&xing[i], "Info", 4) == 0) {
                const uint32_t flags = Be32(&xing[i + 4]);
                if ((flags & 0x01) != 0) {
                    const uint32_t frames = Be32(&xing[i + 8]);
                    if (frames > 0) {
                        return static_cast<uint32_t>(
                            static_cast<uint64_t>(frames) * samples_per_frame * 1000 / sample_rate);
                    }
                }
                break;
            }
        }
    }

    // 退化：CBR 按码率估算（帧头之后都是音频数据）
    return static_cast<uint32_t>(static_cast<uint64_t>(file_size - frame_pos) * 8 / bitrate_kbps);
}

// 返回 0 表示没算出来（网络流 / 未知格式）
uint32_t EstimateLocalAudioDurationMs(FILE* f) {
    if (f == nullptr) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        rewind(f);
        return 0;
    }
    const long file_size = ftell(f);
    rewind(f);
    if (file_size <= 0) {
        rewind(f);
        return 0;
    }

    uint8_t magic[12] = {0};
    const size_t got = fread(magic, 1, sizeof(magic), f);
    rewind(f);

    uint32_t ms = 0;
    if (got >= 12 && memcmp(magic, "RIFF", 4) == 0 && memcmp(magic + 8, "WAVE", 4) == 0) {
        ms = EstimateWavDurationMs(f, file_size);
    } else {
        ms = EstimateMp3DurationMs(f, file_size);
    }
    rewind(f);
    return ms;
}

}  // namespace


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();

    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Play the success sound to indicate the device is ready
    audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        // 如果正在播放音乐（HTTP 流），不要切回省电模式，
        // 否则会覆盖 PlayMusicFromUrl 里设的高性能模式导致卡顿
        if (!music_playing_.load()) {
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        }
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    // 用户开始交互时优先暂停本地音乐播放：
    // 暂停而不是停止，是为了保住播放位置——用户接下来很可能说"下一首""快进30秒"，
    // 这些工具需要一个还在的播放任务才能算出目标位置。
    // 同时必须把外部播放标志清掉，否则麦克风会被省电逻辑一直关着。
    if (IsMusicPlaying()) {
        PauseMusicPlayback();
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    // 用户按下开始监听时，如果正在播歌，先暂停（保留进度，供"下一首/快进"使用）
    if (IsMusicPlaying()) {
        PauseMusicPlayback();
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (IsMusicPlaying()) {
        StopMusicPlayback();
    }

    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // 本地音乐播放期间保持歌词/曲目信息，避免被空闲态重绘清掉。
            if (IsMusicPlaying()) {
                display->SetStatus("音乐播放");
            } else {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
                display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }

            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    StopMusicPlayback();

    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

bool Application::PlayMusicFromUrl(const std::string& url, const std::string& title, const std::string& artist,
                                   const std::string& lyric, const std::string& lyric_url,
                                   uint32_t start_ms, bool force, uint32_t duration_ms,
                                   const std::string& cover) {
    if (url.empty()) {
        return false;
    }

    // 防重播保护：同一 URL 在播放结束后 15 秒内不允许重复启动
    // 防止大模型"播放结束 -> 回答用户 -> 又调一次 play_url"的死循环
    // force=true 用于"快进/上一首/下一首"这类用户主动操作，必须放行
    if (!force) {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        if (!last_played_url_.empty() && last_played_url_ == url && last_play_finished_ms_ > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t elapsed = now_ms - last_play_finished_ms_;
            if (elapsed < 15000) {
                ESP_LOGW(TAG, "防重播：同一 URL 在 %lld ms 前刚播完，跳过", elapsed);
                return false;
            }
        }
    }

    // 先停止旧播放，避免多个任务同时争用音频输出。
    StopMusicPlayback(false);

    // 音乐播放前先退出当前语音会话，避免服务端音频与本地音乐混播。
    if (GetDeviceState() == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    SetDeviceState(kDeviceStateIdle);
    audio_service_.ResetDecoder();
    audio_service_.SetExternalPlaybackActive(true);

    // 音乐播放走 HTTP 流，不经过 AudioChannel，WiFi 不会自动切高性能
    // 这里手动关闭省电，避免 MAX_MODEM 休眠导致下载卡顿
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        stop_music_playback_.store(false);
        music_paused_.store(false);
        music_playing_.store(true);
        music_progress_ms_.store(start_ms);
        music_total_ms_.store(0);
        current_music_title_ = title;
        current_music_artist_ = artist;
        current_music_url_ = url;
    }

    auto display_title = title.empty() ? "未知歌曲" : title;
    auto display_artist = artist.empty() ? "未知歌手" : artist;
    Schedule([display_title, display_artist]() {
        auto display = Board::GetInstance().GetDisplay();
        // 触发音乐播放时，优先切到音乐页，避免歌词继续显示在首页 AI 卡片
        display->SwitchToMusicPage();
        display->SetStatus("音乐播放");
        display->SetChatMessage("system", "音乐播放中~");
        display->SetMusicInfo(display_title.c_str(), display_artist.c_str());
        display->SetMusicProgress(0, 1);
        display->SetMusicLyric("");
        display->SetMusicPlayState(true, false);
    });
    // 歌词不再在这里直接上屏：原始 LRC 带时间戳，直接显示会把
    // "[00:29.34]故事的小黄..." 这种原始行塞进歌词区。统一交给播放任务
    // 解析成"按时间戳滚动"的结构后再显示。

    auto* args = new MusicPlaybackTaskArgs();
    args->app = this;
    args->url = url;
    args->title = display_title;
    args->artist = display_artist;
    // 歌词两个来源二选一：直接文本优先，都没有就交给任务自己按 lyric_url 拉取
    args->lyric_url = lyric.empty() ? lyric_url : "";
    args->lyric_text = lyric;
    args->cover = cover;
    args->start_ms = start_ms;
    args->duration_ms = duration_ms;

    // 播放任务的栈（12KB）放 PSRAM。
    //
    // 曾经挪到内部 RAM 想救音乐卡顿，结果内部 RAM 被挤到只剩 17KB
    // （日志 minimal sram），AFE 语音前端拿不到连续内存，
    // **放歌时就唤不醒 AI 了**——语音是核心功能，优先级高于那点卡顿。
    // PSRAM 80MHz 下栈访问的额外延迟对整体影响很小（顺序访问，且有 cache），
    // 真正影响音质的是 I2S DMA 缓冲深度，那个已经加厚了。
    BaseType_t created = xTaskCreateWithCaps([](void* arg) {
        std::unique_ptr<MusicPlaybackTaskArgs> task_args(static_cast<MusicPlaybackTaskArgs*>(arg));
        task_args->app->MusicPlaybackTask(std::move(task_args->url), std::move(task_args->title),
                                          std::move(task_args->artist), std::move(task_args->lyric_url),
                                          std::move(task_args->lyric_text), task_args->start_ms,
                                          task_args->duration_ms, std::move(task_args->cover));
        // 与 xTaskCreateWithCaps 配对，否则栈不会释放
        vTaskDeleteWithCaps(nullptr);
    }, "music_playback", 12288, args, 3, &music_playback_task_handle_, MALLOC_CAP_SPIRAM);

    if (created != pdPASS) {
        // 内部 RAM 不够（AFE 占了大头）：退回 PSRAM，宁可慢也不能放不了歌
        ESP_LOGW(TAG, "音乐播放任务内部 RAM 不足，回退到 PSRAM 栈");
        created = xTaskCreateWithCaps([](void* arg) {
            std::unique_ptr<MusicPlaybackTaskArgs> task_args(static_cast<MusicPlaybackTaskArgs*>(arg));
            task_args->app->MusicPlaybackTask(std::move(task_args->url), std::move(task_args->title),
                                              std::move(task_args->artist), std::move(task_args->lyric_url),
                                              std::move(task_args->lyric_text), task_args->start_ms,
                                              task_args->duration_ms, std::move(task_args->cover));
            vTaskDeleteWithCaps(nullptr);
        }, "music_playback", 12288, args, 3, &music_playback_task_handle_, MALLOC_CAP_SPIRAM);
    }

    if (created != pdPASS) {
        delete args;
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        music_playing_.store(false);
        music_playback_task_handle_ = nullptr;
        current_music_title_.clear();
        return false;
    }

    return true;
}

void Application::MusicPlaybackTask(std::string url, std::string title, std::string artist,
                                    std::string lyric_url, std::string lyric_text,
                                    uint32_t start_ms, uint32_t duration_ms,
                                    std::string cover) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();
    auto network = board.GetNetwork();
    ESP_LOGI(TAG, "开始播放音乐: %s - %s", title.c_str(), artist.c_str());

    // ===== 本地文件支持（SD 卡音乐）=====
    // url 允许两种写法：/sdcard/music/xxx.mp3 或 file:///sdcard/music/xxx.mp3
    // 支持同名 .lrc 歌词文件（自动探测），无需联网
    std::string local_path;
    if (url.rfind("file://", 0) == 0) {
        local_path = url.substr(7);
    } else if (url.rfind("/sdcard/", 0) == 0) {
        local_path = url;
    }
    const bool is_local = !local_path.empty();
    FILE* local_file = nullptr;
    if (is_local) {
        ESP_LOGI(TAG, "本地音乐文件: %s", local_path.c_str());
    }

    // LRC 歌词结构：(时间戳毫秒, 纯文本)
    struct LyricLine {
        uint32_t time_ms;
        std::string text;
    };
    std::vector<LyricLine> lyrics;
    size_t current_lyric_idx = 0;
    size_t last_displayed_idx = SIZE_MAX;  // 避免重复刷新
    uint32_t lyric_total_ms = 0;
    // 真实总时长：本地文件可从头部算出来，网络流只能靠歌词时间戳兜底
    uint32_t measured_total_ms = 0;
    uint32_t track_total_ms = 0;

    // 本地文件：未显式提供歌词时，自动找同目录同名 .lrc
    if (is_local && lyric_url.empty() && lyric_text.empty()) {
        std::string guess = local_path;
        size_t dot = guess.find_last_of('.');
        if (dot != std::string::npos) {
            guess = guess.substr(0, dot);
        }
        guess += ".lrc";
        FILE* lrc_probe = fopen(guess.c_str(), "r");
        if (lrc_probe != nullptr) {
            fclose(lrc_probe);
            lyric_url = guess;
            ESP_LOGI(TAG, "发现同名歌词文件: %s", guess.c_str());
        }
    }

    // 歌词两个来源：直接文本（中转给的原始 LRC）优先，否则按 lyric_url 拉取。
    // 统一落到 local_lyric_text，再用同一个解析器解析，避免两条路径行为不一致。
    std::string local_lyric_text;
    if (!lyric_text.empty()) {
        local_lyric_text = std::move(lyric_text);
        ESP_LOGI(TAG, "使用直接歌词文本，长度=%d bytes", (int)local_lyric_text.size());
    } else if (!lyric_url.empty()) {
        ESP_LOGI(TAG, "正在拉取歌词: %s", lyric_url.c_str());

        if (lyric_url.rfind("/sdcard/", 0) == 0) {
            // 本地歌词文件：直接读盘
            FILE* lrc_file = fopen(lyric_url.c_str(), "r");
            if (lrc_file != nullptr) {
                char chunk[512];
                size_t got = 0;
                while ((got = fread(chunk, 1, sizeof(chunk), lrc_file)) > 0) {
                    local_lyric_text.append(chunk, got);
                }
                fclose(lrc_file);
            }
        } else {
            auto lyric_http = network->CreateHttp(4);  // 用不同的 connect_id，避免和音乐流(3)的 TLS 连接冲突
            lyric_http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
            if (lyric_http->Open("GET", lyric_url)) {
                if (lyric_http->GetStatusCode() == 200) {
                    local_lyric_text = lyric_http->ReadAll();
                } else {
                    ESP_LOGW(TAG, "歌词拉取失败，HTTP 状态码: %d", lyric_http->GetStatusCode());
                }
                lyric_http->Close();
            } else {
                ESP_LOGW(TAG, "歌词拉取失败：无法连接到歌词服务");
            }
        }
    }

    if (!local_lyric_text.empty()) {
        // 解析 LRC 歌词：逐行提取 [mm:ss.xx] / 【mm:ss.xx】时间戳和纯文本。
        // 兼容全角【】：有些歌词源（尤其翻译歌词）用全角方括号，只认半角会把
        // "【00:29.34】故事的小黄..." 整个当正文显示出来。
        size_t pos = 0;
        while (pos < local_lyric_text.size()) {
            size_t line_end = local_lyric_text.find('\n', pos);
            if (line_end == std::string::npos) line_end = local_lyric_text.size();
            std::string line = local_lyric_text.substr(pos, line_end - pos);
            pos = line_end + 1;

            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::string pure_text;
            std::vector<uint32_t> timestamps;
            size_t i = 0;
            // 逐段吃掉时间标签：支持半角 [ 与全角 【（以及对应的 ] / 】）
            while (i < line.size()) {
                const unsigned char open = line[i];
                if (open != '[' && open != 0xE3) break;   // 0xE3 是全角【 的首字节（UTF-8 0xE3 0x80 0x90）

                size_t close_pos = std::string::npos;
                if (open == '[') {
                    close_pos = line.find(']', i);
                } else {
                    // 全角【：找 】（UTF-8 0xE3 0x80 0x91）
                    close_pos = line.find("\xE3\x80\x91", i);
                }
                if (close_pos == std::string::npos) break;

                size_t tag_start = (open == '[') ? i + 1 : i + 3;   // 跳过 【 的 3 字节
                std::string tag = line.substr(tag_start, close_pos - tag_start);
                i = (open == '[') ? close_pos + 1 : close_pos + 3;  // 跳过 】 的 3 字节

                // 尝试解析时间 mm:ss.xx
                int mm = 0, ss = 0, ms = 0;
                if (sscanf(tag.c_str(), "%d:%d.%d", &mm, &ss, &ms) >= 2) {
                    // 小数点后的数字要按"位数"换算成毫秒，而不是按数值大小：
                    //   [00:29.3]   → 1 位 = 十分之一秒 → 300ms
                    //   [00:29.34]  → 2 位 = 百分之一秒 → 340ms
                    //   [00:29.345] → 3 位 = 毫秒       → 345ms
                    //
                    // 以前写的是 "ms < 10 才换算"，于是 .34 被当成 34ms，
                    // 每句歌词时间戳少了约 300ms，和歌声对不上（歌词偏早）。
                    const size_t dot = tag.find('.');
                    if (dot != std::string::npos && dot + 1 < tag.size()) {
                        const size_t ms_digits = tag.size() - dot - 1;
                        if (ms_digits == 1) {
                            ms *= 100;
                        } else if (ms_digits == 2) {
                            ms *= 10;
                        }
                        // 3 位及以上：sscanf 出来的已经是毫秒，不用动
                    }
                    timestamps.push_back(mm * 60000 + ss * 1000 + ms);
                }
                // 非时间标签（[ti:标题]）跳过，继续看后面还有没有时间戳
            }
            pure_text = line.substr(i);

            if (pure_text.empty()) continue;

            // 去掉正文里可能残留的全角/半角空白
            for (uint32_t ts : timestamps) {
                lyrics.push_back({ts, pure_text});
            }
        }

        std::sort(lyrics.begin(), lyrics.end(),
                  [](const LyricLine& a, const LyricLine& b) { return a.time_ms < b.time_ms; });
        if (!lyrics.empty()) {
            lyric_total_ms = std::max<uint32_t>(lyrics.back().time_ms, 1);
        }
        ESP_LOGI(TAG, "LRC 歌词解析完成，共 %d 行有效歌词", (int)lyrics.size());
    }

    // ===== 专辑封面：已放弃（改成唱片中心显示"小智"标识）=====
    // 网易云封面图 100KB+、原图 1000×1000，下载 + RGB888 解码要占大量 PSRAM，
    // 和音频流抢内存，解码失败还会静默无显示。改成内置"小智"标签后，
    // 这里不再拉图，省掉网络请求和解码开销，也让播放更稳。
    (void)cover;

    constexpr int kReadBufSize = 8192;
    int out_buf_size = 8192;
    bool decoder_registered = false;
    bool playback_finished = false;
    bool playback_failed = false;
    bool notify_failed = false;
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_ae_rate_cvt_handle_t music_resampler = nullptr;
    uint8_t* in_buf = nullptr;
    uint8_t* out_buf = nullptr;
    int16_t* pcm_work_buf = nullptr;   // 热循环复用：声道转换/重采样工作区（内部 RAM）
    bool info_ready = false;
    int stream_sample_rate = codec->output_sample_rate();
    int stream_channels = codec->output_channels();
    // 快进：把目标位置换算成需要解码丢弃的采样数。
    // 不做 fseek（MP3 变码率按字节跳不准），直接解码丢弃，简单且音画同步。
    const uint64_t skip_samples = start_ms == 0 ? 0 :
        static_cast<uint64_t>(start_ms) * codec->output_sample_rate() * codec->output_channels() / 1000;
    int zero_read_count = 0;
    constexpr int kMaxZeroReads = 30;  // 约 3 秒，避免网络抖动导致误判 EOF。
    size_t total_read_bytes = 0;
    size_t total_output_samples = 0;

    auto http = network->CreateHttp(3);
    // 音频流连接设 12 秒超时：网易云 m701/m801 旧服务器偶发慢，默认超时太久会让
    // 用户干等；12 秒内连不上就走下面的重试，而不是一直卡着。
    http->SetTimeout(12000);
    do {
        if (is_local) {
            local_file = fopen(local_path.c_str(), "rb");
            if (local_file == nullptr) {
                ESP_LOGE(TAG, "播放本地音乐失败：无法打开 %s", local_path.c_str());
                playback_failed = true;
                Schedule([display]() {
                    display->SetMusicLyric("播放失败：文件不存在或无法读取");
                    display->SetMusicProgress(0, 1);
                    display->ShowNotification("音乐播放失败：找不到文件");
                });
                notify_failed = true;
                break;
            }
            // 顺便算出真实时长，让没有 .lrc 的歌也有完整进度条
            measured_total_ms = EstimateLocalAudioDurationMs(local_file);
            if (measured_total_ms > 0) {
                ESP_LOGI(TAG, "本地音频时长: %u ms", static_cast<unsigned>(measured_total_ms));
            }
        } else {
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent());

        // 音频流连接重试：网易云旧服务器偶发握手慢/超时，连不上就重试两次，
        // 每次重新 Open（CreateHttp 的连接可能已坏，重试前重建一次）。
        bool opened = false;
        for (int attempt = 0; attempt < 3 && !opened; ++attempt) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "音频流连接失败，第 %d 次重试...", attempt);
                vTaskDelay(pdMS_TO_TICKS(500));
                http = network->CreateHttp(3);
                http->SetTimeout(12000);
                http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
            }
            opened = http->Open("GET", url);
        }

        if (!opened) {
            ESP_LOGE(TAG, "播放音乐失败：无法打开 URL: %s", url.c_str());
            playback_failed = true;
            Schedule([display]() {
                // 音乐页可见时，必须把失败信息写到歌词区，避免用户看不到弹窗。
                display->SetMusicLyric("播放失败：链接无效或网络异常");
                display->SetMusicProgress(0, 1);
                display->ShowNotification("音乐播放失败：网络连接异常");
            });
            notify_failed = true;
            break;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGE(TAG, "播放音乐失败：HTTP 状态码 %d", http->GetStatusCode());
            playback_failed = true;
            Schedule([display]() {
                // 资源不可用时，同步更新音乐页提示，避免只在通知区显示。
                display->SetMusicLyric("播放失败：资源不可用");
                display->SetMusicProgress(0, 1);
                display->ShowNotification("音乐播放失败：资源不可用");
            });
            notify_failed = true;
            break;
        }
        }  // end of (!is_local) network branch

        ret = esp_audio_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册默认解码器失败: %d", ret);
            playback_failed = true;
            break;
        }
        ret = esp_audio_simple_dec_register_default();
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "注册简单解码器失败: %d", ret);
            playback_failed = true;
            break;
        }
        decoder_registered = true;

        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = nullptr,
            .cfg_size = 0,
            .use_frame_dec = false,
        };
        ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
        if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
            ESP_LOGE(TAG, "打开 MP3 解码器失败: %d", ret);
            playback_failed = true;
            break;
        }

        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }

        // 压缩流读缓冲：放 PSRAM。
        // 曾经挪到内部 RAM 想减少卡顿，但 8KB 内部 RAM 挤掉了 AFE 的余量，
        // 导致放歌时唤不醒 AI。PSRAM 80MHz 顺序读 8KB 只要约 100µs，
        // 对音频实时性影响远小于"语音功能失效"。
        in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_SPIRAM));
        if (in_buf == nullptr) {
            ESP_LOGW(TAG, "音频读缓冲 PSRAM 分配失败，回退内部 RAM");
            in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadBufSize, MALLOC_CAP_8BIT));
        }
        // 解码后的 PCM 缓冲：它要直接喂给 I2S DMA，留在内部 RAM。
        // 这不是偷懒——PSRAM 走 cache，一旦和 LVGL 刷新抢带宽就可能欠载爆音，
        // 换来的是 8KB 内部 RAM，不值得。
        out_buf = static_cast<uint8_t*>(heap_caps_malloc(out_buf_size, MALLOC_CAP_8BIT));
        // PCM 工作缓冲：热循环里声道转换/重采样都要用，预分配好反复复用，
        // 避免每解码一块就 new/delete 一个 vector 造成堆抖动 + 卡顿。
        //
        // 大小只需要装下"最大单声道采样数"：
        //   out_buf 8KB = 4096 个 int16 采样；双声道→单声道后是 2048，
        //   升采样（22050→24000）最多到 ~2233，8KB（4096 采样）足够。
        // 以前按 out_buf_size * 2 = 16KB 分配，白白多占 8KB 内部 RAM，
        // 而这块内存正是 AFE 做唤醒词检测要用的。
        pcm_work_buf = static_cast<int16_t*>(heap_caps_malloc(
            static_cast<size_t>(out_buf_size), MALLOC_CAP_8BIT));
        if (in_buf == nullptr || out_buf == nullptr || pcm_work_buf == nullptr) {
            ESP_LOGE(TAG, "内存不足，无法播放音乐");
            playback_failed = true;
            break;
        }

        while (!stop_music_playback_.load()) {
            int read_bytes = is_local
                ? static_cast<int>(fread(in_buf, 1, kReadBufSize, local_file))
                : http->Read(reinterpret_cast<char*>(in_buf), kReadBufSize);
            if (is_local && read_bytes == 0) {
                ESP_LOGI(TAG, "本地音乐文件读取结束");
                playback_finished = true;
                break;
            }
            if (read_bytes < 0) {
                ESP_LOGE(TAG, "读取音乐流失败: %d", read_bytes);
                playback_failed = true;
                break;
            }
            if (read_bytes == 0) {
                zero_read_count++;
                if (zero_read_count >= kMaxZeroReads) {
                    ESP_LOGI(TAG, "音乐流读取结束（连续空读 %d 次）", zero_read_count);
                    playback_finished = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            zero_read_count = 0;
            total_read_bytes += static_cast<size_t>(read_bytes);

            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf,
                .len = static_cast<uint32_t>(read_bytes),
                // 本地文件：EOF 由 fread 返回 0 判断，这里不能因为"短读"就当成流结束
                .eos = is_local ? false : (read_bytes < kReadBufSize),
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };

            while (raw.len > 0 && !stop_music_playback_.load()) {
                // 暂停：阻塞在这里，不读新数据也不出声。
                // 用 50ms 轮询而不是信号量，是因为暂停标志同时也可能被 stop 打断，
                // 两条退出路径都检查一次最省事。
                while (music_paused_.load() && !stop_music_playback_.load()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (stop_music_playback_.load()) {
                    break;
                }

                esp_audio_simple_dec_out_t out = {
                    .buffer = out_buf,
                    .len = static_cast<uint32_t>(out_buf_size),
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                ret = esp_audio_simple_dec_process(decoder, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    auto* new_buf = static_cast<uint8_t*>(heap_caps_realloc(out_buf, out.needed_size, MALLOC_CAP_8BIT));
                    if (new_buf == nullptr) {
                        ESP_LOGE(TAG, "扩容解码输出缓冲区失败");
                        playback_failed = true;
                        break;
                    }
                    out_buf = new_buf;
                    out_buf_size = static_cast<int>(out.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "MP3 解码失败，ret=%d", ret);
                    playback_failed = true;
                    break;
                }

                if (!info_ready && out.decoded_size > 0) {
                    esp_audio_simple_dec_info_t dec_info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK) {
                        stream_sample_rate = static_cast<int>(dec_info.sample_rate);
                        stream_channels = std::max(1, static_cast<int>(dec_info.channel));
                        info_ready = true;
                        ESP_LOGI(TAG, "MP3 解码信息: sample_rate=%d channel=%d", stream_sample_rate, stream_channels);
                    }
                }

                if (out.decoded_size > 0) {
                    // 热循环里不再每块 new vector（原来这里 new 一次 + memcpy 一次，
                    // 声道/重采样又各 new 一次，是放歌卡顿的主因之一）。
                    // 统一复用 pcm_work_buf，直接在原地做声道转换和重采样。
                    size_t pcm_samples = out.decoded_size / sizeof(int16_t);
                    int16_t* pcm_ptr = reinterpret_cast<int16_t*>(out.buffer);

                    // 先做声道转换，统一到 codec 输出声道数。
                    if (stream_channels == 2 && codec->output_channels() == 1) {
                        // 双声道 → 单声道：两两平均，写进工作缓冲
                        const size_t mono_samples = pcm_samples / 2;
                        for (size_t i = 0, j = 0; i < mono_samples; ++i, j += 2) {
                            pcm_work_buf[i] = static_cast<int16_t>(
                                (static_cast<int32_t>(pcm_ptr[j]) + static_cast<int32_t>(pcm_ptr[j + 1])) / 2);
                        }
                        pcm_ptr = pcm_work_buf;
                        pcm_samples = mono_samples;
                    } else if (stream_channels == 1 && codec->output_channels() == 2) {
                        // 单声道 → 双声道：每个采样复制一份
                        const size_t stereo_samples = pcm_samples * 2;
                        for (size_t i = 0; i < pcm_samples; ++i) {
                            pcm_work_buf[i * 2] = pcm_ptr[i];
                            pcm_work_buf[i * 2 + 1] = pcm_ptr[i];
                        }
                        pcm_ptr = pcm_work_buf;
                        pcm_samples = stereo_samples;
                    }

                    // 采样率不一致时重采样，避免"变速/变调"问题。
                    if (stream_sample_rate != codec->output_sample_rate()) {
                        if (music_resampler == nullptr) {
                            esp_ae_rate_cvt_cfg_t cfg = {
                                .src_rate = static_cast<uint32_t>(stream_sample_rate),
                                .dest_rate = static_cast<uint32_t>(codec->output_sample_rate()),
                                .channel = static_cast<uint8_t>(codec->output_channels()),
                                .bits_per_sample = ESP_AUDIO_BIT16,
                                .complexity = 2,
                                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                            };
                            auto cvt_ret = esp_ae_rate_cvt_open(&cfg, &music_resampler);
                            if (music_resampler == nullptr) {
                                ESP_LOGE(TAG, "创建重采样器失败: %d", cvt_ret);
                                playback_failed = true;
                                break;
                            }
                        }
                        const uint32_t in_samples = static_cast<uint32_t>(pcm_samples / codec->output_channels());
                        uint32_t out_samples = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(music_resampler, in_samples, &out_samples);

                        // 容量保护：pcm_work_buf 按 out_buf_size 字节分配，
                        // 每声道样本数 = 字节数 / 2。升采样（如 8kHz→24kHz 三倍）
                        // 时 out_samples 可能超出容量，必须截断，否则写越界会踩坏堆。
                        // 正常的 44.1k/48k → 24k 都是降采样，不会触发。
                        const uint32_t work_capacity =
                            static_cast<uint32_t>(out_buf_size) / static_cast<uint32_t>(sizeof(int16_t));
                        uint32_t actual_out = out_samples;
                        if (actual_out > work_capacity) {
                            ESP_LOGW(TAG, "重采样输出 %u 超过工作缓冲 %u，截断（音质略降）",
                                     static_cast<unsigned>(actual_out),
                                     static_cast<unsigned>(work_capacity));
                            actual_out = work_capacity;
                        }
                        esp_ae_rate_cvt_process(
                            music_resampler,
                            reinterpret_cast<esp_ae_sample_t>(pcm_ptr),
                            in_samples,
                            reinterpret_cast<esp_ae_sample_t>(pcm_work_buf),
                            &actual_out);
                        pcm_ptr = pcm_work_buf;
                        pcm_samples = static_cast<size_t>(actual_out) * codec->output_channels();
                    }

                    if (pcm_samples > 0) {
                        total_output_samples += pcm_samples;
                        // 快进：目标位置之前的采样只解码、不出声，也不刷进度
                        if (total_output_samples > skip_samples) {
                            // 音乐播放绕过了 AudioService，需要手动保持功放开启。
                            if (!codec->output_enabled()) {
                                codec->EnableOutput(true);
                            }
                            // 裸指针版 OutputData：复用工作缓冲，省掉 vector 包装 + memcpy
                            codec->OutputData(pcm_ptr, static_cast<int>(pcm_samples));

                            // 计算当前播放位置（无论有没有歌词都需要）
                            uint32_t current_ms = static_cast<uint32_t>(
                                static_cast<uint64_t>(total_output_samples) * 1000
                                / (codec->output_sample_rate() * codec->output_channels()));

                            // 同步更新原子变量，供 MCP 工具查询当前进度
                            music_progress_ms_.store(current_ms);
                            // 时长优先级：中转给的 duration_ms > 本地文件头算的 > 歌词末尾时间戳
                            if (duration_ms > 0) {
                                track_total_ms = duration_ms;
                            } else {
                                track_total_ms = (measured_total_ms > 0) ? measured_total_ms : lyric_total_ms;
                            }
                            if (track_total_ms > 0) {
                                music_total_ms_.store(track_total_ms);
                            }

                            // 更新进度条（无论有没有歌词都要更新）
                            // 两者都没有时传 0，让页面显示 "--:--" 且不画进度
                            display->SetMusicProgress(current_ms, track_total_ms);

                            // 有歌词时同步歌词显示
                            if (!lyrics.empty()) {
                                // 向前搜索到当前时间对应的歌词行
                                while (current_lyric_idx + 1 < lyrics.size()
                                       && lyrics[current_lyric_idx + 1].time_ms <= current_ms) {
                                    current_lyric_idx++;
                                }
                                // 歌词行变化时更新屏幕显示（传四行：上一句\n当前句\n下一句\n再下一句）
                                if (current_lyric_idx != last_displayed_idx) {
                                    last_displayed_idx = current_lyric_idx;
                                    const size_t last = lyrics.size() - 1;
                                    const std::string prev_line = (current_lyric_idx > 0) ? lyrics[current_lyric_idx - 1].text : "";
                                    const std::string curr_line = lyrics[current_lyric_idx].text;
                                    const std::string next_line = (current_lyric_idx + 1 <= last) ? lyrics[current_lyric_idx + 1].text : "";
                                    const std::string next2_line = (current_lyric_idx + 2 <= last) ? lyrics[current_lyric_idx + 2].text : "";
                                    UpdateMusicLyric(prev_line + "\n" + curr_line + "\n" + next_line + "\n" + next2_line);
                                }
                            }
                        }
                    }
                }

                raw.len -= raw.consumed;
                raw.buffer += raw.consumed;
            }

            if (playback_failed) {
                break;
            }
        }
    } while (false);

    if (music_resampler != nullptr) {
        esp_ae_rate_cvt_close(music_resampler);
    }
    if (in_buf != nullptr) {
        heap_caps_free(in_buf);
    }
    if (out_buf != nullptr) {
        heap_caps_free(out_buf);
    }
    if (pcm_work_buf != nullptr) {
        heap_caps_free(pcm_work_buf);
    }
    if (decoder != nullptr) {
        esp_audio_simple_dec_close(decoder);
    }
    if (decoder_registered) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
    }
    if (local_file != nullptr) {
        fclose(local_file);
        local_file = nullptr;
    }
    if (http) {
        http->Close();
    }
    audio_service_.SetExternalPlaybackActive(false);

    bool stopped_by_user = stop_music_playback_.load();
    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        music_playing_.store(false);
        music_paused_.store(false);
        stop_music_playback_.store(false);
        music_playback_task_handle_ = nullptr;
        // 记录刚播完的 URL 和时间戳，用于防重播保护
        last_played_url_ = url;
        last_play_finished_ms_ = esp_timer_get_time() / 1000;

        current_music_title_.clear();
    }

    // 播放结束，恢复 WiFi 省电模式
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (stopped_by_user) {
        ESP_LOGI(TAG, "音乐播放已被用户中断，累计读取=%u bytes, 输出=%u samples",
                 static_cast<unsigned>(total_read_bytes),
                 static_cast<unsigned>(total_output_samples));
        return;
    }
    if (playback_failed && !notify_failed) {
        ESP_LOGW(TAG, "音乐播放失败，累计读取=%u bytes, 输出=%u samples",
                 static_cast<unsigned>(total_read_bytes),
                 static_cast<unsigned>(total_output_samples));
        Schedule([display]() {
            display->SetMusicLyric("播放失败：请稍后重试");
            display->SetMusicProgress(0, 1);
            display->SetMusicPlayState(false, false);
            display->ShowNotification("音乐播放失败");
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus(Lang::Strings::STANDBY);
            }
        });
        return;
    }
    if (playback_finished) {
        ESP_LOGI(TAG, "音乐播放结束，累计读取=%u bytes, 输出=%u samples",
                 static_cast<unsigned>(total_read_bytes),
                 static_cast<unsigned>(total_output_samples));
        Schedule([display]() {
            // 清除歌词/聊天消息区域，恢复待命状态
            display->SetMusicLyric("");
            display->SetMusicProgress(0, 1);
            display->SetMusicPlayState(false, false);
            // 播放结束后主动重置 AI 文案，避免停留在上一条工具/提示文本
            display->SetChatMessage("system", "AI 待命");
            display->ShowNotification("音乐播放结束");
            // 自动返回天气首页，避免停留在空白音乐页
            display->SwitchToWeatherPage();
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus(Lang::Strings::STANDBY);
            }
        });
    }
}

void Application::StopMusicPlayback(bool clear_lyric) {
    TaskHandle_t handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        handle = music_playback_task_handle_;
        if (handle == nullptr) {
            music_playing_.store(false);
            current_music_title_.clear();
        } else {
            stop_music_playback_.store(true);
        }
    }

    // 等待播放任务主动退出，避免直接删任务导致解码器/HTTP 资源泄漏。
    while (handle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(20));
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        handle = music_playback_task_handle_;
    }

    if (clear_lyric) {
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetMusicLyric("");
            display->SetMusicProgress(0, 1);
            display->SetMusicPlayState(false, false);
            if (GetDeviceState() == kDeviceStateIdle) {
                display->SetStatus(Lang::Strings::STANDBY);
            }
        });
    }
}

bool Application::PauseMusicPlayback() {
    if (!music_playing_.load() || music_playback_task_handle_ == nullptr) {
        return false;
    }
    if (music_paused_.load()) {
        return true;
    }
    music_paused_.store(true);

    // 关键：把音频通道交还给语音链路。
    // external_playback_active_ 为 true 时 AudioService 会一直关着麦克风省电，
    // 不放开的话用户按了按钮也听不到自己说话。
    audio_service_.SetExternalPlaybackActive(false);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetMusicPlayState(true, true);
    });
    ESP_LOGI(TAG, "音乐已暂停（位置 %u ms）", static_cast<unsigned>(music_progress_ms_.load()));
    return true;
}

bool Application::ResumeMusicPlayback() {
    if (!music_playing_.load() || music_playback_task_handle_ == nullptr) {
        return false;
    }
    if (!music_paused_.load()) {
        return true;
    }

    // 先恢复外部播放标志再解除暂停，避免音频省电定时器把输出关掉
    audio_service_.SetExternalPlaybackActive(true);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    music_paused_.store(false);

    Schedule([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->SetMusicPlayState(true, false);
        display->SwitchToMusicPage();
    });
    ESP_LOGI(TAG, "音乐已继续");
    return true;
}

bool Application::SeekMusic(int delta_seconds, int* new_pos_ms) {
    std::string url;
    std::string title;
    std::string artist;
    {
        std::lock_guard<std::mutex> lock(music_playback_mutex_);
        if (!music_playing_.load() || music_playback_task_handle_ == nullptr) {
            return false;
        }
        url = current_music_url_;
        title = current_music_title_;
        artist = current_music_artist_;
    }

    // 网络流不能跳：跳转意味着从头重新下载解码，体验比不支持还差
    const bool is_local = (url.rfind("/sdcard/", 0) == 0) || (url.rfind("file://", 0) == 0);
    if (!is_local) {
        ESP_LOGW(TAG, "非本地文件不支持快进: %s", url.c_str());
        return false;
    }

    const int64_t cur_ms = music_progress_ms_.load();
    const int64_t total_ms = music_total_ms_.load();
    int64_t target = cur_ms + static_cast<int64_t>(delta_seconds) * 1000;
    if (target < 0) {
        target = 0;
    }
    // 留 1 秒余量，避免正好跳到结尾触发"播放结束"
    if (total_ms > 2000 && target > total_ms - 1000) {
        target = total_ms - 1000;
    }
    if (new_pos_ms != nullptr) {
        *new_pos_ms = static_cast<int>(target);
    }

    ESP_LOGI(TAG, "快进: %lld ms -> %lld ms", static_cast<long long>(cur_ms), static_cast<long long>(target));
    // force=true 绕过防重播保护；start_ms 让播放任务解码丢弃到目标位置
    return PlayMusicFromUrl(url, title, artist, "", "", static_cast<uint32_t>(target), true);
}

void Application::UpdateMusicLyric(const std::string& lyric) {
    if (!IsMusicPlaying()) {
        return;
    }
    Schedule([lyric]() {
        auto display = Board::GetInstance().GetDisplay();
        // 歌词刷新时确保音乐页可见，避免内容落在首页 AI 区域
        display->SwitchToMusicPage();
        display->SetMusicLyric(lyric.c_str());
    });
}

void Application::ResetProtocol() {
    StopMusicPlayback();

    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

