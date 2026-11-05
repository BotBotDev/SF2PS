
#include "sf2/responses.hpp"

#include "sf2/packet.hpp"
#include "sf2/protobuf.hpp"
#include "sf2/sfs.hpp"

namespace sf2::resp {
namespace {

void ext_envelope(Buffer &payload, std::string_view cmd, int32_t req_id, ByteView body,
                  int32_t code = 0) {
    sfs::ObjWriter root(payload);
    {
        sfs::ObjWriter p = root.object("p");
        {
            sfs::ObjWriter inner = p.object("p");
            inner.bytes("b", body);
            inner.int_("c", code);
            inner.int_("id", req_id);
        }
        p.str("c", cmd);
    }
    root.short_("a", 13);
    root.byte_("c", 1);
}

void event_envelope(Buffer &payload, std::string_view cmd, ByteView body) {
    sfs::ObjWriter root(payload);
    {
        sfs::ObjWriter p = root.object("p");
        {
            sfs::ObjWriter inner = p.object("p");
            inner.bytes("b", body);
            inner.int_("c", 0);
        }
        p.str("c", cmd);
    }
    root.short_("a", 13);
    root.byte_("c", 1);
}

thread_local Buffer t_payload;
thread_local Buffer t_proto;

Buffer &scratch_payload() {
    t_payload.clear();
    return t_payload;
}

Buffer &scratch_proto() {
    t_proto.clear();
    return t_proto;
}

}

void handshake(Buffer &out, std::string_view token, int32_t max_msg,
               int32_t compression_threshold) {
    Buffer &pl = scratch_payload();
    {
        sfs::ObjWriter root(pl);
        {
            sfs::ObjWriter p = root.object("p");
            p.int_("ct", compression_threshold);
            p.int_("ms", max_msg);
            p.str("tk", token);
        }
        root.short_("a", 0);
        root.byte_("c", 0);
    }
    packet::frame_into(out, pl.view(), false);
}

void login_ok(Buffer &out, std::string_view username, int32_t player_id, std::string_view zone) {
    Buffer &pl = scratch_payload();
    {
        sfs::ObjWriter root(pl);
        {
            sfs::ObjWriter p = root.object("p");
            {
                sfs::ObjWriter inner = p.object("p");
                inner.str("$FS_NEW_LOGIN_NAME", username);
            }
            p.short_("rs", 0);
            p.str("zn", zone);
            p.str("un", username);
            p.short_("pi", 0);
            p.begin_array("rl", 0);
            p.int_("id", player_id);
        }
        root.short_("a", 1);
        root.byte_("c", 0);
    }
    packet::frame_into(out, pl.view(), false);
}

void login_error(Buffer &out, int16_t error_code, std::string_view detail) {
    Buffer &pl = scratch_payload();
    {
        sfs::ObjWriter root(pl);
        {
            sfs::ObjWriter p = root.object("p");
            p.short_("ec", error_code);
            p.begin_array("ep", 1);
            sfs::enc::str(pl, detail);
        }
        root.short_("a", 1);
        root.byte_("c", 0);
    }
    packet::frame_into(out, pl.view(), false);
}

void join_zone(Buffer &out, bool registered) {
    Buffer &pr = scratch_proto();
    pb::v(pr, 1, registered ? 1 : 0);
    Buffer &pl = scratch_payload();
    event_envelope(pl, "join_zone", pr.view());
    packet::frame_into(out, pl.view(), false);
}

void ext_event(Buffer &out, std::string_view cmd, ByteView payload, bool compress) {
    Buffer &pl = scratch_payload();
    event_envelope(pl, cmd, payload);
    packet::frame_into(out, pl.view(), compress);
}

void ping(Buffer &out, int32_t seq_id, int64_t client_ms, int64_t server_ms) {
    Buffer &pr = scratch_proto();
    {
        pb::Sub s(pr, 1);
        pb::v(pr, 1, uint64_t(client_ms));
    }
    {
        pb::Sub s(pr, 2);
        pb::v(pr, 1, uint64_t(server_ms));
    }

    Buffer &pl = scratch_payload();
    ext_envelope(pl, "ping", seq_id, pr.view());
    packet::frame_into(out, pl.view(), false);
}

void ext(Buffer &out, std::string_view cmd, int32_t req_id, ByteView payload, bool compress) {
    Buffer &pl = scratch_payload();
    ext_envelope(pl, cmd, req_id, payload);
    packet::frame_into(out, pl.view(), compress);
}

void ext_error(Buffer &out, std::string_view cmd, int32_t req_id, int32_t code) {
    Buffer &pl = scratch_payload();
    ext_envelope(pl, cmd, req_id, {}, code);
    packet::frame_into(out, pl.view(), false);
}

void get_player(Buffer &out, const Player &p, int32_t seq_id, int64_t now_ms) {
    Buffer &pr = scratch_proto();
    proto::build_player(pr, p, now_ms);
    Buffer &pl = scratch_payload();
    ext_envelope(pl, "get_player", seq_id, pr.view());
    packet::frame_into(out, pl.view(), true);
}

void public_player(Buffer &out, const Player &p, int32_t seq_id, int64_t presence) {
    Buffer &pr = scratch_proto();
    proto::build_public_player(pr, p, presence);
    Buffer &pl = scratch_payload();
    ext_envelope(pl, "get_public_player", seq_id, pr.view());
    packet::frame_into(out, pl.view(), true);
}

void chat_event(Buffer &out, int64_t player_id, std::string_view name, int64_t chat_type,
                std::string_view text, int64_t ts_ms) {
    Buffer &pr = scratch_proto();
    pb::v(pr, 1, uint64_t(player_id));
    pb::str(pr, 2, name);
    pb::v(pr, 3, uint64_t(chat_type));
    pb::str(pr, 4, text);
    {
        pb::Sub s(pr, 5);
        pb::v(pr, 1, uint64_t(ts_ms));
    }
    Buffer &pl = scratch_payload();
    ext_envelope(pl, "chat_event", -1, pr.view());
    packet::frame_into(out, pl.view(), true);
}

void proto_string(Buffer &out, std::string_view s) {
    pb::str(out, 1, s);
}

void proto_varint(Buffer &out, int64_t v) {
    pb::v(out, 1, uint64_t(v));
}

}
