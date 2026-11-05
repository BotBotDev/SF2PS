
#include "sf2/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sf2::json {
namespace {

struct P {
    std::string_view s;
    size_t i = 0;
    const ParseLimits &lim;
    size_t nodes = 0;
    std::string err;

    bool fail(const char *m) {
        if (err.empty())
            err = m;
        return false;
    }

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    bool eof() const {
        return i >= s.size();
    }

    char peek() const {
        return s[i];
    }

    bool tick() {
        return ++nodes <= lim.max_items ? true : fail("too many json nodes");
    }

    bool value(Value &out, size_t depth);

    bool string_(std::string &out) {
        if (eof() || s[i] != '"')
            return fail("expected string");
        ++i;
        out.clear();
        while (true) {
            if (eof())
                return fail("unterminated string");
            unsigned char c = uint8_t(s[i]);
            if (c == '"') {
                ++i;
                return true;
            }
            if (out.size() > lim.max_string)
                return fail("string too long");
            if (c == '\\') {
                ++i;
                if (eof())
                    return fail("bad escape");
                char e = s[i++];
                switch (e) {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp))
                        return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                            i += 2;
                            uint32_t lo;
                            if (!hex4(lo))
                                return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                return fail("bad surrogate pair");
                        } else
                            return fail("lone surrogate");
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("lone surrogate");
                    }
                    utf8(out, cp);
                    break;
                }
                default:
                    return fail("bad escape");
                }
                continue;
            }
            if (c < 0x20)
                return fail("raw control char in string");
            out += char(c);
            ++i;
        }
    }

    bool hex4(uint32_t &cp) {
        if (i + 4 > s.size())
            return fail("bad \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s[i + size_t(k)];
            int d;
            if (h >= '0' && h <= '9')
                d = h - '0';
            else if (h >= 'a' && h <= 'f')
                d = h - 'a' + 10;
            else if (h >= 'A' && h <= 'F')
                d = h - 'A' + 10;
            else
                return fail("bad \\u escape");
            cp = cp * 16 + uint32_t(d);
        }
        i += 4;
        return true;
    }

    static void utf8(std::string &out, uint32_t cp) {
        if (cp < 0x80)
            out += char(cp);
        else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
};

bool P::value(Value &out, size_t depth) {
    if (depth > lim.max_depth)
        return fail("json too deep");
    if (!tick())
        return false;
    ws();
    if (eof())
        return fail("unexpected end of json");

    char c = peek();
    switch (c) {
    case 'n':
        if (s.compare(i, 4, "null") != 0)
            return fail("bad literal");
        i += 4;
        out = Value{};
        return true;
    case 't':
        if (s.compare(i, 4, "true") != 0)
            return fail("bad literal");
        i += 4;
        out.kind = Kind::Bool;
        out.b = true;
        return true;
    case 'f':
        if (s.compare(i, 5, "false") != 0)
            return fail("bad literal");
        i += 5;
        out.kind = Kind::Bool;
        out.b = false;
        return true;
    case '"': {
        out.kind = Kind::String;
        return string_(out.s);
    }
    case '[': {
        ++i;
        out = Value::make_array();
        ws();
        if (!eof() && peek() == ']') {
            ++i;
            return true;
        }
        for (;;) {
            Value e;
            if (!value(e, depth + 1))
                return false;
            out.arr->push_back(std::move(e));
            ws();
            if (eof())
                return fail("unterminated array");
            if (peek() == ',') {
                ++i;
                continue;
            }
            if (peek() == ']') {
                ++i;
                return true;
            }
            return fail("expected , or ]");
        }
    }
    case '{': {
        ++i;
        out = Value::make_object();
        ws();
        if (!eof() && peek() == '}') {
            ++i;
            return true;
        }
        for (;;) {
            ws();
            std::string k;
            if (!string_(k))
                return false;
            ws();
            if (eof() || peek() != ':')
                return fail("expected :");
            ++i;
            Value v;
            if (!value(v, depth + 1))
                return false;
            out.obj->emplace_back(std::move(k), std::move(v));
            ws();
            if (eof())
                return fail("unterminated object");
            if (peek() == ',') {
                ++i;
                continue;
            }
            if (peek() == '}') {
                ++i;
                return true;
            }
            return fail("expected , or }");
        }
    }
    default: {
        size_t start = i;
        if (!eof() && peek() == '-')
            ++i;
        bool digits = false, isdbl = false;
        while (!eof() && peek() >= '0' && peek() <= '9') {
            ++i;
            digits = true;
        }
        if (!eof() && peek() == '.') {
            isdbl = true;
            ++i;
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i;
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            isdbl = true;
            ++i;
            if (!eof() && (peek() == '+' || peek() == '-'))
                ++i;
            while (!eof() && peek() >= '0' && peek() <= '9')
                ++i;
        }
        if (!digits)
            return fail("bad number");
        std::string_view tok = s.substr(start, i - start);
        if (isdbl) {
            out.kind = Kind::Double;
            out.d = std::strtod(std::string(tok).c_str(), nullptr);
        } else {
            int64_t n = 0;
            auto r = std::from_chars(tok.data(), tok.data() + tok.size(), n);
            if (r.ec != std::errc()) {
                out.kind = Kind::Double;
                out.d = std::strtod(std::string(tok).c_str(), nullptr);
            } else {
                out.kind = Kind::Int;
                out.i = n;
            }
        }
        return true;
    }
    }
}

