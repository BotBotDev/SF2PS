

#pragma once

#include <cstdint>

namespace sf2 {

class TokenBucket {
  public:
    TokenBucket() = default;

    TokenBucket(double rate_per_s, uint32_t burst) {
        configure(rate_per_s, burst);
    }

    void configure(double rate_per_s, uint32_t burst) noexcept {
        rate_ = rate_per_s;
        burst_ = double(burst ? burst : 1);
        tokens_ = burst_;
        last_ms_ = 0;
    }

    bool allow(uint64_t now_ms, double cost = 1.0) noexcept {
        if (last_ms_ == 0)
            last_ms_ = now_ms;
        if (now_ms > last_ms_) {
            tokens_ += rate_ * double(now_ms - last_ms_) / 1000.0;
            if (tokens_ > burst_)
                tokens_ = burst_;
            last_ms_ = now_ms;
        }
        if (tokens_ < cost)
            return false;
        tokens_ -= cost;
        return true;
    }

    double tokens() const noexcept {
        return tokens_;
    }

  private:
    double rate_ = 1.0;
    double burst_ = 1.0;
    double tokens_ = 1.0;
    uint64_t last_ms_ = 0;
};

}
