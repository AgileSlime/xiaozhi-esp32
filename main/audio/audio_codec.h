#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>

#include <vector>
#include <string>
#include <functional>

#include "board.h"

// I2S DMA 环形缓冲：描述符数 × 每个描述符的帧数。
//
// 缓冲时长 = DESC_NUM × FRAME_NUM / 采样率。
//   原来是 6 × 240 = 1440 帧，24000Hz 下只有 **60ms**。
//   只要解码、网络读取、WiFi 抢占（WiFi 任务优先级 23，会打断解码）任何一处
//   卡顿超过 60ms，I2S 就欠载——表现就是放歌断断续续、爆音。
//   改成 8 × 512 = 4096 帧 ≈ **170ms**，抖动余量接近 3 倍，
//   代价是每方向多占约 5KB 内部 RAM（单声道 16bit 算）。
#define AUDIO_CODEC_DMA_DESC_NUM 8
#define AUDIO_CODEC_DMA_FRAME_NUM 512

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();
    
    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual void OutputData(std::vector<int16_t>& data);
    virtual bool InputData(std::vector<int16_t>& data);
    // 裸指针版本：音乐播放热循环里复用预分配缓冲，避免每块 new vector 造成堆抖动。
    // 直接转发给 Write，省掉 vector 包装和 memcpy。
    virtual void OutputData(const int16_t* data, int samples) {
        Write(data, samples);
    }
    virtual void Start();

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 50;
    float input_gain_ = 0.0;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;
};

#endif // _AUDIO_CODEC_H
