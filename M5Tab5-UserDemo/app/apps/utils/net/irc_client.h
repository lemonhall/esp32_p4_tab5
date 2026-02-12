/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace net {

class IrcClient {
public:
    struct Config {
        std::string host;
        uint16_t port = 6667;
        std::string nick;
        std::string user = "tab5";
        std::string realname = "Tab5";
        std::string auto_join_channel;
    };

    enum State {
        Idle = 0,
        Connecting,
        Connected,
        Joined,
        Disconnected,
        Error,
    };

    IrcClient();
    ~IrcClient();

    void start(Config cfg);
    void stop();

    State state() const;
    std::string current_nick() const;

    void send_privmsg(const std::string& target, const std::string& msg);

    bool poll_line(std::string& out);

    static std::string make_default_nick_9();

private:
    void push_line(const std::string& line);
    void run();

    bool connect_socket();
    void close_socket();
    bool send_raw(const std::string& line);
    void handle_line(const std::string& line);
    void maybe_join(const std::string& line);
    void handle_nick_in_use(const std::string& line);

    Config _cfg;

    mutable std::mutex _mutex;
    std::deque<std::string> _lines;
    std::string _current_nick;

    std::atomic<State> _state;
    std::atomic<bool> _stop_requested;

    int _sock;
    bool _join_sent;
    uint32_t _nick_attempt;

#if defined(ESP_PLATFORM)
    void* _task_handle;
#else
    void* _thread_handle;
#endif
};

}  // namespace net

