#include "internet_radio_player.h"

#include "audio_codec.h"
#include "board.h"

#include <http.h>
#include <esp_log.h>
#include <esp_audio_types.h>
#include <simple_dec/esp_audio_simple_dec.h>
#include <decoder/impl/esp_aac_dec.h>
#include <decoder/impl/esp_mp3_dec.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#define TAG "InternetRadio"

namespace {

constexpr int kHttpTimeoutMs = 15000;
constexpr int kTaskStackSize = 12 * 1024;
constexpr int kTaskPriority = 6;
constexpr int kOutputTaskStackSize = 6 * 1024;
constexpr int kOutputTaskPriority = 5;
constexpr size_t kReadBufferSize = 2048;
constexpr size_t kInitialDecodeBufferSize = 8192;
constexpr size_t kMaxPendingBytes = 16 * 1024;
constexpr int kRadioGainQ8 = 640; // 2.5x
constexpr int kPrebufferMs = 1200;
constexpr int kMaxPcmBufferMs = 2500;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool Contains(const std::string& value, const char* needle) {
    return value.find(needle) != std::string::npos;
}

bool IsRedirectStatus(int status_code) {
    return status_code == 301 || status_code == 302 || status_code == 303 ||
        status_code == 307 || status_code == 308;
}

bool IsAbsoluteUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

esp_audio_simple_dec_type_t DetectDecoderType(const std::string& codec_hint,
        const std::string& content_type, const std::string& url) {
    std::string combined = ToLower(codec_hint + " " + content_type + " " + url);
    if (Contains(combined, "aac") || Contains(combined, "a-a-c")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (Contains(combined, "mpeg") || Contains(combined, "mp3") ||
            Contains(combined, "audio/mpeg")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

const char* DecoderName(esp_audio_simple_dec_type_t type) {
    switch (type) {
    case ESP_AUDIO_SIMPLE_DEC_TYPE_AAC:
        return "aac";
    case ESP_AUDIO_SIMPLE_DEC_TYPE_MP3:
        return "mp3";
    default:
        return "unknown";
    }
}

void RegisterRadioDecodersOnce() {
    static bool registered = false;
    if (registered) {
        return;
    }
    auto ret = esp_mp3_dec_register();
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "MP3 decoder register returned %d", ret);
    }
    ret = esp_aac_dec_register();
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "AAC decoder register returned %d", ret);
    }
    registered = true;
}

bool OpenHttpWithRedirects(const std::string& initial_url, const std::string& user_agent,
        std::unique_ptr<Http>& http, std::string& final_url, std::string& content_type,
        std::string& error) {
    final_url = initial_url;

    for (int redirect = 0; redirect < 4; ++redirect) {
        http = Board::GetInstance().GetNetwork()->CreateHttp(2);
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("User-Agent", user_agent);
        http->SetHeader("Accept", "audio/mpeg, audio/aac, audio/aacp, */*");
        http->SetHeader("Icy-MetaData", "0");
        http->SetHeader("Connection", "close");

        ESP_LOGD(TAG, "Opening radio stream: %s", final_url.c_str());
        if (!http->Open("GET", final_url)) {
            error = "라디오 스트림 연결에 실패했습니다.";
            return false;
        }

        int status_code = http->GetStatusCode();
        if (status_code == 200) {
            content_type = http->GetResponseHeader("Content-Type");
            return true;
        }

        if (IsRedirectStatus(status_code)) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            if (location.empty() || !IsAbsoluteUrl(location)) {
                error = "라디오 스트림 리다이렉트 주소가 올바르지 않습니다.";
                return false;
            }
            final_url = location;
            continue;
        }

        http->Close();
        error = "라디오 스트림 HTTP 오류: " + std::to_string(status_code);
        return false;
    }

    error = "라디오 스트림 리다이렉트가 너무 많습니다.";
    return false;
}

} // namespace

