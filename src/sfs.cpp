
#include "sf2/sfs.hpp"

#include <stdexcept>

namespace sf2::sfs {

const char *type_name(Type t) noexcept {
    switch (t) {
    case Type::Null:
        return "NULL";
    case Type::Bool:
        return "BOOL";
    case Type::Byte:
        return "BYTE";
    case Type::Short:
        return "SHORT";
    case Type::Int:
        return "INT";
    case Type::Long:
        return "LONG";
    case Type::Float:
        return "FLOAT";
    case Type::Double:
        return "DOUBLE";
    case Type::Utf:
        return "UTF_STRING";
    case Type::BoolArray:
        return "BOOL_ARRAY";
    case Type::ByteArray:
        return "BYTE_ARRAY";
    case Type::ShortArray:
        return "SHORT_ARRAY";
    case Type::IntArray:
        return "INT_ARRAY";
    case Type::LongArray:
        return "LONG_ARRAY";
    case Type::FloatArray:
        return "FLOAT_ARRAY";
    case Type::DoubleArray:
        return "DOUBLE_ARRAY";
    case Type::UtfArray:
        return "UTF_STRING_ARRAY";
    case Type::Array:
        return "SFS_ARRAY";
    case Type::Object:
        return "SFS_OBJECT";
    case Type::Class:
        return "CLASS";
    case Type::Text:
        return "TEXT";
    }
    return "UNKNOWN";
}

namespace enc {

void key(Buffer &b, std::string_view k) {
    if (k.size() > 0xFFFF)
        throw std::length_error("sfs key too long");
    b.u16(uint16_t(k.size()));
    b.str(k);
}

void str(Buffer &b, std::string_view s) {
    if (s.size() > 0xFFFF)
        throw std::length_error("sfs string too long");
    b.u8(uint8_t(Type::Utf));
    b.u16(uint16_t(s.size()));
    b.str(s);
}

void text(Buffer &b, std::string_view s) {
    b.u8(uint8_t(Type::Text));
    b.u32(uint32_t(s.size()));
    b.str(s);
}

void bytes(Buffer &b, ByteView v) {
    b.u8(uint8_t(Type::ByteArray));
    b.u32(uint32_t(v.size()));
    b.raw(v);
}

}

namespace {

struct Ctx {
    const Limits &lim;
    uint32_t depth = 0;
    uint32_t values = 0;

    void tick() {
        if (++values > lim.max_total_values)
            throw DecodeError("value budget exceeded");
    }

    void enter() {
        if (++depth > lim.max_depth)
            throw DecodeError("nesting too deep");
    }

