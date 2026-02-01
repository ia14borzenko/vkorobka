#ifndef CVAR_HPP
#define CVAR_HPP

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>

#include <winsock2.h>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>
#include "tcp.hpp"
#include "wifi.hpp"
#include "netpack.h"

// Внешние переменные для синхронизации с командой test_esp32 (определены в main.cpp)
extern std::mutex g_response_mutex;
extern std::condition_variable g_response_cv;
extern std::atomic<bool> g_test_response_received;
extern std::atomic<bool> g_status_response_received;
extern std::string g_esp32_status_response;

#define ANSI_FG_PREFIX "3"
#define ANSI_FG_BRIGHT_PREFIX "9"
#define ANSI_BG_PREFIX "4"
#define ANSI_BG_BRIGHT_PREFIX "10"

#define ANSI_BLACK   "0"
#define ANSI_RED     "1"
#define ANSI_GREEN   "2"
#define ANSI_YELLOW  "3"
#define ANSI_BLUE    "4"
#define ANSI_MAGENTA "5"
#define ANSI_CYAN    "6"
#define ANSI_WHITE   "7"


#define ANSI_ESC "\x1B["
#define ANSI_END "m"

#define ANSI_ENDL ANSI_ESC "0" ANSI_END
#define ANSI_COLOR(fg, bg) ANSI_ESC ANSI_FG_PREFIX fg ";" ANSI_BG_PREFIX bg ANSI_END

#define ANSI_INFO ANSI_COLOR(ANSI_BLUE, ANSI_BLACK)
#define ANSI_WARN ANSI_COLOR(ANSI_YELLOW, ANSI_BLACK)
#define ANSI_ERR ANSI_COLOR(ANSI_RED, ANSI_WHITE)
#define ANSI_SUCC ANSI_COLOR(ANSI_GREEN, ANSI_BLACK)

/* ============================================================
   Context of console input
   ============================================================ */

struct cctx_t
{
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;
    tcp_t* tcp;  // Указатель на TCP объект для отправки данных
    wifi_t* wifi;  // Указатель на WiFi объект для получения состояния

    const std::string& get(size_t index) const;
    bool has_flag(const std::string& name) const;
    const std::string& flag(const std::string& name) const;
};

/* ============================================================
   Base struct for command
   ============================================================ */

struct cvar_t
{
    std::string name;
    std::string description;

    cvar_t(const std::string& n, const std::string& d);
    virtual ~cvar_t() = default;

    virtual void exec(cctx_t& ctx) = 0;

    static std::map<std::string, cvar_t*>& registry();
};

void parse_line(const std::string& line,
    std::string& cmd,
    cctx_t& ctx);

/* ============================================================
   command help
   ============================================================ */

struct help_t : cvar_t
{
    help_t();
    void exec(cctx_t&) override;
};

/*
 ============================================================
   user command example
 ============================================================ 
*/

struct send_t : cvar_t
{
    send_t();
    void exec(cctx_t& ctx) override;
};

/* ============================================================
   command status
   ============================================================ */

struct status_t : cvar_t
{
    status_t();
    void exec(cctx_t& ctx) override;
};

/* ============================================================
   command test_esp32
   ============================================================ */

struct test_esp32_t : cvar_t
{
    test_esp32_t();
    void exec(cctx_t& ctx) override;
};


#endif
