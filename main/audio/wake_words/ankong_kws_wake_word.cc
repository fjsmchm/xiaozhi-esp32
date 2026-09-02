#include "ankong_kws_wake_word.h"
#include "audio_service.h"
#include "ankong_kws_tables.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <cmath>

#define TAG "AnkongKWS"

// EMBED_FILES 内嵌权重符号(IDF规则: 基于文件名生成, 与路径无关)
extern const uint8_t bin_start[] asm("_binary_ankong_weights_bin_start");
extern const uint8_t bin_end[] asm("_binary_ankong_weights_bin_end");

AnkongKwsWakeWord::AnkongKwsWakeWord() {}

AnkongKwsWakeWord::~AnkongKwsWakeWord() {
    if (wake_word_encode_task_stack_ != nullptr) heap_caps_free(wake_word_encode_task_stack_);
    if (wake_word_encode_task_buffer_ != nullptr) heap_caps_free(wake_word_encode_task_buffer_);
}

// 与 replica.py load_model 严格同序(NEON行补齐: 仅末尾bias[5]->[8]实际生效)
bool AnkongKwsWakeWord::ParseWeights(const uint8_t* data, size_t len) {
    size_t n = len / 4;
    if (n != 122572) {
        ESP_LOGE(TAG, "权重长度异常: %zu floats (期望122572)", n);
        return false;
    }
    const float* w = (const float*)data;
    size_t off = 0;
    auto mat = [&](int rows, int cols) -> const float* {
        int pad = (4 - cols % 4) % 4;
        const float* p = w + off;
        off += (size_t)rows * (cols + pad);
        return p;
    };
    auto vec = [&](int m) -> const float* {
        int pad = (4 - m % 4) % 4;
        const float* p = w + off;
        off += m + pad;
        return p;
    };
    featmap_w_ = mat(144, 120);
    featmap_b_ = vec(144);
    for (int i = 0; i < 5; i++) {
        units_[i].shrink = mat(68, 144);
        units_[i].cl = mat(16, 68);
        units_[i].cr = mat(1, 68);
        units_[i].ew = mat(144, 68);
        units_[i].eb = vec(144);
    }
    dec_w_ = mat(5, 144);
    dec_b_ = vec(5);
    if (off != n) {
        ESP_LOGE(TAG, "权重解析错位: off=%zu n=%zu", off, n);
        return false;
    }
    return true;
}

bool AnkongKwsWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    (void)models_list;
    codec_ = codec;
    threshold_ = CONFIG_ANKONG_KWS_THRESHOLD / 100.0f;
    last_detected_wake_word_ = CONFIG_ANKONG_KWS_DISPLAY;

    if (!ParseWeights(bin_start, bin_end - bin_start)) return false;

    memset(win_, 0, sizeof(win_));
    memset(h_ring_, 0, sizeof(h_ring_));
    memset(layer_in_prev_, 0, sizeof(layer_in_prev_));
    memset(layer_in_cur_, 0, sizeof(layer_in_cur_));
    memset(x_out_, 0, sizeof(x_out_));
    memset(cmn_buf_, 0, sizeof(cmn_buf_));
    memset(cmn_sum_, 0, sizeof(cmn_sum_));
    memset(post_ring_, 0, sizeof(post_ring_));
    memset(f_hist_, 0, sizeof(f_hist_));
    for (int i = 0; i < 5; i++) { ring_pos_[i] = 0; layer_has_prev_[i] = false; }
    win_primed_ = false;
    preemph_last_ = 0.0f;
    f_cnt_ = cmn_cnt_ = cmn_pos_ = post_cnt_ = post_pos_ = frames_ = 0;
    model_ok_ = true;
#if CONFIG_SEND_WAKE_WORD_DATA
    if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
        ESP_LOGW(TAG, "Wake-word audio upload disabled");
    }
#endif
    ESP_LOGI(TAG, "安控自训KWS就绪 词=%s 阈值=%.2f", last_detected_wake_word_.c_str(), threshold_);
    return true;
}

void AnkongKwsWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}
void AnkongKwsWakeWord::Start() { running_ = true; }
void AnkongKwsWakeWord::Stop() {
    running_ = false;
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}
size_t AnkongKwsWakeWord::GetFeedSize() { return 480; }

void AnkongKwsWakeWord::Feed(const std::vector<int16_t>& data) {
    FeedSamplesIntoBuffer(data.data(), data.size(), false);
}
void AnkongKwsWakeWord::FeedMono(const int16_t* data, size_t samples) {
    FeedSamplesIntoBuffer(data, samples, true);
}

