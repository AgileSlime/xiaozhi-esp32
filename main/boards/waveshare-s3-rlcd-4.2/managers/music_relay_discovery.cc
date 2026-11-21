#include "music_relay_discovery.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "music_stream.h"
#include "wifi_manager.h"

static const char* TAG = "RelayFind";

// 电脑端 relay.py 的广播端口
static constexpr uint16_t kBroadcastPort = 8801;
static constexpr const char* kServiceTag = "xiaozhi-music-relay";

MusicRelayDiscovery& MusicRelayDiscovery::GetInstance() {
    static MusicRelayDiscovery instance;
    return instance;
}

void MusicRelayDiscovery::Start() {
    if (task_ != nullptr) {
        return;
    }
    // 栈必须放内部 RAM：收到广播后 SetBase() 会写 NVS，
    // 写 Flash 期间 cache 是关的，PSRAM 上的栈会直接 assert 重启。
    xTaskCreate(TaskEntry, "relay_find", 8192, this, 3, &task_);
}

void MusicRelayDiscovery::TaskEntry(void* arg) {
    static_cast<MusicRelayDiscovery*>(arg)->Run();
}

void MusicRelayDiscovery::Run() {
    // 等 WiFi 拿到地址：没网的时候连 socket 都不用建
    for (int i = 0; i < 120; i++) {
        if (!WifiManager::GetInstance().GetIpAddress().empty()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (WifiManager::GetInstance().GetIpAddress().empty()) {
        ESP_LOGW(TAG, "没有 WiFi 地址，自动发现不启动");
        return;
    }

    char buf[320];
    int idle_rounds = 0;
    int sock = -1;

    // 外层：socket 出问题就重建，不要一崩到底再也不工作。
    // 手机热点重启时板子的 IP 会变，旧 socket 偶尔会直接作废，
    // 靠"静默太久就重建"这个笨办法就能自己爬起来，不用人重启板子。
    while (true) {
        if (sock < 0) {
            sock = OpenSocket();
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            idle_rounds = 0;
        }

        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<struct sockaddr*>(&from), &from_len);
        if (n <= 0) {
            // 2 秒超时一次。连续 20 次（40 秒）一个包都没收到，
            // 多半是热点重启后地址变了，重建 socket 重新等。
            if (++idle_rounds >= 20) {
                close(sock);
                sock = -1;
                ESP_LOGW(TAG, "40 秒没收到电脑广播，重建监听（热点重启后属正常）");
            }
            continue;
        }
        idle_rounds = 0;

        buf[n] = '\0';
        HandlePacket(buf, n, from, sock);
    }
}

int MusicRelayDiscovery::OpenSocket() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "创建 UDP socket 失败");
        return -1;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    // 收广播也要开这个，某些 lwip 配置下不开会收不到 255.255.255.255
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct timeval tv = {};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kBroadcastPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "绑定 UDP %u 失败", static_cast<unsigned>(kBroadcastPort));
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "已监听 UDP %u，等电脑端广播中转地址", static_cast<unsigned>(kBroadcastPort));
    return sock;
}

// 把自己的地址报给电脑：热点重启后板子 IP 会变，电脑端靠这个立刻跟上，
// 不然又得满世界查「图片网页的地址是几号」。
void MusicRelayDiscovery::ReplyToPc(int sock, const struct sockaddr_in& from,
                                    uint16_t ack_port) {
    const std::string my_ip = WifiManager::GetInstance().GetIpAddress();
    if (my_ip.empty()) {
        return;
    }

    // 同一个电脑、同一个地址，5 秒内回过就不再回，省得每 3 秒一封。
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    // 地址变了才算新闻，值得打一行；没变就静默（不然每 5 秒刷一行，串口日志没法看）
    const bool ip_changed = (strcmp(last_ack_ip_, my_ip.c_str()) != 0);
    if (!ip_changed && last_ack_ms_ != 0 && now_ms - last_ack_ms_ < 5000) {
        return;
    }

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"svc\":\"xiaozhi-device\",\"ip\":\"%s\",\"web\":80}",
             my_ip.c_str());

    struct sockaddr_in dst = from;
    dst.sin_port = htons(ack_port);   // 广播包的源端口是随机的，必须用 ack 里给的
    const int sent = sendto(sock, payload, strlen(payload), 0,
                            reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst));
    if (sent > 0) {
        last_ack_ms_ = now_ms;
        snprintf(last_ack_ip_, sizeof(last_ack_ip_), "%s", my_ip.c_str());
        if (ip_changed) {
            ESP_LOGI(TAG, "已把本机地址报给电脑: %s", my_ip.c_str());
        }
    }
}

void MusicRelayDiscovery::HandlePacket(const char* json, int len,
                                       const struct sockaddr_in& from, int sock) {
    if (json == nullptr || len <= 0) {
        return;
    }
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        return;
    }

    const cJSON* svc = cJSON_GetObjectItemCaseSensitive(root, "svc");
    if (!cJSON_IsString(svc) || svc->valuestring == nullptr ||
        strcmp(svc->valuestring, kServiceTag) != 0) {
        cJSON_Delete(root);
        return;   // 不是我们的包（比如别的程序也在 8801 上广播）
    }

    const cJSON* ip = cJSON_GetObjectItemCaseSensitive(root, "ip");
    const cJSON* port = cJSON_GetObjectItemCaseSensitive(root, "port");
    if (!cJSON_IsString(ip) || ip->valuestring == nullptr) {
        cJSON_Delete(root);
        return;
    }

    int port_num = 8800;
    if (cJSON_IsNumber(port)) {
        port_num = port->valueint;
    }

    // 电脑端给的"回包端口"：它想收到板子的地址。没有这个字段就是旧版脚本，不回。
    uint16_t ack_port = 0;
    const cJSON* ack = cJSON_GetObjectItemCaseSensitive(root, "ack");
    if (cJSON_IsNumber(ack) && ack->valueint > 0 && ack->valueint < 65536) {
        ack_port = static_cast<uint16_t>(ack->valueint);
    }

    char base[96];
    snprintf(base, sizeof(base), "http://%s:%d", ip->valuestring, port_num);
    cJSON_Delete(root);

    last_seen_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // 先把地址报回去，再做配置判断——电脑端可能刚启动还没见过板子，
    // 而中转地址早就配好了（下面会直接 return），那样它就永远等不到板子。
    if (ack_port != 0) {
        ReplyToPc(sock, from, ack_port);
    }

    auto& ms = MusicStream::GetInstance();
    if (ms.Base() == base) {
        return;   // 已经是这个地址，每 3 秒来一次的例行广播，不重复写 NVS
    }

    // 电脑有多张网卡时会发好几个地址，只有一个真能连上。
    // 当前地址还活着就别换，否则会在两张网卡之间来回跳。
    if (ms.Configured() && ms.Health(nullptr, nullptr)) {
        return;
    }

    ms.SetBase(base);
    ESP_LOGI(TAG, "已自动配置音乐中转: %s（来自电脑广播，不用再口述 IP）", base);
}
