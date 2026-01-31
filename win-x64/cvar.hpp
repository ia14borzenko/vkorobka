#ifndef CVAR_HPP
#define CVAR_HPP

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>

#include <winsock2.h>
#include "tcp.hpp"
#include "netpack.h"

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

    const std::string& get(size_t index) const
    {
        static const std::string empty;
        if (index >= positional.size())
            return empty;
        return positional[index];
    }

    bool has_flag(const std::string& name) const
    {
        return flags.find(name) != flags.end();
    }

    const std::string& flag(const std::string& name) const
    {
        static const std::string empty;
        auto it = flags.find(name);
        if (it == flags.end())
            return empty;
        return it->second;
    }
};

/* ============================================================
   Base struct for command
   ============================================================ */

struct cvar_t
{
    std::string name;
    std::string description;

    cvar_t(const std::string& n, const std::string& d)
        : name(n), description(d)
    {
        registry()[name] = this;
    }

    virtual ~cvar_t() = default;

    virtual void exec(cctx_t& ctx) = 0;

    static std::map<std::string, cvar_t*>& registry()
    {
        static std::map<std::string, cvar_t*> inst;
        return inst;
    }
};

static void parse_line(const std::string& line,
    std::string& cmd,
    cctx_t& ctx)
{
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string t;

    while (iss >> t)
        tokens.push_back(t);

    if (tokens.empty())
        return;

    cmd = tokens[0];

    for (size_t i = 1; i < tokens.size(); ++i)
    {
        const std::string& tok = tokens[i];

        if (tok.rfind("--", 0) == 0)
        {
            std::string key = tok.substr(2);
            std::string val = "1";

            if (i + 1 < tokens.size() &&
                tokens[i + 1].rfind("--", 0) != 0)
            {
                val = tokens[i + 1];
                ++i;
            }

            ctx.flags[key] = val;
        }
        else
        {
            ctx.positional.push_back(tok);
        }
    }
}

/* ============================================================
   command help
   ============================================================ */

struct help_t : cvar_t
{
    help_t() : cvar_t("help", "show command list") {}

    void exec(cctx_t&) override
    {
        std::cout << "Command list:\n";
        for (const auto& kv : registry())
        {
            std::cout << "  " << kv.first
                << " � " << kv.second->description << "\n";
        }
    }
};

/*
 ============================================================
   user command example
 ============================================================ 
*/

struct send_t : cvar_t
{
    send_t() : cvar_t("send", "sending data (raw or packet format: send --cmd <code> <data>)") {}

    void exec(cctx_t& ctx) override
    {
        if (!ctx.tcp)
        {
            std::cout << ANSI_ERR << "TCP not initialized" << ANSI_ENDL << std::endl;
            return;
        }

        if (!ctx.tcp->is_connected())
        {
            std::cout << ANSI_WARN << "TCP not connected" << ANSI_ENDL << std::endl;
            return;
        }

        // Если указан флаг --cmd, отправляем пакет в формате CMD
        if (ctx.has_flag("cmd"))
        {
            std::string cmd_str = ctx.flag("cmd");
            cmdcode_t cmd_code = CMD_RESERVED;
            
            // Парсим код команды (может быть hex: 0x41 или decimal: 65)
            try
            {
                if (cmd_str.substr(0, 2) == "0x" || cmd_str.substr(0, 2) == "0X")
                {
                    cmd_code = static_cast<cmdcode_t>(std::stoul(cmd_str, nullptr, 16));
                }
                else
                {
                    cmd_code = static_cast<cmdcode_t>(std::stoul(cmd_str, nullptr, 10));
                }
            }
            catch (...)
            {
                std::cout << ANSI_ERR << "Invalid command code format. Use hex (0x41) or decimal (65)" << ANSI_ENDL << std::endl;
                return;
            }

            std::string data = ctx.get(0);
            if (data.empty())
            {
                std::cout << ANSI_WARN << "No data to send. Usage: send --cmd <code> <data>" << ANSI_ENDL << std::endl;
                return;
            }

            if (!ctx.tcp->send_packet(cmd_code, data))
            {
                std::cout << ANSI_ERR << "Packet send failed" << ANSI_ENDL << std::endl;
            }
            else
            {
                std::cout << ANSI_SUCC << "Packet sent: CMD=0x" << std::hex << cmd_code 
                          << std::dec << ", Data=" << data << ANSI_ENDL << std::endl;
            }
        }
        else
        {
            // Отправка raw данных (без упаковки)
            std::string data = ctx.get(0);
            if (data.empty())
            {
                std::cout << ANSI_WARN << "No data to send. Usage: send <data> or send --cmd <code> <data>" << ANSI_ENDL << std::endl;
                return;
            }

            if (!ctx.tcp->send(data))
            {
                std::cout << ANSI_ERR << "Send failed" << ANSI_ENDL << std::endl;
            }
            else
            {
                std::cout << ANSI_SUCC << "Data sent: " << data << ANSI_ENDL << std::endl;
            }
        }
    }
};


#endif