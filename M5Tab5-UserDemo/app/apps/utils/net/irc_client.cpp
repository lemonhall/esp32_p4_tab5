/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "irc_client.h"
#include <mooncake_log.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#if !defined(ESP_PLATFORM)
#include <thread>
#endif

#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_mac.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static const std::string _tag = "irc";

namespace {

static void normalize_crlf(std::string& s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
}

static bool contains_token(const std::string& line, const char* token)
{
    return line.find(token) != std::string::npos;
}

static std::string format_display_line(const std::string& raw)
{
    // Minimal: try parse PRIVMSG
    // :nick!user@host PRIVMSG #chan :message
    if (!raw.empty() && raw[0] == ':') {
        auto ex = raw.find('!');
        auto sp = raw.find(' ');
        if (ex != std::string::npos && sp != std::string::npos && ex < sp) {
            std::string nick = raw.substr(1, ex - 1);
            if (contains_token(raw, " PRIVMSG ")) {
                auto msg_pos = raw.find(" :");
                if (msg_pos != std::string::npos) {
                    std::string msg = raw.substr(msg_pos + 2);
                    return nick + ": " + msg;
                }
            }
        }
    }
    return raw;
}

static std::string suffix36(uint32_t idx)
{
    static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    return std::string(1, digits[idx % 36]);
}

}  // namespace

net::IrcClient::IrcClient()
    : _state(Idle)
    , _stop_requested(false)
    , _sock(-1)
    , _join_sent(false)
    , _nick_attempt(0)
#if defined(ESP_PLATFORM)
    , _task_handle(nullptr)
#else
    , _thread_handle(nullptr)
#endif
{
}

net::IrcClient::~IrcClient()
{
    stop();
}

void net::IrcClient::start(Config cfg)
{
    stop();
    _cfg = std::move(cfg);
    _current_nick = _cfg.nick;
    _join_sent = false;
    _nick_attempt = 0;
    _stop_requested = false;
    _state = Connecting;

#if defined(ESP_PLATFORM)
    auto thunk = [](void* arg) {
        static_cast<IrcClient*>(arg)->run();
        vTaskDelete(nullptr);
    };
    xTaskCreate(thunk, "irc", 8192, this, 5, (TaskHandle_t*)&_task_handle);
#else
    auto* t = new std::thread([this]() { run(); });
    _thread_handle = t;
#endif
}

void net::IrcClient::stop()
{
    _stop_requested = true;
    close_socket();

#if defined(ESP_PLATFORM)
    _task_handle = nullptr;
#else
    if (_thread_handle) {
        auto* t = static_cast<std::thread*>(_thread_handle);
        if (t->joinable()) {
            t->join();
        }
        delete t;
        _thread_handle = nullptr;
    }
#endif

    _state = Idle;
}

net::IrcClient::State net::IrcClient::state() const
{
    return _state.load();
}

std::string net::IrcClient::current_nick() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _current_nick;
}

void net::IrcClient::send_privmsg(const std::string& target, const std::string& msg)
{
    if (_state != Joined && _state != Connected) {
        return;
    }
    if (target.empty()) {
        return;
    }
    std::string line = "PRIVMSG " + target + " :" + msg;
    if (send_raw(line)) {
        push_line(">> " + line);
        mclog::tagInfo(_tag, "{}", line);
    }
}

bool net::IrcClient::poll_line(std::string& out)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lines.empty()) {
        return false;
    }
    out = std::move(_lines.front());
    _lines.pop_front();
    return true;
}

void net::IrcClient::push_line(const std::string& line)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lines.size() > 256) {
        _lines.pop_front();
    }
    _lines.push_back(line);
}

