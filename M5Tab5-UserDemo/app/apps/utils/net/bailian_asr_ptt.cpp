/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "bailian_asr_ptt.h"
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_websocket_client.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace net;

static const std::string TAG = "bailian-asr";

static void log_heap_snapshot(const char* where)
{
    if (!where) {
        where = "?";
    }
    const size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_ps  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    const size_t lfb_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t lfb_ps  = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    const size_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t min_ps  = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    mclog::tagInfo(TAG,
                   "heap@{}: free_int={} lfb_int={} min_int={} free_psram={} lfb_psram={} min_psram={} free_dma={}",
                   where,
                   (int)free_int,
                   (int)lfb_int,
                   (int)min_int,
                   (int)free_ps,
                   (int)lfb_ps,
                   (int)min_ps,
                   (int)free_dma);
}

static void log_heap_integrity(const char* where)
{
    bool ok = heap_caps_check_integrity_all(true);
    mclog::tagInfo(TAG, "heap_integrity@{}: {}", where ? where : "?", ok ? "OK" : "BROKEN");
}

static std::string state_to_text(BailianAsrPtt::State st)
{
    switch (st) {
    case BailianAsrPtt::State::Idle:
        return "Idle";
    case BailianAsrPtt::State::Connecting:
        return "Connecting";
    case BailianAsrPtt::State::Starting:
        return "Starting";
    case BailianAsrPtt::State::Recording:
        return "Recording";
    case BailianAsrPtt::State::Finalizing:
        return "Finalizing";
    case BailianAsrPtt::State::Done:
        return "Done";
    case BailianAsrPtt::State::Error:
        return "Error";
    default:
        return "Unknown";
    }
}

static bool parse_ws_event_and_payload(const char* data, int len, std::string& out_event, cJSON*& out_root)
{
    out_event.clear();
    out_root = nullptr;
    if (!data || len <= 0) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        return false;
    }

    cJSON* header = cJSON_GetObjectItemCaseSensitive(root, "header");
    if (!cJSON_IsObject(header)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* event = cJSON_GetObjectItemCaseSensitive(header, "event");
    if (!cJSON_IsString(event) || !event->valuestring) {
        cJSON_Delete(root);
        return false;
    }

    out_event = event->valuestring;
    out_root  = root;
    return true;
}

static bool extract_sentence_result(cJSON* root, std::string& out_text, bool& out_sentence_end)
{
    out_text.clear();
    out_sentence_end = false;

    cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload)) {
        return false;
    }
    cJSON* output = cJSON_GetObjectItemCaseSensitive(payload, "output");
    if (!cJSON_IsObject(output)) {
        return false;
    }
    cJSON* sentence = cJSON_GetObjectItemCaseSensitive(output, "sentence");
    if (!cJSON_IsObject(sentence)) {
        return false;
    }

    cJSON* text = cJSON_GetObjectItemCaseSensitive(sentence, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        out_text = text->valuestring;
    }

    cJSON* end = cJSON_GetObjectItemCaseSensitive(sentence, "sentence_end");
    if (cJSON_IsBool(end)) {
        out_sentence_end = cJSON_IsTrue(end);
    }

    return !out_text.empty();
}

static uint16_t downsample_48k_interleaved_to_16k_mono_ch0(const std::vector<int16_t>& in, int16_t* out,
                                                           uint16_t out_cap)
{
    if (!out || out_cap == 0) {
        return 0;
    }

    // Heuristic: TAB5 mic path is often 4ch; some modes may report 2ch.
    size_t channels = 0;
    if (in.size() >= 4 && (in.size() % 4) == 0) {
        channels = 4;
    } else if (in.size() >= 2 && (in.size() % 2) == 0) {
        channels = 2;
    } else {
        return 0;
    }

    const size_t frames = in.size() / channels;
    const size_t out_frames = frames / 3;  // 48k -> 16k
    const uint16_t n = (uint16_t)std::min<size_t>(out_cap, out_frames);
    for (uint16_t i = 0; i < n; i++) {
        size_t f = (size_t)i * 3;
        out[i] = in[f * channels + 0];
    }
    return n;
}

BailianAsrPtt::BailianAsrPtt()
    : _state(State::Idle),
      _stop_requested(false),
      _task_started(false),
      _sentence_done(false),
      _finished(true),
      _ws_connected(false),
      _run_task_sent(false),
      _task_handle(nullptr),
      _record_task_handle(nullptr),
      _ws(nullptr),
      _start_ms(0)
{
    _audio_q = xQueueCreateStatic(kAudioQueueLen, sizeof(AudioChunk), _audio_q_storage, &_audio_q_static);
}

