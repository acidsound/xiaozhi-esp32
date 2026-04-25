#ifndef INTERNET_RADIO_PLAYER_H
#define INTERNET_RADIO_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "esp_ae_rate_cvt.h"

class AudioCodec;
class Http;

class InternetRadioPlayer {
public:
    enum class State {
        Stopped,
        Starting,
        Playing,
        Error,
    };

    InternetRadioPlayer() = default;
    ~InternetRadioPlayer();

    bool Start(const std::string& url, const std::string& name, const std::string& codec_hint);
    void Stop();
    void SetOnStopped(std::function<void(State final_state, const std::string& error)> callback);

    bool IsPlaying() const;
    State state() const;
    std::string station_name() const;
    std::string stream_url() const;
    std::string codec() const;
    std::string last_error() const;

private:
    static void TaskEntry(void* arg);
    static void OutputTaskEntry(void* arg);
    void TaskMain();
    void OutputTaskMain();

    bool ShouldStop() const;
    void SetState(State state);
    void SetError(const std::string& error);
    void NotifyStopped(State final_state, const std::string& error);
    void CloseResampler();
    void QueuePcm(std::vector<int16_t>&& data);
    void OutputPcm(AudioCodec* codec, const uint8_t* data, size_t size,
        int sample_rate, int channels, int bits_per_sample);

    mutable std::mutex mutex_;
    std::condition_variable pcm_cv_;
    TaskHandle_t task_handle_ = nullptr;
    TaskHandle_t output_task_handle_ = nullptr;
    std::atomic<bool> task_handle_ready_{false};
    std::atomic<bool> stop_requested_{false};
    bool pcm_decoder_done_ = false;
    bool pcm_output_started_ = false;
    size_t pcm_queued_samples_ = 0;
    int output_sample_rate_ = 0;
    AudioCodec* output_codec_ = nullptr;
    Http* active_http_ = nullptr;
    std::function<void(State final_state, const std::string& error)> on_stopped_;
    std::deque<std::vector<int16_t>> pcm_queue_;
    State state_ = State::Stopped;
    std::string station_name_;
    std::string stream_url_;
    std::string codec_hint_;
    std::string last_error_;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    int resampler_src_rate_ = 0;
};

#endif
