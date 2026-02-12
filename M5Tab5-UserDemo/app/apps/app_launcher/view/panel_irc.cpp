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
#include <apps/utils/net/irc_client.h>
#include <apps/utils/ui/toast.h>
#include <apps/utils/ui/window.h>
#include <hal/hal.h>
#include <lvgl.h>
#include <memory>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mooncake_log.h>
#include <smooth_ui_toolkit.h>
#include <smooth_lvgl.h>

using namespace launcher_view;
using namespace smooth_ui_toolkit;
using namespace smooth_ui_toolkit::lvgl_cpp;

static const std::string _tag = "panel-irc";

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

        _status = std::make_unique<Label>(_window->get());
        _status->setTextFont(&lv_font_montserrat_18);
        _status->setTextColor(lv_color_hex(0xDEDEDE));
        _status->setLongMode(LV_LABEL_LONG_CLIP);

        _log = std::make_unique<TextArea>(_window->get());
        _log->setMaxLength(8192);
        _log->setCursorClickPos(false);
        _log->setText("");
        _log->setPasswordMode(false);
        _log->setOneLine(false);
        _log->setBorderWidth(0);
        _log->setRadius(12);
        _log->setBgColor(lv_color_hex(0x2A2A2A));

        const lv_font_t* cn = fonts::get_sd_ttf_font(20);
        if (cn) {
            _log->setTextFont(cn);
        } else {
            _log->setTextFont(&lv_font_montserrat_18);
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
        _btn_send_test->label().setText("Send Test");
        _btn_send_test->onClick().connect([&] {
            audio::play_next_tone_progression();
            _irc.send_privmsg("#tab5", "hi from " + _irc.current_nick());
        });

        _btn_voice = std::make_unique<Button>(_window->get());
        _btn_voice->setBgColor(lv_color_hex(0x616161));
        _btn_voice->setRadius(16);
        _btn_voice->label().setTextFont(&lv_font_montserrat_22);
        _btn_voice->label().setTextColor(lv_color_hex(0xE7E7E7));
        _btn_voice->label().setText("Voice");
        _btn_voice->onClick().connect([&] {
            audio::play_next_tone_progression();
            ui::pop_a_toast("TODO: ASR voice input", ui::toast_type::info);
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
    }

    void onUpdate() override
    {
        apply_layout(false);

        if (_state != Opened) {
            return;
        }

        refresh_status();

        std::string line;
        while (_irc.poll_line(line)) {
            _log->addText(line + "\n");
        }
    }

    void onClose() override
    {
        audio::play_next_tone_progression();
        _irc.stop();
        _status.reset();
        _log.reset();
        _btn_connect.reset();
        _btn_send_test.reset();
        _btn_voice.reset();
    }

private:
    void apply_layout(bool force)
    {
        if (!_window || !_status || !_log || !_btn_connect || !_btn_send_test || !_btn_voice) {
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
        int status_h = 28;
        int btn_h = 56;
        int btn_gap = 14;
        int bottom = 16;

        _status->align(LV_ALIGN_TOP_LEFT, pad, top);
        _status->setSize(std::max(0, w - pad * 2), status_h);

        int log_y = top + status_h + 10;
        int log_h = std::max(120, h - log_y - bottom - btn_h);
        _log->align(LV_ALIGN_TOP_LEFT, pad, log_y);
        _log->setSize(std::max(0, w - pad * 2), std::max(0, log_h));

        _btn_connect->setSize(220, btn_h);
        _btn_connect->align(LV_ALIGN_BOTTOM_LEFT, pad, -bottom);

        _btn_send_test->setSize(320, btn_h);
        _btn_send_test->align(LV_ALIGN_BOTTOM_LEFT, pad + 220 + btn_gap, -bottom);

        _btn_voice->setSize(220, btn_h);
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

        _status->setText("Wi-Fi: " + wifi + (ip.empty() ? "" : (" ip=" + ip)) + " | IRC: " + irc_st +
                         " | Nick: " + nick + " | #tab5");

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
    std::unique_ptr<Button> _btn_connect;
    std::unique_ptr<Button> _btn_send_test;
    std::unique_ptr<Button> _btn_voice;

    net::IrcClient _irc;
    net::IrcClient::Config _irc_cfg;
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