BailianAsrPtt::~BailianAsrPtt()
{
    stop();
}

bool BailianAsrPtt::start(Config cfg, OnTextFn on_text, OnErrorFn on_error, OnStateFn on_state)
{
    if (running()) {
        return false;
    }
    if (cfg.api_key.empty()) {
        if (on_error) {
            on_error("BAILIAN_API_KEY is empty");
        }
        return false;
    }
    // Current implementation assumes fixed 16kHz/100ms chunks to avoid dynamic allocations.
    cfg.sample_rate_hz = 16000;
    cfg.chunk_ms       = 100;

    _cfg = std::move(cfg);
    _on_text  = std::move(on_text);
    _on_error = std::move(on_error);
    _on_state = std::move(on_state);

    _stop_requested = false;
    _task_started   = false;
    _sentence_done  = false;
    _finished       = false;
    _ws_connected   = false;
    _run_task_sent  = false;
    _final_text[0] = '\0';
    _start_ms = GetHAL()->millis();
    _task_id  = "tab5_" + std::to_string(_start_ms);

    log_heap_snapshot("asr.start");
    log_heap_integrity("asr.start");

    if (_audio_q) {
        xQueueReset(_audio_q);
    }

    // 48kHz * 100ms = 4800 frames; TAB5 mic path is often 4ch => 19200 samples.
    _raw_buf.clear();
    _raw_buf.reserve(19200);

    set_state(State::Connecting);

    TaskHandle_t h = nullptr;
    BaseType_t ok  = xTaskCreate(task_entry, "bailian_asr", 16384, this, 5, &h);
    if (ok != pdPASS) {
        fail("xTaskCreate failed");
        return false;
    }
    _task_handle = h;
    return true;
}