std::string net::IrcClient::make_default_nick_9()
{
#if defined(ESP_PLATFORM)
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hex6[7] = {0};
    std::snprintf(hex6, sizeof(hex6), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    // 2 + 6 + 1 = 9
    return std::string("t5") + hex6 + "0";
#else
    // 9 chars best-effort fallback
    return "t5desktop0";
#endif
}

bool net::IrcClient::connect_socket()
{
    close_socket();

    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", (unsigned)_cfg.port);
    int err = getaddrinfo(_cfg.host.c_str(), port_str, &hints, &res);
    if (err != 0 || !res) {
        push_line("! getaddrinfo failed");
        return false;
    }

    _sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (_sock < 0) {
        freeaddrinfo(res);
        push_line("! socket failed");
        return false;
    }

    if (connect(_sock, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        push_line("! connect failed");
        close_socket();
        return false;
    }

    freeaddrinfo(res);
    _state = Connected;
    push_line("* connected");
    mclog::tagInfo(_tag, "connected {}:{}", _cfg.host, _cfg.port);
    return true;
}

void net::IrcClient::close_socket()
{
    if (_sock >= 0) {
#if defined(ESP_PLATFORM)
        shutdown(_sock, SHUT_RDWR);
        close(_sock);
#else
        shutdown(_sock, SHUT_RDWR);
        ::close(_sock);
#endif
        _sock = -1;
    }
}

bool net::IrcClient::send_raw(const std::string& line)
{
    if (_sock < 0) {
        return false;
    }
    std::string buf = line + "\r\n";
    int sent = ::send(_sock, buf.data(), (int)buf.size(), 0);
    return sent == (int)buf.size();
}

void net::IrcClient::handle_nick_in_use(const std::string& line)
{
    if (!contains_token(line, " 433 ")) {
        return;
    }

    _nick_attempt++;
    if (_nick_attempt >= 36) {
        push_line("! nick in use (give up)");
        _state = Error;
        return;
    }

    std::string base = _cfg.nick;
    if (base.size() < 9) {
        base.resize(9, '0');
    }
    base.resize(9);
    base[8] = suffix36(_nick_attempt)[0];

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _current_nick = base;
    }

    send_raw("NICK " + base);
    push_line("* nick retry " + base);
    mclog::tagWarn(_tag, "nick in use, retry {}", base);
}

void net::IrcClient::maybe_join(const std::string& line)
{
    if (_join_sent) {
        return;
    }

    if (contains_token(line, " 001 ") || contains_token(line, " 376 ") || contains_token(line, " 422 ")) {
        if (!_cfg.auto_join_channel.empty()) {
            send_raw("JOIN " + _cfg.auto_join_channel);
            push_line(">> JOIN " + _cfg.auto_join_channel);
            mclog::tagInfo(_tag, "join {}", _cfg.auto_join_channel);
            _join_sent = true;
        }
    }
}

void net::IrcClient::handle_line(const std::string& line)
{
    if (line.rfind("PING ", 0) == 0 || line.rfind("PING:", 0) == 0) {
        std::string token = line.substr(4);
        normalize_crlf(token);
        send_raw("PONG" + token);
        push_line("<< " + line);
        return;
    }

    handle_nick_in_use(line);
    maybe_join(line);

    if (contains_token(line, " JOIN ")) {
        if (_cfg.auto_join_channel.empty() || contains_token(line, _cfg.auto_join_channel.c_str())) {
            _state = Joined;
        }
    }

    push_line(format_display_line(line));
}

void net::IrcClient::run()
{
    push_line("* connecting " + _cfg.host + ":" + std::to_string(_cfg.port));
    mclog::tagInfo(_tag, "start nick={}", _current_nick);

    if (!connect_socket()) {
        _state = Error;
        return;
    }

    send_raw("NICK " + _current_nick);
    send_raw("USER " + _cfg.user + " 0 * :" + _cfg.realname);
    push_line(">> NICK/USER");

    std::string recv_buf;
    recv_buf.reserve(2048);

    while (!_stop_requested) {
        char tmp[256];
        int r = recv(_sock, tmp, sizeof(tmp) - 1, 0);
        if (r <= 0) {
            _state = Disconnected;
            push_line("! disconnected");
            break;
        }
        tmp[r] = '\0';
        recv_buf.append(tmp, tmp + r);

        size_t pos = 0;
        while (true) {
            size_t eol = recv_buf.find("\r\n", pos);
            if (eol == std::string::npos) {
                recv_buf.erase(0, pos);
                break;
            }
            std::string line = recv_buf.substr(pos, eol - pos);
            pos = eol + 2;
            handle_line(line);
        }
    }

    close_socket();
}
