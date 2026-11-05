

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>

namespace sf2::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

extern std::atomic<int> g_level;

void set_level(Level l) noexcept;
bool parse_level(const std::string &s, Level &out) noexcept;
void write(Level l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void audit(const char *event, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void throttled(Level l, const char *key, uint64_t period_ms, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

}

#define SF2_LOG(lvl, ...)                                                                          \
    do {                                                                                           \
        if (int(lvl) >= ::sf2::log::g_level.load(std::memory_order_relaxed))                       \
            ::sf2::log::write(lvl, __VA_ARGS__);                                                   \
    } while (0)

#define LOG_TRACE(...) SF2_LOG(::sf2::log::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) SF2_LOG(::sf2::log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(...) SF2_LOG(::sf2::log::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) SF2_LOG(::sf2::log::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) SF2_LOG(::sf2::log::Level::Error, __VA_ARGS__)
