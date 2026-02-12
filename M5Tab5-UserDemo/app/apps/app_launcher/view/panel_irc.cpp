/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "lvgl_cpp/button.h"
#include "lvgl_cpp/label.h"
#include "lvgl_cpp/text_area.h"
#include "view.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/fonts/font_manager.h>
#include <apps/utils/net/bailian_asr_ptt.h>
#include <apps/utils/net/irc_client.h>
#include <apps/utils/ui/toast.h>
#include <apps/utils/ui/window.h>
#include <hal/hal.h>
#include <wifi_secrets.h>
#include <lvgl.h>
#include <memory>
#include <algorithm>
#include <deque>
#include <mutex>
#include <mooncake_log.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>

using namespace launcher_view;
using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const std::string _tag = "panel-irc";

#ifndef TAB5_ENV_E2E_AUTORUN
#define TAB5_ENV_E2E_AUTORUN 0
#endif

static void e2e_click_obj_once_cb(lv_timer_t* t)
{
    if (!t) {
        return;
    }
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(t));
    if (obj) {
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
    }
    lv_timer_del(t);
}

static std::string wifi_state_to_text(hal::HalBase::WifiState_t st)
{
    switch (st) {
    case hal::HalBase::WIFI_PROVISIONING_AP:
        return "AP(Setup)";
    case hal::HalBase::WIFI_STA_CONNECTING:
        return "STA(connecting)";
    case hal::HalBase::WIFI_STA_CONNECTED:
        return "STA(connected)";
    default:
        return "Stopped";
    }
}

class IrcWindow : public ui::Window {
public:
    IrcWindow()
    {
        config.closeBtn = true;
        config.clickBgClose = false;
        config.title = "";
    }

    void init(lv_obj_t* parent) override
    {
        _parent = parent;
        _disp = lv_display_get_default();
        if (_disp) {
            _prev_rotation = lv_display_get_rotation(_disp);
            // Default to portrait while IRC window is active
            lv_display_set_rotation(_disp, LV_DISPLAY_ROTATION_0);
            _restore_rotation_on_close = true;
        }

        int pw = lv_obj_get_width(parent);
        int ph = lv_obj_get_height(parent);

        // Keyframe coordinates are offsets from center (Window aligns to center).
        config.kfOpened = {0, 0, (int16_t)pw, (int16_t)ph, 255};

        // Closed state animates back to the launcher button near bottom-right.
        config.kfClosed = {(int16_t)(pw / 2 - 30), (int16_t)(ph / 2 - 60), 110, 110, 0};

        ui::Window::init(parent);
    }

