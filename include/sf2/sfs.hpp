

#pragma once

#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sf2/bytes.hpp"

namespace sf2::sfs {

enum class Type : uint8_t {
    Null = 0x00,
    Bool = 0x01,
    Byte = 0x02,
    Short = 0x03,
    Int = 0x04,
    Long = 0x05,
    Float = 0x06,
    Double = 0x07,
    Utf = 0x08,
    BoolArray = 0x09,
    ByteArray = 0x0a,
    ShortArray = 0x0b,
    IntArray = 0x0c,
    LongArray = 0x0d,
    FloatArray = 0x0e,
    DoubleArray = 0x0f,
    UtfArray = 0x10,
    Array = 0x11,
    Object = 0x12,
    Class = 0x13,
    Text = 0x14,
};

const char *type_name(Type t) noexcept;

struct Limits {
    uint32_t max_depth = 12;
    uint32_t max_object_keys = 512;
    uint32_t max_array_items = 4096;
    uint32_t max_string_bytes = 64 * 1024;
    uint32_t max_binary_bytes = 1024 * 1024;
    uint32_t max_total_values = 20000;
    bool allow_class = false;
};

struct Value;

struct Agg {
    std::vector<uint8_t> bin;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;
};

struct Value {
    Type t = Type::Null;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::shared_ptr<Agg> agg;

    Value() = default;

    explicit Value(Type ty) : t(ty) {}

    bool is_null() const noexcept {
        return t == Type::Null;
    }

    bool is_int() const noexcept {
        return t == Type::Bool || t == Type::Byte || t == Type::Short || t == Type::Int ||
               t == Type::Long;
    }

    bool is_string() const noexcept {
        return t == Type::Utf || t == Type::Text;
    }

    bool is_binary() const noexcept {
        return t == Type::ByteArray;
    }

    bool is_object() const noexcept {
        return t == Type::Object;
    }

    bool is_array() const noexcept {
        return t >= Type::BoolArray && t <= Type::Array;
    }

    int64_t as_int(int64_t dflt = 0) const noexcept {
        if (is_int())
            return i;
        if (t == Type::Float || t == Type::Double)
            return int64_t(d);
        return dflt;
    }

    std::string_view as_str(std::string_view dflt = {}) const noexcept {
        return is_string() ? std::string_view(s) : dflt;
    }

    ByteView as_bin() const noexcept {
        if (t == Type::ByteArray && agg)
            return ByteView(agg->bin);
        return {};
    }

    const std::vector<Value> &items() const noexcept {
        static const std::vector<Value> kEmpty;
        return agg ? agg->arr : kEmpty;
    }

    size_t size() const noexcept {
        if (!agg)
            return 0;
        if (is_object())
            return agg->obj.size();
        if (is_array())
            return agg->arr.size();
        if (is_binary())
            return agg->bin.size();
        return 0;
    }

    const Value *find(std::string_view key) const noexcept {
        if (t != Type::Object || !agg)
            return nullptr;
        for (const auto &kv : agg->obj)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }

    int64_t geti(std::string_view k, int64_t dflt = 0) const noexcept {
        const Value *v = find(k);
        return v ? v->as_int(dflt) : dflt;
    }

    std::string_view gets(std::string_view k, std::string_view dflt = {}) const noexcept {
        const Value *v = find(k);
        return v ? v->as_str(dflt) : dflt;
    }

    ByteView getb(std::string_view k) const noexcept {
        const Value *v = find(k);
        return v ? v->as_bin() : ByteView{};
    }

    const Value *geto(std::string_view k) const noexcept {
        const Value *v = find(k);
        return (v && v->is_object()) ? v : nullptr;
    }
};

Value decode_value(Cursor &c, const Limits &lim);

Value decode_object_body(Cursor &c, const Limits &lim);

void encode_value(Buffer &out, const Value &v);

namespace enc {

inline void null_(Buffer &b) {
    b.u8(0x00);
}

inline void boolean(Buffer &b, bool v) {
    b.u8(0x01);
    b.u8(v ? 1 : 0);
}

inline void byte_(Buffer &b, int v) {
    b.u8(0x02);
    b.u8(uint8_t(v));
}

inline void short_(Buffer &b, int v) {
    b.u8(0x03);
    b.u16(uint16_t(int16_t(v)));
}

inline void int_(Buffer &b, int32_t v) {
    b.u8(0x04);
    b.u32(uint32_t(v));
}

inline void long_(Buffer &b, int64_t v) {
    b.u8(0x05);
    b.u64(uint64_t(v));
}

inline void float_(Buffer &b, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    b.u8(0x06);
    b.u32(bits);
}

inline void double_(Buffer &b, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    b.u8(0x07);
    b.u64(bits);
}

void str(Buffer &b, std::string_view s);
void text(Buffer &b, std::string_view s);
void bytes(Buffer &b, ByteView v);
void key(Buffer &b, std::string_view k);

}

class ObjWriter {
  public:
    explicit ObjWriter(Buffer &b) : b_(b) {
        b_.u8(uint8_t(Type::Object));
        off_ = b_.size();
        b_.u16(0);
    }

    ObjWriter(const ObjWriter &) = delete;
    ObjWriter &operator=(const ObjWriter &) = delete;

    ~ObjWriter() {
        close();
    }

    void close() noexcept {
        if (!closed_) {
            b_.patch_u16(off_, n_);
            closed_ = true;
        }
    }

    ObjWriter &null_(std::string_view k) {
        field(k);
        enc::null_(b_);
        return *this;
    }

    ObjWriter &boolean(std::string_view k, bool v) {
        field(k);
        enc::boolean(b_, v);
        return *this;
    }

    ObjWriter &byte_(std::string_view k, int v) {
        field(k);
        enc::byte_(b_, v);
        return *this;
    }

    ObjWriter &short_(std::string_view k, int v) {
        field(k);
        enc::short_(b_, v);
        return *this;
    }

    ObjWriter &int_(std::string_view k, int32_t v) {
        field(k);
        enc::int_(b_, v);
        return *this;
    }

    ObjWriter &long_(std::string_view k, int64_t v) {
        field(k);
        enc::long_(b_, v);
        return *this;
    }

    ObjWriter &str(std::string_view k, std::string_view v) {
        field(k);
        enc::str(b_, v);
        return *this;
    }

    ObjWriter &bytes(std::string_view k, ByteView v) {
        field(k);
        enc::bytes(b_, v);
        return *this;
    }

    [[nodiscard]] ObjWriter object(std::string_view k) {
        field(k);
        return ObjWriter(b_);
    }

    void begin_array(std::string_view k, uint16_t count) {
        field(k);
        b_.u8(uint8_t(Type::Array));
        b_.u16(count);
    }

    Buffer &buf() noexcept {
        return b_;
    }

  private:
    void field(std::string_view k) {
        enc::key(b_, k);
        ++n_;
    }

    Buffer &b_;
    size_t off_ = 0;
    uint16_t n_ = 0;
    bool closed_ = false;
};

}