InternetRadioPlayer::~InternetRadioPlayer() {
    Stop();
    CloseResampler();
}

bool InternetRadioPlayer::Start(const std::string& url, const std::string& name,
        const std::string& codec_hint) {
    if (url.empty()) {
        SetError("라디오 스트림 주소가 비어 있습니다.");
        return false;
    }

    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (task_handle_ != nullptr) {
            last_error_ = "이전 라디오 스트림을 아직 정리하는 중입니다.";
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stream_url_ = url;
        station_name_ = name.empty() ? "Internet Radio" : name;
        codec_hint_ = codec_hint;
        last_error_.clear();
        state_ = State::Starting;
        task_handle_ready_ = false;
        stop_requested_ = false;
    }

    TaskHandle_t handle = nullptr;
    BaseType_t ok = xTaskCreate(TaskEntry, "radio_player", kTaskStackSize, this,
        kTaskPriority, &handle);
    if (ok != pdPASS) {
        SetError("라디오 재생 태스크를 만들 수 없습니다.");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    task_handle_ = handle;
    task_handle_ready_ = true;
    return true;
}

void InternetRadioPlayer::Stop() {
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task = task_handle_;
        if (task == nullptr && state_ == State::Stopped) {
            return;
        }
        if (task == nullptr) {
            state_ = State::Stopped;
            stop_requested_ = false;
            return;
        }
        stop_requested_ = true;
        if (active_http_ != nullptr) {
            active_http_->Close();
        }
    }
    pcm_cv_.notify_all();

    if (task != nullptr && xTaskGetCurrentTaskHandle() != task) {
        for (int i = 0; i < 40; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (task_handle_ == nullptr) {
                    return;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ESP_LOGW(TAG, "Timed out waiting for radio task to stop");
    }
}

bool InternetRadioPlayer::IsPlaying() const {
    auto state_value = state();
    return state_value == State::Starting || state_value == State::Playing;
}

InternetRadioPlayer::State InternetRadioPlayer::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string InternetRadioPlayer::station_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_name_;
}

std::string InternetRadioPlayer::stream_url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stream_url_;
}

std::string InternetRadioPlayer::codec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return codec_hint_;
}

std::string InternetRadioPlayer::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void InternetRadioPlayer::SetOnStopped(std::function<void(State final_state,
        const std::string& error)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_stopped_ = std::move(callback);
}

void InternetRadioPlayer::TaskEntry(void* arg) {
    auto* player = static_cast<InternetRadioPlayer*>(arg);
    while (!player->task_handle_ready_.load()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    player->TaskMain();
    vTaskDelete(nullptr);
}

void InternetRadioPlayer::OutputTaskEntry(void* arg) {
    auto* player = static_cast<InternetRadioPlayer*>(arg);
    player->OutputTaskMain();
    vTaskDelete(nullptr);
}

bool InternetRadioPlayer::ShouldStop() const {
    return stop_requested_.load();
}

void InternetRadioPlayer::SetState(State state) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
}

void InternetRadioPlayer::SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
    state_ = State::Error;
}

void InternetRadioPlayer::NotifyStopped(State final_state, const std::string& error) {
    std::function<void(State final_state, const std::string& error)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = on_stopped_;
    }
    if (callback) {
        callback(final_state, error);
    }
}

void InternetRadioPlayer::CloseResampler() {
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
        resampler_src_rate_ = 0;
    }
}

void InternetRadioPlayer::QueuePcm(std::vector<int16_t>&& data) {
    if (data.empty()) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    size_t max_samples = static_cast<size_t>(output_sample_rate_) * kMaxPcmBufferMs / 1000;
    pcm_cv_.wait(lock, [this, max_samples, incoming = data.size()]() {
        return stop_requested_.load() || pcm_decoder_done_ ||
            pcm_queued_samples_ + incoming <= max_samples;
    });
    if (stop_requested_.load() || pcm_decoder_done_) {
        return;
    }

    pcm_queued_samples_ += data.size();
    pcm_queue_.push_back(std::move(data));
    pcm_cv_.notify_all();
}