size_t utf8_next(std::string_view s, size_t i, uint32_t &cp) {
    unsigned char c = uint8_t(s[i]);
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        cp = uint32_t(c & 0x1F) << 6 | uint32_t(uint8_t(s[i + 1]) & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        cp = uint32_t(c & 0x0F) << 12 | uint32_t(uint8_t(s[i + 1]) & 0x3F) << 6 |
             uint32_t(uint8_t(s[i + 2]) & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        cp = uint32_t(c & 0x07) << 18 | uint32_t(uint8_t(s[i + 1]) & 0x3F) << 12 |
             uint32_t(uint8_t(s[i + 2]) & 0x3F) << 6 | uint32_t(uint8_t(s[i + 3]) & 0x3F);
        return 4;
    }
    cp = 0xFFFD;
    return 1;
}

void dump_string(std::string &out, std::string_view s) {
    static const char *H = "0123456789abcdef";
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = uint8_t(s[i]);
        if (c == '"') {
            out += "\\\"";
            ++i;
            continue;
        }
        if (c == '\\') {
            out += "\\\\";
            ++i;
            continue;
        }
        if (c == '\b') {
            out += "\\b";
            ++i;
            continue;
        }
        if (c == '\f') {
            out += "\\f";
            ++i;
            continue;
        }
        if (c == '\n') {
            out += "\\n";
            ++i;
            continue;
        }
        if (c == '\r') {
            out += "\\r";
            ++i;
            continue;
        }
        if (c == '\t') {
            out += "\\t";
            ++i;
            continue;
        }
        if (c < 0x20) {
            out += "\\u00";
            out += H[c >> 4];
            out += H[c & 0xF];
            ++i;
            continue;
        }
        if (c < 0x80) {
            out += char(c);
            ++i;
            continue;
        }

        uint32_t cp = 0;
        i += utf8_next(s, i, cp);
        if (cp < 0x10000) {
            out += "\\u";
            out += H[(cp >> 12) & 0xF];
            out += H[(cp >> 8) & 0xF];
            out += H[(cp >> 4) & 0xF];
            out += H[cp & 0xF];
        } else {
            uint32_t v = cp - 0x10000;
            uint32_t hi = 0xD800 + (v >> 10), lo = 0xDC00 + (v & 0x3FF);
            for (uint32_t u : {hi, lo}) {
                out += "\\u";
                out += H[(u >> 12) & 0xF];
                out += H[(u >> 8) & 0xF];
                out += H[(u >> 4) & 0xF];
                out += H[u & 0xF];
            }
        }
    }
    out += '"';
}

}

bool parse(std::string_view text, Value &out, const ParseLimits &lim, std::string &err) {
    P p{text, 0, lim, 0, {}};
    if (!p.value(out, 0)) {
        err = p.err.empty() ? "invalid json" : p.err;
        return false;
    }
    p.ws();
    if (!p.eof()) {
        err = "trailing data after json value";
        return false;
    }
    return true;
}

void dump_to(std::string &out, const Value &v) {
    switch (v.kind) {
    case Kind::Null:
        out += "null";
        return;
    case Kind::Bool:
        out += v.b ? "true" : "false";
        return;
    case Kind::Int: {
        char b[24];
        int n = snprintf(b, sizeof b, "%lld", (long long)v.i);
        out.append(b, size_t(n));
        return;
    }
    case Kind::Double: {
        if (std::isnan(v.d)) {
            out += "NaN";
            return;
        }
        if (std::isinf(v.d)) {
            out += v.d > 0 ? "Infinity" : "-Infinity";
            return;
        }
        char b[40];
        int n = snprintf(b, sizeof b, "%.17g", v.d);

        for (int prec = 1; prec <= 17; ++prec) {
            char t[40];
            int m = snprintf(t, sizeof t, "%.*g", prec, v.d);
            if (std::strtod(t, nullptr) == v.d) {
                std::memcpy(b, t, size_t(m) + 1);
                n = m;
                break;
            }
        }
        out.append(b, size_t(n));
        if (out.find_first_of(".eEnI", out.size() - size_t(n)) == std::string::npos)
            out += ".0";
        return;
    }
    case Kind::String:
        dump_string(out, v.s);
        return;
    case Kind::Array: {
        out += '[';
        if (v.arr) {
            bool first = true;
            for (const auto &e : *v.arr) {
                if (!first)
                    out += ',';
                first = false;
                dump_to(out, e);
            }
        }
        out += ']';
        return;
    }
    case Kind::Object: {
        out += '{';
        if (v.obj) {
            bool first = true;
            for (const auto &kv : *v.obj) {
                if (!first)
                    out += ',';
                first = false;
                dump_string(out, kv.first);
                out += ':';
                dump_to(out, kv.second);
            }
        }
        out += '}';
        return;
    }
    }
}

std::string dump(const Value &v) {
    std::string s;
    s.reserve(256);
    dump_to(s, v);
    return s;
}

}