    void onOpen() override
    {
        _window->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
        sync_window_frame_to_parent();

        _asr_cb_guard = std::make_shared<std::atomic<bool>>(true);
        _asr_state.store(net::BailianAsrPtt::State::Idle);
        _last_asr_state = net::BailianAsrPtt::State::Idle;

        _status = std::make_unique<Label>(_window->get());
        _status->setTextFont(&lv_font_montserrat_18);
        _status->setTextColor(lv_color_hex(0xDEDEDE));
        _status->setLongMode(LV_LABEL_LONG_WRAP);

        _log = std::make_unique<TextArea>(_window->get());
        _log->setMaxLength(8192);
        _log->setCursorClickPos(false);
        _log->setText("");
        _log->setPasswordMode(false);
        _log->setOneLine(false);
        _log->setBorderWidth(0);
        _log->setRadius(12);
        _log->setBgColor(lv_color_hex(0x2A2A2A));

        const lv_font_t* cn = fonts::get_sd_ttf_font(36);
        if (cn) {
            _log->setTextFont(cn);
        } else {
            _log->setTextFont(&lv_font_montserrat_24);
            if (!GetHAL()->ensureSdCardMounted()) {
                _log->addText("! SD not mounted. Re-insert SD card and reboot.\n");
            } else {
                _log->addText("! font.ttf not found on SD root.\n");
                // Show a quick directory listing to help diagnose filename/path issues.
                auto entries = GetHAL()->scanSdCard("");
                if (entries.empty()) {
                    _log->addText("! /sd is empty (or scan failed)\n");
                } else {
                    _log->addText("* /sd top files:\n");
                    int shown = 0;
                    for (const auto& e : entries) {
                        if (e.isDir) {
                            continue;
                        }
                        _log->addText("  - " + e.name + "\n");
                        if (++shown >= 12) {
                            break;
                        }
                    }
                }
            }
        }

        _btn_rotate = std::make_unique<Button>(_window->get());
        _btn_rotate->setBgColor(lv_color_hex(0x3B3B3B));
        _btn_rotate->setRadius(16);
        _btn_rotate->label().setTextFont(&lv_font_montserrat_20);
        _btn_rotate->label().setTextColor(lv_color_hex(0xE7E7E7));
        _btn_rotate->label().setText("Rotate");
        _btn_rotate->onClick().connect([&] {
            audio::play_next_tone_progression();
            if (!_disp) {
                return;
            }
            lv_display_rotation_t rot = lv_display_get_rotation(_disp);
            switch (rot) {
            case LV_DISPLAY_ROTATION_0:
                rot = LV_DISPLAY_ROTATION_90;
                break;
            case LV_DISPLAY_ROTATION_90:
                rot = LV_DISPLAY_ROTATION_180;
                break;
            case LV_DISPLAY_ROTATION_180:
                rot = LV_DISPLAY_ROTATION_270;
                break;
            default:
                rot = LV_DISPLAY_ROTATION_0;
                break;
            }
            lv_display_set_rotation(_disp, rot);
            sync_window_frame_to_parent();
            apply_layout(true);
        });

        _btn_connect = std::make_unique<Button>(_window->get());
        _btn_connect->setBgColor(lv_color_hex(0x3B3B3B));
        _btn_connect->setRadius(16);
        _btn_connect->label().setTextFont(&lv_font_montserrat_22);
        _btn_connect->label().setTextColor(lv_color_hex(0xE7E7E7));
        _btn_connect->label().setText("Connect");
        _btn_connect->onClick().connect([&] {
            audio::play_next_tone_progression();
            mclog::tagInfo(_tag, "connect clicked");
            if (_log) {
                _log->addText("* Connect clicked\n");
            }
            connect_if_possible(true);
        });

        _btn_send_test = std::make_unique<Button>(_window->get());
        _btn_send_test->setBgColor(lv_color_hex(0x2D5BFF));
        _btn_send_test->setRadius(16);
        _btn_send_test->label().setTextFont(&lv_font_montserrat_22);
        _btn_send_test->label().setTextColor(lv_color_hex(0xFFFFFF));
        _btn_send_test->label().setText("Hello");
        _btn_send_test->onClick().connect([&] {
            audio::play_next_tone_progression();
            _irc.send_privmsg("#tab5", "hello from " + _irc.current_nick());
        });

        _btn_voice = std::make_unique<Button>(_window->get());
        _btn_voice->setBgColor(lv_color_hex(0x616161));
        _btn_voice->setRadius(16);
        _btn_voice->label().setTextFont(&lv_font_montserrat_22);
        _btn_voice->label().setTextColor(lv_color_hex(0xE7E7E7));
        _btn_voice->label().setText("Voice");
        _btn_voice->onClick().connect([&] {
            // Don't play tones here: recording uses the same codec/I2S path and concurrent play+record
            // can destabilize the audio driver.
            if (!GetHAL()->isWifiStaConnected()) {
                ui::pop_a_toast("Wi-Fi not connected", ui::toast_type::warning, 1800);
                return;
            }

            if (_asr.running()) {
                stop_voice();
            } else {
                start_voice();
            }
        });

        _irc_cfg.host = "irc.lemonhall.me";
        _irc_cfg.port = 6667;
        _irc_cfg.nick = net::IrcClient::make_default_nick_9();
        _irc_cfg.user = "tab5";
        _irc_cfg.realname = "Tab5";
        _irc_cfg.auto_join_channel = "#tab5";

        apply_layout(true);
        connect_if_possible(false);
        refresh_status();

        if (_log && TAB5_ENV_E2E_AUTORUN) {
            _log->addText("* E2E_AUTORUN=1\n");
        }

        if (TAB5_ENV_E2E_AUTORUN) {
            _e2e_voice_wait_start_ms = GetHAL()->millis();
            // In E2E mode, try auto-start voice after Wi-Fi is connected and IRC has had time to join.
            lv_timer_t* tm = lv_timer_create(e2e_voice_timer_cb, 500, this);
            if (tm) {
                lv_timer_set_repeat_count(tm, 120); // ~60s max
            }
        }
    }

