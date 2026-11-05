

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "sf2/bytes.hpp"

namespace sf2::crypto {

using Digest = std::array<uint8_t, 32>;

class Sha256 {
  public:
    Sha256() {
        reset();
    }

    void reset() noexcept;
    void update(const uint8_t *p, size_t n) noexcept;

    void update(ByteView v) noexcept {
        update(v.data(), v.size());
    }

    void update(std::string_view s) noexcept {
        update(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    Digest final_() noexcept;

  private:
    void block(const uint8_t *p) noexcept;
    uint32_t h_[8]{};
    uint8_t buf_[64]{};
    size_t len_ = 0;
    uint64_t total_ = 0;
};

Digest sha256(ByteView v) noexcept;
Digest hmac_sha256(ByteView key, ByteView msg) noexcept;

std::array<uint8_t, 16> md5(ByteView v) noexcept;
std::string md5_hex(std::string_view s);

Digest pbkdf2(std::string_view password, ByteView salt, uint32_t iterations) noexcept;

void random_bytes(uint8_t *out, size_t n);
std::string random_hex(size_t nbytes);

std::string to_hex(ByteView v);
bool from_hex(std::string_view hex, std::vector<uint8_t> &out);

bool const_time_eq(ByteView a, ByteView b) noexcept;

}
