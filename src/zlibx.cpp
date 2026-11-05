
#include "sf2/zlibx.hpp"

#include <zlib.h>

#include <cstring>
#include <memory>

namespace sf2::zx {
namespace {

struct DeflateStream {
    z_stream zs{};
    bool ready = false;

    ~DeflateStream() {
        if (ready)
            deflateEnd(&zs);
    }

    bool begin() {
        if (!ready) {
            if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK)
                return false;
            ready = true;
            return true;
        }
        return deflateReset(&zs) == Z_OK;
    }
};

struct InflateStream {
    z_stream zs{};
    bool ready = false;

    ~InflateStream() {
        if (ready)
            inflateEnd(&zs);
    }

    bool begin() {
        if (!ready) {
            if (inflateInit(&zs) != Z_OK)
                return false;
            ready = true;
            return true;
        }
        return inflateReset(&zs) == Z_OK;
    }
};

DeflateStream &deflater() {
    static thread_local DeflateStream s;
    return s;
}

InflateStream &inflater() {
    static thread_local InflateStream s;
    return s;
}

}

void deflate_to(Buffer &out, ByteView in) {
    DeflateStream &d = deflater();
    if (!d.begin())
        throw std::runtime_error("deflateInit failed");

    const uLong cap = compressBound(uLong(in.size()));
    const size_t base = out.size();
    out.vec().resize(base + cap);

    d.zs.next_in = const_cast<Bytef *>(in.data());
    d.zs.avail_in = uInt(in.size());
    d.zs.next_out = out.data() + base;
    d.zs.avail_out = uInt(cap);

    int rc = ::deflate(&d.zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        out.vec().resize(base);
        throw std::runtime_error("zlib deflate failed");
    }
    out.vec().resize(base + (cap - d.zs.avail_out));
}

std::vector<uint8_t> deflate(ByteView in) {
    Buffer b;
    deflate_to(b, in);
    return b.take();
}

bool inflate_bounded(ByteView in, size_t max_out, std::vector<uint8_t> &out) {
    out.clear();
    if (in.empty() || max_out == 0)
        return false;

    InflateStream &s = inflater();
    if (!s.begin())
        return false;

    s.zs.next_in = const_cast<Bytef *>(in.data());
    s.zs.avail_in = uInt(in.size());

    size_t cap = in.size() * 4 + 1024;
    if (cap > max_out)
        cap = max_out;
    out.resize(cap);

    size_t produced = 0;
    for (;;) {
        s.zs.next_out = out.data() + produced;
        s.zs.avail_out = uInt(out.size() - produced);
        int rc = ::inflate(&s.zs, Z_NO_FLUSH);
        produced = out.size() - s.zs.avail_out;

        if (rc == Z_STREAM_END)
            break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            out.clear();
            return false;
        }
        if (produced >= max_out) {
            out.clear();
            return false;
        }
        if (s.zs.avail_out == 0) {
            size_t next = out.size() * 2;
            if (next > max_out)
                next = max_out;
            if (next == out.size()) {
                out.clear();
                return false;
            }
            out.resize(next);
            continue;
        }
        if (rc == Z_BUF_ERROR) {
            out.clear();
            return false;
        }
    }
    out.resize(produced);
    return true;
}

}