    void onUpdate() override
    {
        apply_layout(false);

        // Restore rotation only after the close animation is fully finished,
        // to avoid rotating the whole UI mid-animation.
        if (_state == Closed && _restore_rotation_on_close && _disp) {
            _restore_rotation_on_close = false;
            lv_display_set_rotation(_disp, _prev_rotation);
        }

        if (_state != Opened) {
            return;
        }

        auto st = _asr_state.load();
        if (_btn_voice && st != _last_asr_state) {
            _last_asr_state = st;
            switch (st) {
            case net::BailianAsrPtt::State::Recording:
            case net::BailianAsrPtt::State::Starting:
            case net::BailianAsrPtt::State::Connecting:
                _btn_voice->label().setText("REC");
                break;
            case net::BailianAsrPtt::State::Finalizing:
                _btn_voice->label().setText("...");
                break;
            default:
                _btn_voice->label().setText("Voice");
                break;
            }
        }

        refresh_status();

        // Drain ASR queues (UI thread)
        for (;;) {
            std::string text;
            {
                std::lock_guard<std::mutex> lock(_asr_mutex);
                if (_asr_text_queue.empty()) {
                    break;
                }
                text = _asr_text_queue.front();
                _asr_text_queue.pop_front();
            }
            if (!text.empty()) {
                ui::pop_a_toast(text, ui::toast_type::success, 4500);
                if (_log) {
                    _log->addText("[ASR] " + text + "\n");
                }
                _irc.send_privmsg("#tab5", text);
            }
        }

        for (;;) {
            std::string err;
            {
                std::lock_guard<std::mutex> lock(_asr_mutex);
                if (_asr_error_queue.empty()) {
                    break;
                }
                err = _asr_error_queue.front();
                _asr_error_queue.pop_front();
            }
            if (!err.empty()) {
                ui::pop_a_toast("ASR error", ui::toast_type::error, 2000);
                if (_log) {
                    _log->addText("! ASR error: " + err + "\n");
                }
            }
        }

        std::string line;
        while (_irc.poll_line(line)) {
            _log->addText(line + "\n");
        }
    }

    void onClose() override
    {
        audio::play_next_tone_progression();
        _asr.stop();
        if (_asr_cb_guard) {
            _asr_cb_guard->store(false);
        }
        _irc.stop();
        _status.reset();
        _log.reset();
        _btn_rotate.reset();
        _btn_connect.reset();
        _btn_send_test.reset();
        _btn_voice.reset();
    }

private:
    void sync_window_frame_to_parent()
    {
        if (!_parent) {
            return;
        }

        int pw = lv_obj_get_width(_parent);
        int ph = lv_obj_get_height(_parent);

        config.kfOpened = {0, 0, (int16_t)pw, (int16_t)ph, 255};
        config.kfClosed = {(int16_t)(pw / 2 - 30), (int16_t)(ph / 2 - 60), 110, 110, 0};

        // Teleport window to the new opened frame if it's currently visible
        if (_window && (_state == Opening || _state == Opened)) {
            update_anim(config.kfOpened, true);
        }

        // Keep the invisible close hit-area aligned with the opened frame
        if (_close_btn) {
            _close_btn->align(LV_ALIGN_CENTER, config.kfOpened.x + config.kfOpened.w / 2 - 30,
                              config.kfOpened.y - config.kfOpened.h / 2 + 26);
        }
    }

