

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "sf2/bytes.hpp"

namespace sf2::pb {

constexpr size_t varint_size(uint64_t v) noexcept {
    size_t n = 1;
    while (v > 0x7F) {
        v >>= 7;
        ++n;
    }
    return n;
}

inline void varint(Buffer &b, uint64_t v) {
    while (v > 0x7F) {
        b.u8(uint8_t((v & 0x7F) | 0x80));
        v >>= 7;
    }
    b.u8(uint8_t(v));
}

inline void varint_at(Buffer &b, size_t off, uint64_t v, size_t width) {
    for (size_t k = 0; k < width; ++k) {
        uint8_t byte = uint8_t(v & 0x7F);
        v >>= 7;
        b[off + k] = uint8_t(byte | (k + 1 < width ? 0x80 : 0x00));
    }
}

inline void tag(Buffer &b, uint32_t field, uint32_t wire) {
    varint(b, (uint64_t(field) << 3) | wire);
}

inline void v(Buffer &b, uint32_t field, uint64_t value) {
    tag(b, field, 0);
    varint(b, value);
}

inline void bin(Buffer &b, uint32_t field, ByteView data) {
    tag(b, field, 2);
    varint(b, data.size());
    b.raw(data);
}

inline void str(Buffer &b, uint32_t field, std::string_view s) {
    tag(b, field, 2);
    varint(b, s.size());
    b.str(s);
}

inline void fixed32(Buffer &b, uint32_t field, uint32_t value) {
    tag(b, field, 5);
    for (int i = 0; i < 4; ++i)
        b.u8(uint8_t(value >> (8 * i)));
}

inline void fixed64(Buffer &b, uint32_t field, uint64_t value) {
    tag(b, field, 1);
    for (int i = 0; i < 8; ++i)
        b.u8(uint8_t(value >> (8 * i)));
}

class Sub {
  public:
    Sub(Buffer &b, uint32_t field) : b_(b) {
        tag(b_, field, 2);
        off_ = b_.size();
        b_.u8(0);
    }

    Sub(const Sub &) = delete;
    Sub &operator=(const Sub &) = delete;

    ~Sub() {
        close();
    }

    void close() {
        if (closed_)
            return;
        closed_ = true;
        const size_t body = b_.size() - off_ - 1;
        const size_t want = varint_size(body);
        if (want > 1)
            b_.vec().insert(b_.vec().begin() + long(off_) + 1, want - 1, 0);
        varint_at(b_, off_, body, want);
    }

  private:
    Buffer &b_;
    size_t off_ = 0;
    bool closed_ = false;
};

enum class Wire : uint8_t { Varint = 0, Fixed64 = 1, Bytes = 2, Fixed32 = 5 };

struct Field {
    uint32_t number = 0;
    Wire wire = Wire::Varint;
    uint64_t num = 0;
    ByteView data;
};

bool read_varint(Cursor &c, uint64_t &out) noexcept;

void scan(ByteView msg, const std::function<bool(const Field &)> &fn) noexcept;

uint64_t get_varint(ByteView msg, uint32_t field, uint64_t dflt = 0) noexcept;
ByteView get_bytes(ByteView msg, uint32_t field) noexcept;
std::string_view get_string(ByteView msg, uint32_t field) noexcept;
bool has_field(ByteView msg, uint32_t field) noexcept;

size_t get_repeated_varints(ByteView msg, uint32_t field, std::vector<int64_t> &out,
                            size_t max) noexcept;

}