void BailianAsrPtt::stop()
{
    request_stop();
    // Best-effort wait to avoid use-after-free if the owner object is destroyed soon after stop().
    // Keep it short so UI doesn't feel stuck.
    uint32_t start = GetHAL()->millis();
    while (!_finished.load() && (GetHAL()->millis() - start) < 1500) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

BailianAsrPtt::State BailianAsrPtt::state() const
{
    return _state.load();
}

bool BailianAsrPtt::running() const
{
    State st = _state.load();
    return st != State::Idle && st != State::Done && st != State::Error;
}

void BailianAsrPtt::set_state(State st)
{
    _state.store(st);
    if (_on_state) {
        _on_state(st);
    }
}

void BailianAsrPtt::fail(const std::string& msg)
{
    mclog::tagError(TAG, "{} (state={})", msg, state_to_text(_state.load()));
    if (_on_error) {
        _on_error(msg);
    }
    set_state(State::Error);
    request_stop();
}

void BailianAsrPtt::request_stop()
{
    _stop_requested.store(true);
}

void BailianAsrPtt::task_entry(void* arg)
{
    auto* self = static_cast<BailianAsrPtt*>(arg);
    self->run();
    self->_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void BailianAsrPtt::record_task_entry(void* arg)
{
    auto* self = static_cast<BailianAsrPtt*>(arg);
    self->record_loop();
    self->_record_task_handle = nullptr;
    vTaskDelete(nullptr);
}

void BailianAsrPtt::run()
{
    if (!open_ws()) {
        _finished = true;
        return;
    }

    uint32_t last_state_log = 0;
    while (!_stop_requested.load()) {
        if (_sentence_done.load()) {
            break;
        }

        uint32_t now = GetHAL()->millis();
        if (_cfg.max_record_ms > 0 && now - _start_ms > _cfg.max_record_ms) {
            mclog::tagWarn(TAG, "max_record_ms reached, stopping");
            break;
        }

        if (now - last_state_log > 2000) {
            last_state_log = now;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
            mclog::tagInfo(TAG, "state={} stack_hwm={}B", state_to_text(_state.load()),
                           (int)(hwm * sizeof(StackType_t)));
        }

        if (_ws_connected.load() && !_run_task_sent.load()) {
            send_run_task();
            _run_task_sent = true;
        }

        if (_task_started.load() && _record_task_handle == nullptr) {
            TaskHandle_t rh = nullptr;
            BaseType_t ok   = xTaskCreate(record_task_entry, "bailian_asr_rec", 16384, this, 5, &rh);
            if (ok == pdPASS) {
                _record_task_handle = rh;
            } else {
                fail("record task create failed");
            }
        }

        if (_task_started.load()) {
            send_audio_from_queue();
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }

    // Stop recording first so we never send audio while tearing down the websocket.
    _stop_requested.store(true);
    set_state(State::Finalizing);

    uint32_t wait_start = GetHAL()->millis();
    while (_record_task_handle != nullptr && (GetHAL()->millis() - wait_start) < 800) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    send_finish_task();
    close_ws();

    if (_final_text[0] != '\0' && _on_text) {
        _on_text(std::string(_final_text));
    }

    if (_state.load() != State::Error) {
        set_state(State::Done);
    }
    set_state(State::Idle);
    _finished = true;
}

static std::string heap_short()
{
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t lfb_int  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_ps  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t lfb_ps   = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return "heap(int free=" + std::to_string((int)free_int) + " lfb=" + std::to_string((int)lfb_int) +
           ", psram free=" + std::to_string((int)free_ps) + " lfb=" + std::to_string((int)lfb_ps) + ")";
}

bool BailianAsrPtt::open_ws()
{
    if (_ws) {
        return true;
    }

    _ws_headers = "Authorization: Bearer " + _cfg.api_key + "\r\n";

    log_heap_snapshot("ws.before_init");
    log_heap_integrity("ws.before_init");

    // `esp_websocket_client_init()` allocates buffers/mutexes; `start()` creates a task with given stack.
    // On memory pressure, try smaller stack/buffer before failing.
    for (int task_stack : {16384, 12288}) {
        for (int buf_size : {4096, 2048}) {
            esp_websocket_client_config_t cfg = {};
            cfg.uri                    = _cfg.ws_url.c_str();
            cfg.headers                = _ws_headers.c_str();
            cfg.disable_auto_reconnect = true;
            cfg.task_stack             = task_stack;
            cfg.task_prio              = 5;
            cfg.task_name              = "bailian_ws";
            cfg.buffer_size            = buf_size;
            cfg.crt_bundle_attach      = esp_crt_bundle_attach;
            cfg.network_timeout_ms     = 15000;

            auto ws = esp_websocket_client_init(&cfg);
            if (!ws) {
                mclog::tagWarn(TAG, "ws init failed (stack={} buf={}) {}", task_stack, buf_size, heap_short());
                continue;
            }

            esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, this);

            esp_err_t err = esp_websocket_client_start(ws);
            if (err != ESP_OK) {
                mclog::tagWarn(TAG, "ws start failed (stack={} buf={}) err={} {}", task_stack, buf_size, (int)err,
                               heap_short());
                esp_websocket_client_destroy(ws);
                continue;
            }

            _ws = ws;
            log_heap_snapshot("ws.started");
            return true;
        }
    }

    log_heap_snapshot("ws.init_failed");
    log_heap_integrity("ws.init_failed");
    fail(std::string("websocket init/start failed (") + heap_short() + ")");
    return false;
}

void BailianAsrPtt::close_ws()
{
    if (!_ws) {
        return;
    }
    auto ws = _ws;
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    _ws = nullptr;
}

void BailianAsrPtt::send_run_task()
{
    if (!_ws) {
        return;
    }
    set_state(State::Starting);

    std::string params = "\"format\":\"pcm\",\"sample_rate\":";
    params += std::to_string(_cfg.sample_rate_hz);
    params += ",\"semantic_punctuation_enabled\":";
    params += (_cfg.semantic_punctuation_enabled ? "true" : "false");
    params += ",\"max_sentence_silence\":";
    params += std::to_string(_cfg.max_sentence_silence);

    std::string msg;
    msg += "{";
    msg += "\"header\":{\"action\":\"run-task\",\"task_id\":\"" + _task_id + "\",\"streaming\":\"duplex\"},";
    msg += "\"payload\":{";
    msg += "\"task_group\":\"audio\",\"task\":\"asr\",\"function\":\"recognition\",";
    msg += "\"model\":\"" + _cfg.model + "\",";
    msg += "\"parameters\":{" + params + "},";
    msg += "\"input\":{}";
    msg += "}";
    msg += "}";

    esp_websocket_client_send_text(_ws, msg.c_str(), msg.size(), pdMS_TO_TICKS(2000));
}

void BailianAsrPtt::send_finish_task()
{
    if (!_ws) {
        return;
    }
    std::string msg;
    msg += "{";
    msg += "\"header\":{\"action\":\"finish-task\",\"task_id\":\"" + _task_id + "\"},";
    msg += "\"payload\":{}";
    msg += "}";

    esp_websocket_client_send_text(_ws, msg.c_str(), msg.size(), pdMS_TO_TICKS(1000));
}

void BailianAsrPtt::record_loop()
{
    uint32_t last_hwm_ms = 0;

    while (!_stop_requested.load()) {
        if (_sentence_done.load()) {
            break;
        }

        if (_state.load() != State::Recording) {
            set_state(State::Recording);
        }

        GetHAL()->audioRecord(_raw_buf, _cfg.chunk_ms, _cfg.mic_gain);
        AudioChunk chunk;
        chunk.sample_count =
            downsample_48k_interleaved_to_16k_mono_ch0(_raw_buf, chunk.data, (uint16_t)kChunkSamples);
        if (chunk.sample_count == 0) {
            continue;
        }

        if (_audio_q) {
            if (xQueueSend(_audio_q, &chunk, 0) != pdTRUE) {
                // Drop oldest
                AudioChunk drop;
                (void)xQueueReceive(_audio_q, &drop, 0);
                (void)xQueueSend(_audio_q, &chunk, 0);
            }
        }

        uint32_t now = GetHAL()->millis();
        if (now - last_hwm_ms > 2000) {
            last_hwm_ms = now;
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
            mclog::tagInfo(TAG, "rec stack_hwm={}B q_waiting={}", (int)(hwm * sizeof(StackType_t)),
                           _audio_q ? (int)uxQueueMessagesWaiting(_audio_q) : -1);
        }
    }
}

bool BailianAsrPtt::send_audio_from_queue()
{
    if (!_audio_q || !_ws || !_ws_connected.load()) {
        return false;
    }
    bool any = false;
    AudioChunk chunk;
    while (xQueueReceive(_audio_q, &chunk, 0) == pdTRUE) {
        if (chunk.sample_count == 0) {
            continue;
        }
        const int bytes = (int)chunk.sample_count * (int)sizeof(int16_t);
        int sent = esp_websocket_client_send_bin(_ws, (const char*)chunk.data, bytes, pdMS_TO_TICKS(50));
        if (sent < 0) {
            mclog::tagWarn(TAG, "send_bin failed");
            return any;
        }
        any = true;
    }
    return any;
}

bool BailianAsrPtt::handle_ws_text(const char* data, int len)
{
    std::string event;
    cJSON* root = nullptr;
    if (!parse_ws_event_and_payload(data, len, event, root)) {
        return false;
    }

    if (event == "task-started") {
        on_task_started();
        cJSON_Delete(root);
        return true;
    }

    if (event == "result-generated") {
        std::string text;
        bool sentence_end = false;
        if (extract_sentence_result(root, text, sentence_end)) {
            on_result_generated(text, sentence_end);
        }
        cJSON_Delete(root);
        return true;
    }

    if (event == "task-finished") {
        on_task_finished();
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return true;
}

void BailianAsrPtt::on_task_started()
{
    mclog::tagInfo(TAG, "task-started");
    _task_started.store(true);
}

void BailianAsrPtt::on_result_generated(const std::string& text, bool sentence_end)
{
    if (text.empty()) {
        return;
    }

    mclog::tagInfo(TAG, "result-generated: end={} text.len={}", sentence_end ? 1 : 0, (int)text.size());

    if (sentence_end) {
        size_t n = std::min<size_t>(sizeof(_final_text) - 1, text.size());
        std::memcpy(_final_text, text.data(), n);
        _final_text[n] = '\0';
        _sentence_done.store(true);
    }
}

void BailianAsrPtt::on_task_finished()
{
    mclog::tagInfo(TAG, "task-finished");
}

void BailianAsrPtt::ws_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    (void)base;
    auto* self = static_cast<BailianAsrPtt*>(handler_args);
    if (!self) {
        return;
    }

    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
    if (!data) {
        return;
    }

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        mclog::tagInfo(TAG, "ws connected");
        self->_ws_connected.store(true);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        mclog::tagWarn(TAG, "ws disconnected");
        self->_ws_connected.store(false);
        self->request_stop();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x1 && data->data_ptr && data->data_len > 0) {  // text
            if (!self->handle_ws_text(data->data_ptr, data->data_len)) {
                std::string snippet(data->data_ptr, data->data_ptr + std::min(240, data->data_len));
                mclog::tagWarn(TAG, "ws text unparsed: {}", snippet);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        mclog::tagError(TAG,
                        "ws error: type={} http={} tls_esp={} tls_stack={} verify_flags={} errno={}",
                        (int)data->error_handle.error_type,
                        data->error_handle.esp_ws_handshake_status_code,
                        (int)data->error_handle.esp_tls_last_esp_err,
                        data->error_handle.esp_tls_stack_err,
                        data->error_handle.esp_tls_cert_verify_flags,
                        data->error_handle.esp_transport_sock_errno);
        self->fail("websocket error (see log)");
        break;
    default:
        break;
    }
}