    void apply_layout(bool force)
    {
        if (!_window || !_status || !_log || !_btn_rotate || !_btn_connect || !_btn_send_test || !_btn_voice) {
            return;
        }

        int w = lv_obj_get_width(_window->get());
        int h = lv_obj_get_height(_window->get());
        if (!force && w == _last_layout_w && h == _last_layout_h) {
            return;
        }
        _last_layout_w = w;
        _last_layout_h = h;

        int pad = 18;
        int top = 16;
        int status_h = 52;
        int btn_h = 56;
        int btn_gap = 14;
        int bottom = 16;

        _status->align(LV_ALIGN_TOP_LEFT, pad, top);
        int rotate_w = 140;
        _status->setSize(std::max(0, w - pad * 2 - rotate_w - 10), status_h);

        _btn_rotate->setSize(rotate_w, 44);
        _btn_rotate->align(LV_ALIGN_TOP_RIGHT, -pad, top - 6);

        int log_y = top + status_h + 10;
        int log_h = std::max(120, h - log_y - bottom - btn_h);
        _log->align(LV_ALIGN_TOP_LEFT, pad, log_y);
        _log->setSize(std::max(0, w - pad * 2), std::max(0, log_h));

        int available = std::max(0, w - pad * 2);
        int connect_w = 200;
        int voice_w = 200;
        int mid_w = std::max(120, available - connect_w - voice_w - btn_gap * 2);
        _btn_connect->setSize(connect_w, btn_h);
        _btn_connect->align(LV_ALIGN_BOTTOM_LEFT, pad, -bottom);

        _btn_send_test->setSize(mid_w, btn_h);
        _btn_send_test->align(LV_ALIGN_BOTTOM_LEFT, pad + connect_w + btn_gap, -bottom);

        _btn_voice->setSize(voice_w, btn_h);
        _btn_voice->align(LV_ALIGN_BOTTOM_RIGHT, -pad, -bottom);
    }

    void connect_if_possible(bool force_restart)
    {
        if (!GetHAL()->isWifiStaConnected()) {
            ui::pop_a_toast("Wi-Fi not connected. Use AP setup: 192.168.4.1", ui::toast_type::warning);
            if (_log) {
                _log->addText("! Wi-Fi not connected. Connect to AP and open http://192.168.4.1\n");
            }
            return;
        }

        if (!force_restart) {
            if (_irc.state() == net::IrcClient::Connecting || _irc.state() == net::IrcClient::Connected ||
                _irc.state() == net::IrcClient::Joined) {
                ui::pop_a_toast("IRC is already running", ui::toast_type::info);
                return;
            }
        }

        if (force_restart) {
            _irc.stop();
        }
        _irc.start(_irc_cfg);
        ui::pop_a_toast("IRC connecting...", ui::toast_type::info);
    }

    bool start_voice()
    {
        if (!GetHAL()->isWifiStaConnected()) {
            ui::pop_a_toast("Wi-Fi not connected", ui::toast_type::warning, 1800);
            return false;
        }

        net::BailianAsrPtt::Config cfg;
        cfg.api_key = TAB5_ENV_BAILIAN_API_KEY;  // from repo .env at build time
        cfg.model   = "fun-asr-realtime";
        cfg.max_sentence_silence = 1300;
        cfg.semantic_punctuation_enabled = false;

        auto guard = _asr_cb_guard;
        bool ok = _asr.start(
            cfg,
            [this, guard](const std::string& text) {
                if (!guard || !guard->load()) {
                    return;
                }
                std::lock_guard<std::mutex> lock(_asr_mutex);
                _asr_text_queue.push_back(text);
            },
            [this, guard](const std::string& err) {
                if (!guard || !guard->load()) {
                    return;
                }
                std::lock_guard<std::mutex> lock(_asr_mutex);
                _asr_error_queue.push_back(err);
            },
            [this, guard](net::BailianAsrPtt::State st) {
                if (!guard || !guard->load()) {
                    return;
                }
                _asr_state.store(st);
            });

        if (!ok) {
            ui::pop_a_toast("Voice start failed (check .env)", ui::toast_type::error, 2500);
            return false;
        }

        ui::pop_a_toast("Listening...", ui::toast_type::info, 1000);
        return true;
    }

    void stop_voice()
    {
        _asr.stop();
        ui::pop_a_toast("Voice stopped", ui::toast_type::gray, 1200);
    }

    static void e2e_voice_timer_cb(lv_timer_t* t)
    {
        auto* self = static_cast<IrcWindow*>(t ? lv_timer_get_user_data(t) : nullptr);
        if (!self) {
            return;
        }
        if (!TAB5_ENV_E2E_AUTORUN) {
            lv_timer_del(t);
            return;
        }
        if (self->_asr.running()) {
            lv_timer_del(t);
            return;
        }
        if (!GetHAL()->isWifiStaConnected()) {
            return;
        }
        // Prefer waiting for IRC join, but don't block forever.
        if (self->_irc.state() != net::IrcClient::Joined) {
            uint32_t now = GetHAL()->millis();
            if (now - self->_e2e_voice_wait_start_ms < 15000) {
                return;
            }
        }
        (void)self->start_voice();
        lv_timer_del(t);
    }

