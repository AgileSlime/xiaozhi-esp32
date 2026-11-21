#ifndef __SERIAL_CONSOLE_H__
#define __SERIAL_CONSOLE_H__

#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// USB 串口命令通道（电脑控制板子）
//
// 板子插上 USB 之后，电脑那边除了看日志，还能直接发命令控制它。
// 走的是 UART0（就是打日志那个口），读写方向独立，互不干扰。
//
// 支持的命令（一行一条，回车执行）：
//   help                      看这条帮助
//   ip                        打印板子当前 IP
//   relay [host:port]         设置/查看音乐中转地址（不带参数就是查看）
//   play <关键词>              搜歌并播放，同时切到播放器页
//   stop                      停止播放
//   page <home|weather|memo|music>  切页面
//   reboot                    重启
//
// 回显统一是 "CMD> " 开头，方便电脑端脚本按行抓取，不会被日志时间戳干扰。
// 例：
//   CMD> OK 正在播放 晴天 - 周杰伦
//   CMD> FAIL 还没设置中转地址
class CustomLcdDisplay;   // 前向声明，头文件不用拖进整个 LVGL

class SerialConsole {
public:
    static SerialConsole& GetInstance();

    // display 可以为空（只是没有切页能力），start 之后常驻
    void Start(CustomLcdDisplay* display);

    // 给别处复用：往串口回一行（带 CMD> 前缀）
    static void Reply(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    // 远程执行一条命令（WiFi 的 /cmd 用），返回结果文本，一行一条。
    // 不开串口、不插线也能控制板子：电脑那边 HTTP 发过来，这里跑完把结果带回去。
    std::string Execute(const std::string& cmd);

private:
    SerialConsole() = default;
    ~SerialConsole() = default;
    SerialConsole(const SerialConsole&) = delete;
    SerialConsole& operator=(const SerialConsole&) = delete;

    static void TaskEntry(void* arg);
    void Run();
    void HandleLine(char* line);

    CustomLcdDisplay* display_ = nullptr;
    TaskHandle_t task_ = nullptr;

    // 串口和 HTTP 两条命令来源，需要串行化，否则两个线程同时改 len/输出会乱
    std::mutex cmd_mutex_;
    std::mutex reply_mutex_;
    std::string reply_buf_;      // 捕获模式下攒结果
    bool capture_ = false;       // true = Reply 写进 reply_buf_，不写串口
};

#endif  // __SERIAL_CONSOLE_H__