    void leave() noexcept {
        --depth;
    }
};

template <typename T> void safe_reserve(std::vector<T> &v, uint32_t declared) {
    v.reserve(declared < 64 ? declared : 64);
}

Value read(Cursor &c, Ctx &ctx);

Value read_object_body(Cursor &c, Ctx &ctx) {
    ctx.enter();
    Value v(Type::Object);
    v.agg = std::make_shared<Agg>();

    uint32_t n = c.u16();
    if (n > ctx.lim.max_object_keys)
        throw DecodeError("too many object keys");
    safe_reserve(v.agg->obj, n);

    for (uint32_t k = 0; k < n; ++k) {
        uint32_t klen = c.u16();
        if (klen > ctx.lim.max_string_bytes)
            throw DecodeError("key too long");
        ByteView kb = c.take(klen);
        v.agg->obj.emplace_back(std::string(kb.sv()), read(c, ctx));
    }
    ctx.leave();
    return v;
}

Value read(Cursor &c, Ctx &ctx) {
    ctx.tick();
    const Limits &L = ctx.lim;
    uint8_t tb = c.u8();
    Type t = Type(tb);

    switch (t) {
    case Type::Null:
        return Value(Type::Null);

    case Type::Bool: {
        Value v(Type::Bool);
        v.i = c.u8() ? 1 : 0;
        return v;
    }
    case Type::Byte: {
        Value v(Type::Byte);
        v.i = int8_t(c.u8());
        return v;
    }
    case Type::Short: {
        Value v(Type::Short);
        v.i = c.i16();
        return v;
    }
    case Type::Int: {
        Value v(Type::Int);
        v.i = c.i32();
        return v;
    }
    case Type::Long: {
        Value v(Type::Long);
        v.i = c.i64();
        return v;
    }
    case Type::Float: {
        Value v(Type::Float);
        uint32_t bits = c.u32();
        float f;
        std::memcpy(&f, &bits, 4);
        v.d = f;
        return v;
    }
    case Type::Double: {
        Value v(Type::Double);
        uint64_t bits = c.u64();
        double d;
        std::memcpy(&d, &bits, 8);
        v.d = d;
        return v;
    }
    case Type::Utf: {
        uint32_t n = c.u16();
        if (n > L.max_string_bytes)
            throw DecodeError("string too long");
        Value v(Type::Utf);
        v.s = std::string(c.take(n).sv());
        return v;
    }
    case Type::Text: {
        uint32_t n = c.u32();
        if (n > L.max_string_bytes)
            throw DecodeError("text too long");
        Value v(Type::Text);
        v.s = std::string(c.take(n).sv());
        return v;
    }
    case Type::ByteArray: {
        uint32_t n = c.u32();
        if (n > L.max_binary_bytes)
            throw DecodeError("byte array too large");
        ByteView bv = c.take(n);
        Value v(Type::ByteArray);
        v.agg = std::make_shared<Agg>();
        v.agg->bin.assign(bv.data(), bv.data() + bv.size());
        return v;
    }
    case Type::BoolArray:
    case Type::ShortArray:
    case Type::IntArray:
    case Type::LongArray:
    case Type::FloatArray:
    case Type::DoubleArray:
    case Type::UtfArray: {
        uint32_t n = c.u16();
        if (n > L.max_array_items)
            throw DecodeError("array too long");
        Value v(t);
        v.agg = std::make_shared<Agg>();
        safe_reserve(v.agg->arr, n);
        for (uint32_t k = 0; k < n; ++k) {
            ctx.tick();
            Value e;
            switch (t) {
            case Type::BoolArray:
                e.t = Type::Bool;
                e.i = c.u8() ? 1 : 0;
                break;
            case Type::ShortArray:
                e.t = Type::Short;
                e.i = c.i16();
                break;
            case Type::IntArray:
                e.t = Type::Int;
                e.i = c.i32();
                break;
            case Type::LongArray:
                e.t = Type::Long;
                e.i = c.i64();
                break;
            case Type::FloatArray: {
                e.t = Type::Float;
                uint32_t b = c.u32();
                float f;
                std::memcpy(&f, &b, 4);
                e.d = f;
                break;
            }
            case Type::DoubleArray: {
                e.t = Type::Double;
                uint64_t b = c.u64();
                double d;
                std::memcpy(&d, &b, 8);
                e.d = d;
                break;
            }
            default: {
                uint32_t sl = c.u16();
                if (sl > L.max_string_bytes)
                    throw DecodeError("string too long");
                e.t = Type::Utf;
                e.s = std::string(c.take(sl).sv());
                break;
            }
            }
            v.agg->arr.push_back(std::move(e));
        }
        return v;
    }
    case Type::Array: {
        ctx.enter();
        uint32_t n = c.u16();
        if (n > L.max_array_items)
            throw DecodeError("array too long");
        Value v(Type::Array);
        v.agg = std::make_shared<Agg>();
        safe_reserve(v.agg->arr, n);
        for (uint32_t k = 0; k < n; ++k)
            v.agg->arr.push_back(read(c, ctx));
        ctx.leave();
        return v;
    }
    case Type::Object:
        return read_object_body(c, ctx);

    case Type::Class:

        if (!L.allow_class)
            throw DecodeError("CLASS type refused");
        return read_object_body(c, ctx);
    }
    throw DecodeError("unknown SFS type 0x" + std::string(1, "0123456789abcdef"[(tb >> 4) & 0xF]) +
                      std::string(1, "0123456789abcdef"[tb & 0xF]));
}

}

Value decode_value(Cursor &c, const Limits &lim) {
    Ctx ctx{lim};
    return read(c, ctx);
}

Value decode_object_body(Cursor &c, const Limits &lim) {
    Ctx ctx{lim};
    ctx.tick();
    return read_object_body(c, ctx);
}

void encode_value(Buffer &out, const Value &v) {
    switch (v.t) {
    case Type::Null:
        enc::null_(out);
        return;
    case Type::Bool:
        enc::boolean(out, v.i != 0);
        return;
    case Type::Byte:
        enc::byte_(out, int(v.i));
        return;
    case Type::Short:
        enc::short_(out, int(v.i));
        return;
    case Type::Int:
        enc::int_(out, int32_t(v.i));
        return;
    case Type::Long:
        enc::long_(out, v.i);
        return;
    case Type::Float:
        enc::float_(out, float(v.d));
        return;
    case Type::Double:
        enc::double_(out, v.d);
        return;
    case Type::Utf:
        enc::str(out, v.s);
        return;
    case Type::Text:
        enc::text(out, v.s);
        return;
    case Type::ByteArray:
        enc::bytes(out, v.agg ? ByteView(v.agg->bin) : ByteView{});
        return;
    case Type::BoolArray:
    case Type::ShortArray:
    case Type::IntArray:
    case Type::LongArray:
    case Type::FloatArray:
    case Type::DoubleArray:
    case Type::UtfArray: {
        const auto &a = v.items();
        out.u8(uint8_t(v.t));
        out.u16(uint16_t(a.size()));
        for (const auto &e : a) {
            switch (v.t) {
            case Type::BoolArray:
                out.u8(e.i ? 1 : 0);
                break;
            case Type::ShortArray:
                out.u16(uint16_t(int16_t(e.i)));
                break;
            case Type::IntArray:
                out.u32(uint32_t(int32_t(e.i)));
                break;
            case Type::LongArray:
                out.u64(uint64_t(e.i));
                break;
            case Type::FloatArray: {
                float f = float(e.d);
                uint32_t b;
                std::memcpy(&b, &f, 4);
                out.u32(b);
                break;
            }
            case Type::DoubleArray: {
                double d = e.d;
                uint64_t b;
                std::memcpy(&b, &d, 8);
                out.u64(b);
                break;
            }
            default:
                out.u16(uint16_t(e.s.size()));
                out.str(e.s);
                break;
            }
        }
        return;
    }
    case Type::Array: {
        out.u8(uint8_t(Type::Array));
        out.u16(uint16_t(v.items().size()));
        for (const auto &e : v.items())
            encode_value(out, e);
        return;
    }
    case Type::Object:
    case Type::Class: {
        static const std::vector<std::pair<std::string, Value>> kEmptyObj;
        out.u8(uint8_t(Type::Object));
        const auto &o = v.agg ? v.agg->obj : kEmptyObj;
        out.u16(uint16_t(o.size()));
        for (const auto &kv : o) {
            enc::key(out, kv.first);
            encode_value(out, kv.second);
        }
        return;
    }
    }
    enc::null_(out);
}

}
