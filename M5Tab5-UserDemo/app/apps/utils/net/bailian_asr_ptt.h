/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_websocket_client.h>

namespace net {

class BailianAsrPtt {
public:
    struct Config {
        std::string ws_url = "wss://dashscope.aliyuncs.com/api-ws/v1/inference/";
        std::string model  = "fun-asr-realtime";
        std::string api_key;

        uint16_t sample_rate_hz       = 16000;
        uint16_t chunk_ms             = 100;
        uint16_t max_record_ms        = 12000;
        uint32_t max_sentence_silence = 1300;
        bool semantic_punctuation_enabled = false;  // false => VAD-based segmentation (per docs)
        float mic_gain                = 80.0f;
    };

    enum class State {
        Idle = 0,
        Connecting,
        Starting,
        Recording,
        Finalizing,
        Done,
        Error,
    };

    using OnTextFn  = std::function<void(const std::string&)>;
    using OnErrorFn = std::function<void(const std::string&)>;
    using OnStateFn = std::function<void(State)>;

    BailianAsrPtt();
    ~BailianAsrPtt();

    bool start(Config cfg, OnTextFn on_text, OnErrorFn on_error, OnStateFn on_state);
    void stop();

    State state() const;
    bool running() const;

private:
    void set_state(State st);
    void fail(const std::string& msg);
    void request_stop();

    void run();
    bool open_ws();
    void close_ws();
    void send_run_task();
    void send_finish_task();
    void record_loop();
    bool send_audio_from_queue();

    bool handle_ws_text(const char* data, int len);
    void on_task_started();
    void on_result_generated(const std::string& text, bool sentence_end);
    void on_task_finished();

    static void task_entry(void* arg);
    static void record_task_entry(void* arg);
    static void ws_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);

    Config _cfg;
    std::string _ws_headers;

    OnTextFn _on_text;
    OnErrorFn _on_error;
    OnStateFn _on_state;

    std::atomic<State> _state;
    std::atomic<bool> _stop_requested;
    std::atomic<bool> _task_started;
    std::atomic<bool> _sentence_done;
    std::atomic<bool> _finished;
    std::atomic<bool> _ws_connected;
    std::atomic<bool> _run_task_sent;

    TaskHandle_t _task_handle;
    TaskHandle_t _record_task_handle;

    esp_websocket_client_handle_t _ws;
    uint32_t _start_ms;
    std::string _task_id;

    static constexpr size_t kChunkSamples = 1600;  // 16kHz * 100ms
    struct AudioChunk {
        uint16_t sample_count = 0;
        int16_t data[kChunkSamples] = {0};
    };
    static constexpr size_t kAudioQueueLen = 6;
    StaticQueue_t _audio_q_static;
    uint8_t _audio_q_storage[kAudioQueueLen * sizeof(AudioChunk)] = {0};
    QueueHandle_t _audio_q = nullptr;

    // Final result copied from WS callback; fixed buffer to avoid heap allocations in callback context.
    char _final_text[512] = {0};

    // Reusable raw audio buffer to reduce heap churn in record_loop().
    std::vector<int16_t> _raw_buf;
};

}  // namespace net
