
#include "sf2/packet.hpp"

#include "sf2/zlibx.hpp"

namespace sf2::packet {

FrameInfo probe(ByteView buf, const Limits &lim) noexcept {
    FrameInfo fi;
    if (buf.size() < 1)
        return fi;

    const uint8_t flags = buf[0];
    fi.flags = flags;

    if (!(flags & kBinary)) {
        fi.state = FrameState::Invalid;
        fi.error = "not a binary packet";
        return fi;
    }
    if (flags & ~kKnownFlags) {
        fi.state = FrameState::Invalid;
        fi.error = "unknown packet flags";
        return fi;
    }

    const size_t lenbytes = (flags & kBigSize) ? 4 : 2;
    fi.header = 1 + lenbytes;
    if (buf.size() < fi.header)
        return fi;

    const size_t body = (lenbytes == 2) ? be16(buf.data() + 1) : be32(buf.data() + 1);
    fi.body = body;
    fi.total = fi.header + body;

    if (body == 0) {
        fi.state = FrameState::Invalid;
        fi.error = "empty packet body";
        return fi;
    }
    if (fi.total > lim.max_frame) {
        fi.state = FrameState::Invalid;
        fi.error = "packet too large";
        return fi;
    }
    if (buf.size() < fi.total)
        return fi;

    fi.state = FrameState::Ready;
    return fi;
}

sfs::Value parse(ByteView frame, const Limits &lim, std::vector<uint8_t> &scratch) {
    FrameInfo fi = probe(frame, lim);
    if (fi.state != FrameState::Ready)
        throw DecodeError(fi.error ? fi.error : "incomplete frame");

    ByteView payload = frame.sub(fi.header, fi.body);

    if (fi.flags & kCompressed) {
        if (!zx::inflate_bounded(payload, lim.max_decompressed, scratch))
            throw DecodeError("bad or oversized compressed payload");
        payload = ByteView(scratch);
    }

    if (payload.empty() || payload[0] != uint8_t(sfs::Type::Object))
        throw DecodeError("payload is not an SFSObject");

    Cursor c(payload);
    c.skip(1);
    sfs::Value root = sfs::decode_object_body(c, lim.sfs);
    return root;
}

Request route(const sfs::Value &root) noexcept {
    Request r;
    if (!root.is_object())
        return r;

    r.c = int(root.geti("c", -1));
    r.a = int(root.geti("a", -1));

    const sfs::Value *p = root.geto("p");
    if (!p)
        return r;
    r.params = p;
    r.cmd = p->gets("c");

    const sfs::Value *inner = p->geto("p");
    if (inner) {
        r.inner = inner;
        int64_t id = inner->geti("id", 0);

        if (id > INT32_MAX)
            id = INT32_MAX;
        if (id < INT32_MIN)
            id = INT32_MIN;
        r.req_id = int32_t(id);
        r.body = inner->getb("b");
    }
    return r;
}

void frame_into(Buffer &out, ByteView payload, bool compress) {
    if (!compress) {
        out.u8(kBinary);
        out.u16(uint16_t(payload.size()));
        out.raw(payload);
        return;
    }
    out.u8(kBinary | kCompressed);
    const size_t len_off = out.size();
    out.u16(0);
    const size_t start = out.size();
    zx::deflate_to(out, payload);
    out.patch_u16(len_off, uint16_t(out.size() - start));
}

std::vector<uint8_t> frame(ByteView payload, bool compress) {
    Buffer b;
    frame_into(b, payload, compress);
    return b.take();
}

}
