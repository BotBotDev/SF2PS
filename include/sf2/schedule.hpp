#pragma once
#include <cstdint>
#include <string>

namespace sf2 {

bool cron_next_ms(const std::string &cron, int64_t tz_offset_s, int64_t from_utc_ms,
                  int64_t &out_ms);
}