void InternetRadioPlayer::OutputPcm(AudioCodec* codec, const uint8_t* data, size_t size,
        int sample_rate, int channels, int bits_per_sample) {
    if (codec == nullptr || data == nullptr || size == 0 || bits_per_sample != 16 ||
            sample_rate <= 0 || channels <= 0) {
        return;
    }

    const int16_t* samples = reinterpret_cast<const int16_t*>(data);
    size_t sample_count = size / sizeof(int16_t);
    size_t frame_count = sample_count / channels;
    if (frame_count == 0) {
        return;
    }

    std::vector<int16_t> mono(frame_count);
    if (channels == 1) {
        std::memcpy(mono.data(), samples, frame_count * sizeof(int16_t));
    } else {
        for (size_t i = 0; i < frame_count; ++i) {
            int32_t mixed = 0;
            int mix_channels = std::min(channels, 2);
            for (int ch = 0; ch < mix_channels; ++ch) {
                mixed += samples[i * channels + ch];
            }
            mono[i] = static_cast<int16_t>(mixed / mix_channels);
        }
    }

    for (auto& sample : mono) {
        int32_t amplified = static_cast<int32_t>(sample) * kRadioGainQ8 / 256;
        if (amplified > INT16_MAX) {
            amplified = INT16_MAX;
        } else if (amplified < INT16_MIN) {
            amplified = INT16_MIN;
        }
        sample = static_cast<int16_t>(amplified);
    }

    if (sample_rate != codec->output_sample_rate()) {
        if (resampler_ == nullptr || resampler_src_rate_ != sample_rate) {
            CloseResampler();
            esp_ae_rate_cvt_cfg_t cfg = {
                .src_rate = static_cast<uint32_t>(sample_rate),
                .dest_rate = static_cast<uint32_t>(codec->output_sample_rate()),
                .channel = 1,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity = 2,
                .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            auto ret = esp_ae_rate_cvt_open(&cfg, &resampler_);
            if (resampler_ == nullptr) {
                ESP_LOGE(TAG, "Failed to create radio resampler: %d", ret);
                return;
            }
            resampler_src_rate_ = sample_rate;
        }

        uint32_t target_size = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(resampler_, mono.size(), &target_size);
        std::vector<int16_t> resampled(target_size);
        uint32_t actual_output = target_size;
        esp_ae_rate_cvt_process(resampler_, reinterpret_cast<esp_ae_sample_t>(mono.data()), mono.size(),
            reinterpret_cast<esp_ae_sample_t>(resampled.data()), &actual_output);
        resampled.resize(actual_output);
        QueuePcm(std::move(resampled));
    } else {
        QueuePcm(std::move(mono));
    }
}

void InternetRadioPlayer::OutputTaskMain() {
    AudioCodec* codec = nullptr;
    int output_sample_rate = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        codec = output_codec_;
        output_sample_rate = output_sample_rate_;
    }
    if (codec == nullptr || output_sample_rate <= 0) {
        ESP_LOGE(TAG, "Radio output task missing codec");
        std::lock_guard<std::mutex> lock(mutex_);
        output_task_handle_ = nullptr;
        pcm_cv_.notify_all();
        return;
    }

    const size_t prebuffer_samples = static_cast<size_t>(output_sample_rate) * kPrebufferMs / 1000;
    bool output_enabled_here = false;

    while (true) {
        std::vector<int16_t> chunk;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            pcm_cv_.wait(lock, [this, prebuffer_samples]() {
                return stop_requested_.load() || pcm_decoder_done_ ||
                    (pcm_output_started_ && !pcm_queue_.empty()) ||
                    (!pcm_output_started_ && pcm_queued_samples_ >= prebuffer_samples);
            });

            if (stop_requested_.load()) {
                break;
            }

            if (!pcm_output_started_) {
                if (pcm_queued_samples_ >= prebuffer_samples) {
                    pcm_output_started_ = true;
                    ESP_LOGD(TAG, "Radio prebuffer ready: %u samples",
                        static_cast<unsigned>(pcm_queued_samples_));
                } else if (pcm_decoder_done_) {
                    if (pcm_queue_.empty()) {
                        break;
                    }
                    pcm_output_started_ = true;
                } else {
                    continue;
                }
            }

            if (pcm_queue_.empty()) {
                if (pcm_decoder_done_) {
                    break;
                }
                continue;
            }

            chunk = std::move(pcm_queue_.front());
            pcm_queue_.pop_front();
            pcm_queued_samples_ -= chunk.size();
            pcm_cv_.notify_all();
        }

        if (!codec->output_enabled()) {
            if (output_enabled_here) {
                ESP_LOGW(TAG, "Radio output was disabled during playback; enabling it again");
            }
            codec->EnableOutput(true);
            output_enabled_here = true;
        }
        codec->OutputData(chunk);
    }

    if (output_enabled_here && codec->output_enabled()) {
        codec->EnableOutput(false);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_task_handle_ = nullptr;
        pcm_queue_.clear();
        pcm_queued_samples_ = 0;
        pcm_output_started_ = false;
    }
    pcm_cv_.notify_all();
}

