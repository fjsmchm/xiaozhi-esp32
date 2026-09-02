#ifndef ANKONG_KWS_WAKE_WORD_H
#define ANKONG_KWS_WAKE_WORD_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <model_path.h>

#include <deque>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "audio_codec.h"
#include "wake_word.h"
#include "custom_wake_word.h"
#include "wake_word_audio_cache.h"

// 安控云自训唤醒引擎: fbank40 + splice3 + FSMNSeleNetV2 + 解码规则
// 权重经 EMBED_FILES 内嵌(ankong_weights.bin, 由 b_model.txt 转出, 含NEON行补齐)
// 继承CustomWakeWord以复用afe_audio_engine的成员类型, 全部虚函数覆盖为其实现
class AnkongKwsWakeWord : public CustomWakeWord {
public:
    AnkongKwsWakeWord();
    ~AnkongKwsWakeWord();

    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) override;
    void Feed(const std::vector<int16_t>& data) override;
    void FeedMono(const int16_t* data, size_t samples);
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) override;
    void Start() override;
    void Stop() override;
    size_t GetFeedSize() override;
    void EncodeWakeWordData() override;
    bool GetWakeWordOpus(std::vector<uint8_t>& opus) override;
    const std::string& GetLastDetectedWakeWord() const override { return last_detected_wake_word_; }

private:
    // ---- 模型参数(flash常量, 从内嵌bin按 replica.py 顺序解析) ----
    struct Unit {
        const float* shrink;   // [68][144] 无偏置
        const float* cl;       // [16][68] 左记忆(行=时间抽头)
        const float* cr;       // [68]     右1帧
        const float* ew;       // [144][68]
        const float* eb;       // [144]
    } units_[5];
    const float* featmap_w_ = nullptr;   // [144][120]
    const float* featmap_b_ = nullptr;   // [144]
    const float* dec_w_ = nullptr;       // [5][144]
    const float* dec_b_ = nullptr;       // [5]
    bool model_ok_ = false;

    // ---- 音频/特征状态 ----
    float win_[400];
    bool win_primed_ = false;
    float fft_re_[512];
    float fft_im_[512];
    float preemph_last_ = 0.0f;

    float f_hist_[3][40];      // 最近3帧raw fbank(t-2,t-1,t)
    int f_cnt_ = 0;

    static const int CMN_WIN = 300;   // 流式CMN尾随窗(训练为逐句均值, 流式近似)
    float cmn_buf_[300][40];
    float cmn_sum_[40];
    int cmn_cnt_ = 0, cmn_pos_ = 0;

    // FSMN层管线: 每层输入前帧缓存 + h环(17帧)
    float layer_in_prev_[5][144];
    float layer_in_cur_[5][144];
    bool layer_has_prev_[5];
    float h_ring_[5][17][68];
    int ring_pos_[5];
    float x_out_[144];          // 第5层最近finalized输出
    int frames_ = 0;

    float post_ring_[40][5];
    int post_cnt_ = 0, post_pos_ = 0;
    float threshold_ = 0.5f;

    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    AudioCodec* codec_ = nullptr;
    std::string last_detected_wake_word_;
    std::atomic<bool> running_ = false;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;

    TaskHandle_t wake_word_encode_task_ = nullptr;
    StaticTask_t* wake_word_encode_task_buffer_ = nullptr;
    StackType_t* wake_word_encode_task_stack_ = nullptr;
    WakeWordAudioCache wake_word_audio_cache_;
    std::deque<std::vector<uint8_t>> wake_word_opus_;
    std::mutex wake_word_mutex_;
    std::condition_variable wake_word_cv_;

    bool ParseWeights(const uint8_t* data, size_t len);
    void FeedSamplesIntoBuffer(const int16_t* data, size_t samples, bool mono);
    void AdvanceOneFrame(const int16_t* chunk160);  // 滑窗前进160样本并处理
    void ComputeFbank();
    void NetworkStep(const float* net_in120);
};

#endif