void AnkongKwsWakeWord::FeedSamplesIntoBuffer(const int16_t* data, size_t samples, bool mono) {
    if (!model_ok_ || data == nullptr || samples == 0) return;
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (!running_) return;
    if (!mono && codec_ && codec_->input_channels() > 1) {
        for (size_t i = 0; i < samples; i += codec_->input_channels()) {
            input_buffer_.push_back(data[i]);
        }
    } else {
        input_buffer_.insert(input_buffer_.end(), data, data + samples);
    }
    while (input_buffer_.size() >= 160) {
        int16_t chunk[160];
        memcpy(chunk, input_buffer_.data(), sizeof(chunk));
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + 160);
#if CONFIG_SEND_WAKE_WORD_DATA
        wake_word_audio_cache_.Store(chunk, 160);
#endif
        AdvanceOneFrame(chunk);
    }
}

void AnkongKwsWakeWord::AdvanceOneFrame(const int16_t* chunk160) {
    if (!win_primed_) {
        // 首3个chunk填满400样本窗
        int off = f_cnt_ * 160;   // 借f_cnt_计已填chunk数(0..2)
        for (int i = 0; i < 160; i++) win_[off + i] = chunk160[i] / 32768.0f;
        f_cnt_++;
        if (f_cnt_ >= 3) { win_primed_ = true; f_cnt_ = 0; }
        return;
    }
    memmove(win_, win_ + 160, 240 * sizeof(float));
    for (int i = 0; i < 160; i++) win_[240 + i] = chunk160[i] / 32768.0f;
    ComputeFbank();
    if (f_cnt_ >= 3) {
        float net_in[120];
        float inv = 1.0f / (cmn_cnt_ ? cmn_cnt_ : 1);
        for (int fi = 0; fi < 3; fi++)
            for (int j = 0; j < 40; j++)
                net_in[fi * 40 + j] = f_hist_[fi][j] - cmn_sum_[j] * inv;
        NetworkStep(net_in);
    }
}

// ---- 512点基2 FFT ----
static void fft512(float* re, float* im) {
    for (int i = 1, j = 0; i < 512; i++) {
        int bit = 512 >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= 512; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < 512; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float xr = re[i + k + len / 2], xi = im[i + k + len / 2];
                float vr = xr * cr - xi * ci, vi = xr * ci + xi * cr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
            }
        }
    }
}

void AnkongKwsWakeWord::ComputeFbank() {
    float x[400];
    x[0] = win_[0] - 0.97f * preemph_last_;
    for (int i = 1; i < 400; i++) x[i] = win_[i] - 0.97f * win_[i - 1];
    preemph_last_ = win_[399];
    for (int i = 0; i < 400; i++) { fft_re_[i] = x[i] * ak_hamming[i]; fft_im_[i] = 0.0f; }
    memset(fft_re_ + 400, 0, 112 * sizeof(float));
    memset(fft_im_ + 400, 0, 112 * sizeof(float));
    fft512(fft_re_, fft_im_);
    float mel[40];
    for (int j = 0; j < 40; j++) {
        float s = 0.0f;
        const float* wv = ak_mel_w[j];
        int st = ak_mel_start[j], en = ak_mel_end[j];
        for (int k = st; k < en; k++) {
            float p = fft_re_[k] * fft_re_[k] + fft_im_[k] * fft_im_[k];
            s += wv[k - st] * p;
        }
        mel[j] = logf(s > 1e-10f ? s : 1e-10f);
    }
    memmove(f_hist_[0], f_hist_[1], 2 * 40 * sizeof(float));
    memcpy(f_hist_[2], mel, 40 * sizeof(float));
    f_cnt_++;
    // 流式CMN
    if (cmn_cnt_ == CMN_WIN) {
        for (int j = 0; j < 40; j++) cmn_sum_[j] -= cmn_buf_[cmn_pos_][j];
    } else {
        cmn_cnt_++;
    }
    for (int j = 0; j < 40; j++) { cmn_buf_[cmn_pos_][j] = mel[j]; cmn_sum_[j] += mel[j]; }
    cmn_pos_ = (cmn_pos_ + 1) % CMN_WIN;
}

static void affine(const float* w, const float* b, const float* x, float* y, int rows, int cols) {
    for (int r = 0; r < rows; r++) {
        float s = b ? b[r] : 0.0f;
        const float* wr = w + (size_t)r * cols;
        for (int c = 0; c < cols; c++) s += wr[c] * x[c];
        y[r] = s;
    }
}

