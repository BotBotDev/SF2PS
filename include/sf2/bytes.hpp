

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sf2 {

class ByteView {
  public:
    ByteView() = default;

    ByteView(const uint8_t *p, size_t n) : p_(p), n_(n) {}

    explicit ByteView(std::string_view s)
        : p_(reinterpret_cast<const uint8_t *>(s.data())), n_(s.size()) {}

    ByteView(const std::vector<uint8_t> &v) : p_(v.data()), n_(v.size()) {}

    const uint8_t *data() const noexcept {
        return p_;
    }

    size_t size() const noexcept {
        return n_;
    }

    bool empty() const noexcept {
        return n_ == 0;
    }

    uint8_t operator[](size_t i) const noexcept {
        return p_[i];
    }

    ByteView sub(size_t off, size_t len) const noexcept {
        if (off > n_)
            return {};
        return ByteView(p_ + off, std::min(len, n_ - off));
    }

    std::string_view sv() const noexcept {
        return {reinterpret_cast<const char *>(p_), n_};
    }

    std::vector<uint8_t> vec() const {
        return {p_, p_ + n_};
    }

    bool operator==(const ByteView &o) const noexcept {
        return n_ == o.n_ && (n_ == 0 || std::memcmp(p_, o.p_, n_) == 0);
    }

  private:
    const uint8_t *p_ = nullptr;
    size_t n_ = 0;
};

class Buffer {
  public:
    Buffer() = default;

    explicit Buffer(size_t reserve) {
        b_.reserve(reserve);
    }

    void clear() noexcept {
        b_.clear();
    }

    void reserve(size_t n) {
        b_.reserve(n);
    }

    size_t size() const noexcept {
        return b_.size();
    }

    bool empty() const noexcept {
        return b_.empty();
    }

    const uint8_t *data() const noexcept {
        return b_.data();
    }

    uint8_t *data() noexcept {
        return b_.data();
    }

    uint8_t &operator[](size_t i) noexcept {
        return b_[i];
    }

    const uint8_t &operator[](size_t i) const noexcept {
        return b_[i];
    }

    std::vector<uint8_t> &vec() noexcept {
        return b_;
    }

    const std::vector<uint8_t> &vec() const noexcept {
        return b_;
    }

    ByteView view() const noexcept {
        return ByteView(b_.data(), b_.size());
    }

    std::vector<uint8_t> take() {
        return std::move(b_);
    }

    void u8(uint8_t v) {
        b_.push_back(v);
    }

    void u16(uint16_t v) {
        b_.push_back(uint8_t(v >> 8));
        b_.push_back(uint8_t(v));
    }

    void u32(uint32_t v) {
        b_.push_back(uint8_t(v >> 24));
        b_.push_back(uint8_t(v >> 16));
        b_.push_back(uint8_t(v >> 8));
        b_.push_back(uint8_t(v));
    }

    void u64(uint64_t v) {
        for (int s = 56; s >= 0; s -= 8)
            b_.push_back(uint8_t(v >> s));
    }

    void raw(const void *p, size_t n) {
        if (!n)
            return;
        const uint8_t *q = static_cast<const uint8_t *>(p);
        b_.insert(b_.end(), q, q + n);
    }

    void raw(ByteView v) {
        raw(v.data(), v.size());
    }

    void raw(const Buffer &o) {
        raw(o.data(), o.size());
    }

    void str(std::string_view s) {
        raw(s.data(), s.size());
    }

    void patch_u16(size_t off, uint16_t v) noexcept {
        b_[off] = uint8_t(v >> 8);
        b_[off + 1] = uint8_t(v);
    }

    void patch_u32(size_t off, uint32_t v) noexcept {
        b_[off] = uint8_t(v >> 24);
        b_[off + 1] = uint8_t(v >> 16);
        b_[off + 2] = uint8_t(v >> 8);
        b_[off + 3] = uint8_t(v);
    }

  private:
    std::vector<uint8_t> b_;
};

inline uint16_t be16(const uint8_t *p) noexcept {
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}

inline uint32_t be32(const uint8_t *p) noexcept {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

inline uint64_t be64(const uint8_t *p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | p[i];
    return v;
}

struct DecodeError : std::runtime_error {
    explicit DecodeError(const std::string &what) : std::runtime_error(what) {}
};

class Cursor {
  public:
    Cursor(ByteView v) : v_(v) {}

    size_t pos() const noexcept {
        return i_;
    }

    size_t left() const noexcept {
        return v_.size() - i_;
    }

    bool eof() const noexcept {
        return i_ >= v_.size();
    }

    ByteView all() const noexcept {
        return v_;
    }

    void need(size_t n) const {
        if (n > left())
            throw DecodeError("truncated input");
    }

    uint8_t u8() {
        need(1);
        return v_[i_++];
    }

    uint16_t u16() {
        need(2);
        uint16_t r = be16(v_.data() + i_);
        i_ += 2;
        return r;
    }

    uint32_t u32() {
        need(4);
        uint32_t r = be32(v_.data() + i_);
        i_ += 4;
        return r;
    }

    uint64_t u64() {
        need(8);
        uint64_t r = be64(v_.data() + i_);
        i_ += 8;
        return r;
    }

    int16_t i16() {
        return int16_t(u16());
    }

    int32_t i32() {
        return int32_t(u32());
    }

    int64_t i64() {
        return int64_t(u64());
    }

    ByteView take(size_t n) {
        need(n);
        ByteView r = v_.sub(i_, n);
        i_ += n;
        return r;
    }

    void skip(size_t n) {
        need(n);
        i_ += n;
    }

  private:
    ByteView v_;
    size_t i_ = 0;
};

}
