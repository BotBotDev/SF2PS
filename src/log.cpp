
#include "sf2/log.hpp"

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace sf2::log {

std::atomic<int> g_level{int(Level::Info)};

namespace {
std::mutex g_mu;

const char *tag(Level l) noexcept {
    switch (l) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO ";
    case Level::Warn:
        return "WARN ";
    case Level::Error:
        return "ERROR";
    default:
        return "?????";
    }
}

uint64_t now_ms() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

struct Throttle {
    uint64_t last = 0;
    uint64_t hidden = 0;
};

std::unordered_map<std::string, Throttle> g_throttle;
}

void set_level(Level l) noexcept {
    g_level.store(int(l), std::memory_order_relaxed);
}

bool parse_level(const std::string &s, Level &out) noexcept {
    if (s == "trace") {
        out = Level::Trace;
        return true;
    }
    if (s == "debug") {
        out = Level::Debug;
        return true;
    }
    if (s == "info") {
        out = Level::Info;
        return true;
    }
    if (s == "warn") {
        out = Level::Warn;
        return true;
    }
    if (s == "error") {
        out = Level::Error;
        return true;
    }
    if (s == "off") {
        out = Level::Off;
        return true;
    }
    return false;
}

void write(Level l, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tmv{};
    localtime_r(&ts.tv_sec, &tmv);
    char when[32];
    snprintf(when, sizeof when, "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             int(ts.tv_nsec / 1000000));

    std::lock_guard<std::mutex> lk(g_mu);
    fprintf(stderr, "%s %s %s\n", when, tag(l), msg);
}

void audit(const char *event, const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    std::string clean;
    for (const unsigned char c : std::string(msg)) {
        if (c < 32 || c == 127)
            clean += '?';
        else
            clean += char(c);
    }
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm utc{};
    gmtime_r(&ts.tv_sec, &utc);
    char date[32];
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &utc);
    char line[2304];
    int n = snprintf(line, sizeof line, "%s.%03dZ AUDIT event=%s %s\n", date,
                     int(ts.tv_nsec / 1000000), event, clean.c_str());
    if (n < 0)
        return;
    const size_t len = std::min(size_t(n), sizeof(line) - 1);
    std::lock_guard<std::mutex> lk(g_mu);
    static const std::string path = [] {
        const char *value = std::getenv("SF2_AUDIT_LOG");
        return value ? std::string(value) : std::string();
    }();
    static int fd = -1;
    static bool reported = false;
    if (path.empty()) {
        if (int(Level::Info) >= g_level.load(std::memory_order_relaxed))
            fwrite(line, 1, len, stderr);
        return;
    }
    if (fd >= 0) {
        struct stat st{};
        if (fstat(fd, &st) == 0 && st.st_size >= 50LL * 1024 * 1024) {
            close(fd);
            fd = -1;
            for (int i = 9; i >= 1; --i)
                rename((path + "." + std::to_string(i)).c_str(),
                       (path + "." + std::to_string(i + 1)).c_str());
            rename(path.c_str(), (path + ".1").c_str());
        }
    }
    if (fd < 0)
        fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    size_t off = 0;
    while (fd >= 0 && off < len) {
        const ssize_t wrote = ::write(fd, line + off, len - off);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            break;
        off += size_t(wrote);
    }
    if (off != len) {

        fwrite(line, 1, len, stderr);
        if (!reported)
            fprintf(stderr, "ERROR: audit file unavailable: %s\n", path.c_str());
        reported = true;
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    } else
        reported = false;
}

void throttled(Level l, const char *key, uint64_t period_ms, const char *fmt, ...) {
    if (int(l) < g_level.load(std::memory_order_relaxed))
        return;

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    uint64_t hidden = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Throttle &t = g_throttle[key];
        uint64_t now = now_ms();
        if (t.last != 0 && now - t.last < period_ms) {
            ++t.hidden;
            return;
        }
        hidden = t.hidden;
        t.hidden = 0;
        t.last = now;
    }
    if (hidden)
        write(l, "%s (+%llu suppressed)", msg, (unsigned long long)hidden);
    else
        write(l, "%s", msg);
}

}