void AnkongKwsWakeWord::NetworkStep(const float* net_in) {
    float x0[144];
    affine(featmap_w_, featmap_b_, net_in, x0, 144, 120);
    for (int i = 0; i < 144; i++) if (x0[i] < 0) x0[i] = 0;
    memcpy(layer_in_cur_[0], x0, sizeof(x0));

    frames_++;
    for (int li = 0; li < 5; li++) {
        Unit& u = units_[li];
        const float* in_t = layer_in_cur_[li];
        float h_t[68];
        affine(u.shrink, nullptr, in_t, h_t, 68, 144);
        int pos = ring_pos_[li];
        memcpy(h_ring_[li][pos], h_t, 68 * sizeof(float));
        ring_pos_[li] = (pos + 1) % 17;

        if (layer_has_prev_[li]) {
            int pt = (pos + 16) % 17;   // 上一帧(t-1)的h在环中位置
            float mem[68];
            for (int d = 0; d < 68; d++) {
                float s = h_ring_[li][pt][d];
                for (int k = 0; k < 16; k++) {
                    int idx = (pt - (15 - k) + 68) % 17;
                    s += u.cl[k * 68 + d] * h_ring_[li][idx][d];
                }
                s += u.cr[d] * h_t[d];   // 右抽头=当前帧h[t]
                mem[d] = s;
            }
            float out[144];
            affine(u.ew, u.eb, mem, out, 144, 68);
            for (int i = 0; i < 144; i++) if (out[i] < 0) out[i] = 0;
            if (li < 4) {
                memcpy(layer_in_cur_[li + 1], out, sizeof(out));
            } else {
                memcpy(x_out_, out, sizeof(out));
            }
        }
        layer_has_prev_[li] = true;
    }

    if (frames_ < 6) return;   // 层间滞后预热(5层×1帧)

    float logits[5];
    affine(dec_w_, dec_b_, x_out_, logits, 5, 144);
    float mx = logits[0];
    for (int i = 1; i < 5; i++) if (logits[i] > mx) mx = logits[i];
    float p[5], sum = 0;
    for (int i = 0; i < 5; i++) { p[i] = expf(logits[i] - mx); sum += p[i]; }
    for (int i = 0; i < 5; i++) p[i] /= sum;
    memcpy(post_ring_[post_pos_], p, 5 * sizeof(float));
    post_pos_ = (post_pos_ + 1) % 40;
    if (post_cnt_ < 40) post_cnt_++;

    float best[4] = {0, 0, 0, 0};
    for (int i = 0; i < post_cnt_; i++) {
        const float* q = post_ring_[i];
        for (int u2 = 0; u2 < 4; u2++) if (q[u2 + 1] > best[u2]) best[u2] = q[u2 + 1];
    }
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 4; j++)
            if (best[j] > best[i]) { float t = best[i]; best[i] = best[j]; best[j] = t; }
    float conf = best[0] * best[1] * best[2];
    if (conf >= threshold_) {
        ESP_LOGI(TAG, "唤醒! conf=%.3f", conf);
        running_ = false;
        // 注意: 调用链(FeedSamplesIntoBuffer)已持有input_buffer_mutex_, 此处不可再加锁
        input_buffer_.clear();
        if (wake_word_detected_callback_) wake_word_detected_callback_(last_detected_wake_word_);
    }
}

void AnkongKwsWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }
    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AnkongKwsWakeWord*)arg;
        {
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                this_->wake_word_audio_cache_.Clear();
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                vTaskDelete(nullptr);
                return;
            }
            (void)ret;
            int frame_size = 0, outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            int packets = 0;
            std::vector<int16_t> in_buffer(frame_size);
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};
            const size_t cached_samples = this_->wake_word_audio_cache_.Size();
            for (size_t offset = 0; offset + static_cast<size_t>(frame_size) <= cached_samples; offset += frame_size) {
                if (this_->wake_word_audio_cache_.Read(offset, in_buffer.data(), frame_size) != static_cast<size_t>(frame_size)) break;
                std::vector<uint8_t> opus_buf(outbuf_size);
                in.buffer = reinterpret_cast<uint8_t*>(in_buffer.data());
                in.len = frame_size * sizeof(int16_t);
                out.buffer = opus_buf.data();
                out.len = outbuf_size;
                out.encoded_bytes = 0;
                ret = esp_opus_enc_process(encoder_handle, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                    this_->wake_word_cv_.notify_all();
                    packets++;
                }
            }
            this_->wake_word_audio_cache_.Clear();
            esp_opus_enc_close(encoder_handle);
            ESP_LOGI(TAG, "Encode wake word opus %d packets", packets);
            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_ak_kws", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AnkongKwsWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