void InternetRadioPlayer::TaskMain() {
    std::string url;
    std::string name;
    std::string codec_hint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = stream_url_;
        name = station_name_;
        codec_hint = codec_hint_;
    }

    auto finish_task = [this]() {
        State final_state;
        std::string final_error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_handle_ = nullptr;
            active_http_ = nullptr;
            output_codec_ = nullptr;
            output_sample_rate_ = 0;
            pcm_decoder_done_ = true;
            pcm_output_started_ = false;
            pcm_queue_.clear();
            pcm_queued_samples_ = 0;
            if (stop_requested_ || state_ == State::Playing || state_ == State::Starting) {
                state_ = State::Stopped;
            }
            final_state = state_;
            final_error = last_error_;
            stop_requested_ = false;
        }
        pcm_cv_.notify_all();
        NotifyStopped(final_state, final_error);
    };

    RegisterRadioDecodersOnce();

    auto codec = Board::GetInstance().GetAudioCodec();
    std::unique_ptr<Http> http;
    std::string final_url;
    std::string content_type;
    std::string error;
    bool opened = OpenHttpWithRedirects(url,
        "xiaozhi-esp32-vq2/0.1 (https://github.com/acidsound/xiaozhi-esp32)",
        http, final_url, content_type, error);
    if (http != nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_http_ = http.get();
    }

    auto close_http = [this, &http]() {
        if (http == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_http_ == http.get()) {
                active_http_ = nullptr;
            }
        }
        http->Close();
    };

    if (!opened || ShouldStop()) {
        if (!error.empty() && !ShouldStop()) {
            SetError(error);
        }
        close_http();
        finish_task();
        return;
    }

    esp_audio_simple_dec_type_t dec_type = DetectDecoderType(codec_hint, content_type, final_url);
    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_cfg.aac_plus_enable = true;

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = dec_type,
        .dec_cfg = dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? static_cast<void*>(&aac_cfg) : nullptr,
        .cfg_size = dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? static_cast<int>(sizeof(aac_cfg)) : 0,
        .use_frame_dec = false,
    };

    esp_audio_simple_dec_handle_t decoder = nullptr;
    auto ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        close_http();
        SetError(std::string("라디오 디코더를 열 수 없습니다: ") + DecoderName(dec_type));
        finish_task();
        return;
    }

    ESP_LOGI(TAG, "Radio playback started: %s codec=%s content-type=%s",
        name.c_str(), DecoderName(dec_type), content_type.c_str());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_codec_ = codec;
        output_sample_rate_ = codec->output_sample_rate();
        pcm_decoder_done_ = false;
        pcm_output_started_ = false;
        pcm_queued_samples_ = 0;
        pcm_queue_.clear();
        output_task_handle_ = nullptr;
    }

    TaskHandle_t output_handle = nullptr;
    BaseType_t output_ok = xTaskCreate(OutputTaskEntry, "radio_output", kOutputTaskStackSize, this,
        kOutputTaskPriority, &output_handle);
    if (output_ok != pdPASS) {
        close_http();
        esp_audio_simple_dec_close(decoder);
        SetError("라디오 출력 태스크를 만들 수 없습니다.");
        finish_task();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_task_handle_ = output_handle;
    }

    SetState(State::Playing);
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    std::vector<uint8_t> read_buffer(kReadBufferSize);
    std::vector<uint8_t> pending;
    std::vector<uint8_t> decode_buffer(kInitialDecodeBufferSize);
    int sample_rate = 0;
    int channels = 0;
    int bits_per_sample = 0;
    bool decode_error = false;

    while (!ShouldStop()) {
        int bytes_read = http->Read(reinterpret_cast<char*>(read_buffer.data()), read_buffer.size());
        if (bytes_read < 0) {
            if (!ShouldStop()) {
                SetError("라디오 스트림을 읽는 중 오류가 발생했습니다.");
            }
            break;
        }
        if (bytes_read == 0) {
            if (!ShouldStop()) {
                SetError("라디오 스트림이 종료되었습니다.");
            }
            break;
        }

        pending.insert(pending.end(), read_buffer.begin(), read_buffer.begin() + bytes_read);
        if (pending.size() > kMaxPendingBytes) {
            ESP_LOGW(TAG, "Radio pending input exceeded %u bytes; dropping buffered data",
                static_cast<unsigned>(pending.size()));
            pending.clear();
            continue;
        }

        size_t offset = 0;
        while (offset < pending.size() && !ShouldStop()) {
            esp_audio_simple_dec_raw_t raw = {
                .buffer = pending.data() + offset,
                .len = static_cast<uint32_t>(pending.size() - offset),
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            esp_audio_simple_dec_out_t out_frame = {
                .buffer = decode_buffer.data(),
                .len = static_cast<uint32_t>(decode_buffer.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };

            ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out_frame.needed_size > decode_buffer.size()) {
                decode_buffer.resize(out_frame.needed_size);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "Radio decode failed: %d", ret);
                decode_error = true;
                break;
            }

            if (out_frame.decoded_size > 0) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                    if (sample_rate != static_cast<int>(info.sample_rate) ||
                            channels != static_cast<int>(info.channel)) {
                        CloseResampler();
                    }
                    sample_rate = info.sample_rate;
                    channels = info.channel;
                    bits_per_sample = info.bits_per_sample;
                }
                OutputPcm(codec, out_frame.buffer, out_frame.decoded_size,
                    sample_rate, channels, bits_per_sample);
            }

            if (raw.consumed == 0) {
                break;
            }
            offset += raw.consumed;
        }

        if (decode_error) {
            if (!ShouldStop()) {
                SetError("라디오 오디오 형식을 디코딩할 수 없습니다.");
            }
            break;
        }

        if (offset > 0) {
            pending.erase(pending.begin(), pending.begin() + offset);
        }
    }

    esp_audio_simple_dec_close(decoder);
    CloseResampler();
    close_http();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pcm_decoder_done_ = true;
    }
    pcm_cv_.notify_all();

    if (output_handle != nullptr) {
        for (int i = 0; i < 100; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (output_task_handle_ == nullptr) {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Timed out waiting for radio output task to stop");
        }
    }

    ESP_LOGI(TAG, "Radio playback stopped");
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_task_handle_ == nullptr) {
            pcm_queue_.clear();
            pcm_queued_samples_ = 0;
            pcm_output_started_ = false;
        }
    }
    finish_task();
}
