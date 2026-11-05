
#include "sf2/protobuf.hpp"

namespace sf2::pb {

bool read_varint(Cursor &c, uint64_t &out) noexcept {
    uint64_t v = 0;
    for (int shift = 0; shift < 70; shift += 7) {
        if (c.eof())
            return false;
        uint8_t byte;
        try {
            byte = c.u8();
        } catch (...) {
            return false;
        }

        if (shift == 63 && (byte & 0xFE))
            return false;
        v |= uint64_t(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            out = v;
            return true;
        }
    }
    return false;
}

void scan(ByteView msg, const std::function<bool(const Field &)> &fn) noexcept {
    Cursor c(msg);
    while (!c.eof()) {
        uint64_t tv;
        if (!read_varint(c, tv))
            return;
        if (tv == 0)
            return;
        Field f;
        f.number = uint32_t(tv >> 3);
        uint32_t wire = uint32_t(tv & 7);
        if (f.number == 0)
            return;

        switch (wire) {
        case 0: {
            f.wire = Wire::Varint;
            if (!read_varint(c, f.num))
                return;
            break;
        }
        case 2: {
            f.wire = Wire::Bytes;
            uint64_t len;
            if (!read_varint(c, len))
                return;
            if (len > c.left())
                return;
            try {
                f.data = c.take(size_t(len));
            } catch (...) {
                return;
            }
            break;
        }
        case 1: {
            f.wire = Wire::Fixed64;
            if (c.left() < 8)
                return;
            try {
                ByteView d = c.take(8);
                f.num = 0;
                for (int i = 7; i >= 0; --i)
                    f.num = (f.num << 8) | d[size_t(i)];
            } catch (...) {
                return;
            }
            break;
        }
        case 5: {
            f.wire = Wire::Fixed32;
            if (c.left() < 4)
                return;
            try {
                ByteView d = c.take(4);
                f.num = 0;
                for (int i = 3; i >= 0; --i)
                    f.num = (f.num << 8) | d[size_t(i)];
            } catch (...) {
                return;
            }
            break;
        }
        default:
            return;
        }
        if (!fn(f))
            return;
    }
}

uint64_t get_varint(ByteView msg, uint32_t field, uint64_t dflt) noexcept {
    uint64_t out = dflt;
    scan(msg, [&](const Field &f) {
        if (f.number == field && f.wire == Wire::Varint) {
            out = f.num;
            return false;
        }
        return true;
    });
    return out;
}

ByteView get_bytes(ByteView msg, uint32_t field) noexcept {
    ByteView out;
    scan(msg, [&](const Field &f) {
        if (f.number == field && f.wire == Wire::Bytes) {
            out = f.data;
            return false;
        }
        return true;
    });
    return out;
}

std::string_view get_string(ByteView msg, uint32_t field) noexcept {
    return get_bytes(msg, field).sv();
}

size_t get_repeated_varints(ByteView msg, uint32_t field, std::vector<int64_t> &out,
                            size_t max) noexcept {
    out.clear();
    scan(msg, [&](const Field &f) {
        if (f.number != field)
            return true;
        if (f.wire == Wire::Varint) {
            if (out.size() < max)
                out.push_back(int64_t(f.num));
        } else if (f.wire == Wire::Bytes) {
            Cursor c(f.data);
            uint64_t v;
            while (!c.eof() && out.size() < max && read_varint(c, v))
                out.push_back(int64_t(v));
        }
        return out.size() < max;
    });
    return out.size();
}

bool has_field(ByteView msg, uint32_t field) noexcept {
    bool found = false;
    scan(msg, [&](const Field &f) {
        if (f.number == field) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

}