    void refresh_status()
    {
        if (GetHAL()->millis() - _last_status_ms < 300) {
            return;
        }
        _last_status_ms = GetHAL()->millis();

        std::string wifi = wifi_state_to_text(GetHAL()->getWifiState());
        std::string ip = GetHAL()->getWifiStaIp();
        std::string nick = _irc.current_nick();

        const char* irc_st = "Idle";
        switch (_irc.state()) {
        case net::IrcClient::Connecting:
            irc_st = "Connecting";
            break;
        case net::IrcClient::Connected:
            irc_st = "Connected";
            break;
        case net::IrcClient::Joined:
            irc_st = "Joined";
            break;
        case net::IrcClient::Disconnected:
            irc_st = "Disconnected";
            break;
        case net::IrcClient::Error:
            irc_st = "Error";
            break;
        default:
            break;
        }

        std::string line1 = "Wi-Fi: " + wifi + (ip.empty() ? "" : ("  ip=" + ip));
        std::string line2 = std::string("IRC: ") + irc_st + "  Nick: " + nick + "  #tab5";
        _status->setText(line1 + "\n" + line2);

        if (_btn_connect) {
            const char* label = "Connect";
            switch (_irc.state()) {
            case net::IrcClient::Connecting:
                label = "Connecting";
                break;
            case net::IrcClient::Connected:
            case net::IrcClient::Joined:
                label = "Connected";
                break;
            case net::IrcClient::Error:
            case net::IrcClient::Disconnected:
                label = "Retry";
                break;
            default:
                break;
            }
            _btn_connect->label().setText(label);
        }
    }

    int _last_layout_w = -1;
    int _last_layout_h = -1;
    uint32_t _last_status_ms = 0;
    std::unique_ptr<Label> _status;
    std::unique_ptr<TextArea> _log;
    std::unique_ptr<Button> _btn_rotate;
    std::unique_ptr<Button> _btn_connect;
    std::unique_ptr<Button> _btn_send_test;
    std::unique_ptr<Button> _btn_voice;

    net::IrcClient _irc;
    net::IrcClient::Config _irc_cfg;

    net::BailianAsrPtt _asr;
    std::shared_ptr<std::atomic<bool>> _asr_cb_guard;
    std::atomic<net::BailianAsrPtt::State> _asr_state;
    net::BailianAsrPtt::State _last_asr_state = net::BailianAsrPtt::State::Idle;
    std::mutex _asr_mutex;
    std::deque<std::string> _asr_text_queue;
    std::deque<std::string> _asr_error_queue;

    lv_obj_t* _parent = nullptr;
    lv_display_t* _disp = nullptr;
    lv_display_rotation_t _prev_rotation = LV_DISPLAY_ROTATION_0;
    bool _restore_rotation_on_close = false;

    uint32_t _e2e_voice_wait_start_ms = 0;
};

void PanelIrc::init()
{
    _btn_irc = std::make_unique<Container>(lv_screen_active());
    _btn_irc->align(LV_ALIGN_CENTER, 610, 300);
    _btn_irc->setSize(110, 110);
    _btn_irc->setRadius(24);
    _btn_irc->setBorderWidth(0);
    _btn_irc->setBgColor(lv_color_hex(0x2D5BFF));
    _btn_irc->setBgOpa(LV_OPA_80);

    _label_irc = std::make_unique<Label>(_btn_irc->get());
    _label_irc->setAlign(LV_ALIGN_CENTER);
    _label_irc->setText("IRC");
    _label_irc->setTextFont(&lv_font_montserrat_22);
    _label_irc->setTextColor(lv_color_hex(0xFFFFFF));
    _btn_irc->onClick().connect([&] {
        audio::play_next_tone_progression();
        _window = std::make_unique<IrcWindow>();
        _window->init(lv_screen_active());
        _window->open();
    });

    if (TAB5_ENV_E2E_AUTORUN) {
        // Delay a bit so launcher finishes its init/animation, then "click" IRC automatically.
        lv_timer_t* tm = lv_timer_create(e2e_click_obj_once_cb, 1200, _btn_irc->raw_ptr());
        if (tm) {
            lv_timer_set_repeat_count(tm, 1);
        }
    }
}

void PanelIrc::update(bool)
{
    if (_window) {
        _window->update();
        if (_window->getState() == ui::Window::State_t::Closed) {
            _window.reset();
        }
    }
}
