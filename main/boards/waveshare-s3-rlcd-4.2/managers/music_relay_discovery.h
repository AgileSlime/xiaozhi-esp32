#ifndef __MUSIC_RELAY_DISCOVERY_H__
#define __MUSIC_RELAY_DISCOVERY_H__

#include <cstdint>

#include <netinet/in.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 音乐中转地址自动发现（板子端）
//
// 以前要人对着板子说一句"音乐服务器设为 192.168.1.10:8800"——这串 IP 谁都记不住，
// 而且路由器重新分配地址之后还得再说一遍。现在反过来：电脑上的 relay.py 每 3 秒
// 往局域网 8801 端口广播一次自己的地址，板子收到就自动记下来，人一句话都不用说。
//
// 广播包（JSON，一行）：
//   {"svc":"xiaozhi-music-relay","ip":"10.76.94.160","port":8800,"ack":8802}
//
// 板子收到后会往 ack 端口回一个包，把自己的地址报给电脑：
//   {"svc":"xiaozhi-device","ip":"10.76.94.120","web":80}
// 这样电脑端也能"看见"板子——热点重启后板子 IP 变了，电脑这边立刻就知道，
// 不用再满世界查「板子现在是几号地址」（这就是图片上传网页打不开的常见原因）。
//
// 几种边界情况：
//   - 电脑有多张网卡（有线 + 无线）时会发多个包，只有一个地址真的通。
//     板子先拿当前地址做一次 /health，能通就不换，避免在多张网卡之间来回跳。
//   - 同一个地址每 3 秒就来一次，Base() 相同直接返回，不会反复写 NVS。
//   - 一直收不到广播（没装电脑端 / 防火墙拦了）就什么都不做，设备安静退化成离线状态。
//   - 手机热点重启后 IP 会变，老 socket 可能已经作废：静默 40 秒就重建 socket。
class MusicRelayDiscovery {
public:
    static MusicRelayDiscovery& GetInstance();

    // 起一个后台任务监听广播（重复调用安全）
    void Start();

    // 最后一次收到广播的时间（毫秒，0=从没收到），给日志/排错用
    uint32_t LastSeenMs() const { return last_seen_ms_; }

private:
    MusicRelayDiscovery() = default;
    ~MusicRelayDiscovery() = default;
    MusicRelayDiscovery(const MusicRelayDiscovery&) = delete;
    MusicRelayDiscovery& operator=(const MusicRelayDiscovery&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    int OpenSocket();           // 建 socket + bind，失败返回 -1
    void HandlePacket(const char* json, int len,
                      const struct sockaddr_in& from, int sock);
    void ReplyToPc(int sock, const struct sockaddr_in& from, uint16_t ack_port);

    TaskHandle_t task_ = nullptr;
    volatile uint32_t last_seen_ms_ = 0;
    uint32_t last_ack_ms_ = 0;      // 上次回包时间，避免每 3 秒狂回
    char last_ack_ip_[20] = {0};
};

#endif  // __MUSIC_RELAY_DISCOVERY_H__
