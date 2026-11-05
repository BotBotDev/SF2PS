#include "sf2/schedule.hpp"
#include <algorithm>
#include <ctime>
#include <string_view>
#include <vector>

namespace sf2 {
namespace {

bool cron_field(std::string_view f, int lo, int hi, std::vector<int> &out) {
    out.clear();
    if (f.empty())
        return false;
    size_t at = 0;
    while (at <= f.size()) {
        const size_t comma = f.find(',', at);
        std::string_view part =
            f.substr(at, comma == std::string_view::npos ? f.size() - at : comma - at);
        at = (comma == std::string_view::npos) ? f.size() + 1 : comma + 1;
        if (part.empty())
            return false;

        int step = 1;
        if (const size_t slash = part.find('/'); slash != std::string_view::npos) {
            step = 0;
            for (char c : part.substr(slash + 1)) {
                if (c < '0' || c > '9')
                    return false;
                step = step * 10 + (c - '0');
                if (step > 1000)
                    return false;
            }
            if (step <= 0)
                return false;
            part = part.substr(0, slash);
        }

        int from = lo, to = hi;
        if (part != "*") {
            const size_t dash = part.find('-');
            auto num = [](std::string_view t, int &v) {
                if (t.empty() || t.size() > 4)
                    return false;
                v = 0;
                for (char c : t) {
                    if (c < '0' || c > '9')
                        return false;
                    v = v * 10 + (c - '0');
                }
                return true;
            };
            if (dash == std::string_view::npos) {
                if (!num(part, from))
                    return false;
                to = from;
            } else {
                if (!num(part.substr(0, dash), from))
                    return false;
                if (!num(part.substr(dash + 1), to))
                    return false;
            }
        }
        if (from < lo || to > hi || from > to)
            return false;
        for (int v = from; v <= to; v += step)
            out.push_back(v);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return !out.empty();
}

}

bool cron_next_ms(const std::string &cron, int64_t tz_offset_s, int64_t from_utc_ms,
                  int64_t &out_ms) {
    std::vector<std::string_view> parts;
    std::string_view v(cron);
    for (size_t at = 0; at < v.size();) {
        while (at < v.size() && v[at] == ' ')
            ++at;
        const size_t end = v.find(' ', at);
        if (at < v.size())
            parts.push_back(v.substr(at, end == std::string_view::npos ? v.size() - at : end - at));
        at = (end == std::string_view::npos) ? v.size() : end + 1;
    }
    if (parts.size() != 5 || parts[3] != "*" || parts[4] != "*")
        return false;

    std::vector<int> minutes, hours, days;
    if (!cron_field(parts[0], 0, 59, minutes))
        return false;
    if (!cron_field(parts[1], 0, 23, hours))
        return false;
    if (!cron_field(parts[2], 1, 31, days))
        return false;

    const int64_t local_s = from_utc_ms / 1000 + tz_offset_s;
    std::time_t t = std::time_t(local_s);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif

    const int64_t day_start_local = local_s - (tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec);
    for (int day = 0; day <= 62; ++day) {
        const int64_t probe = day_start_local + int64_t(day) * 86400;
        std::time_t pt = std::time_t(probe);
        std::tm ptm{};
#if defined(_WIN32)
        gmtime_s(&ptm, &pt);
#else
        gmtime_r(&pt, &ptm);
#endif
        bool day_ok = false;
        for (int d : days)
            if (d == ptm.tm_mday) {
                day_ok = true;
                break;
            }
        if (!day_ok)
            continue;
        for (int h : hours) {
            for (int m : minutes) {
                const int64_t when = probe + h * 3600 + m * 60;
                if (when <= local_s)
                    continue;
                out_ms = (when - tz_offset_s) * 1000;
                return true;
            }
        }
    }
    return false;
}

}
