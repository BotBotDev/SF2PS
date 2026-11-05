

#pragma once

#include <string_view>

#include "sf2/bytes.hpp"
#include "sf2/player.hpp"

namespace sf2::resp {

void handshake(Buffer &out, std::string_view token, int32_t max_msg = 1000000,
               int32_t compression_threshold = 512);

void login_ok(Buffer &out, std::string_view username, int32_t player_id, std::string_view zone);

void login_error(Buffer &out, int16_t error_code, std::string_view detail);

void join_zone(Buffer &out, bool registered);

void ping(Buffer &out, int32_t seq_id, int64_t client_ms, int64_t server_ms);

void ext(Buffer &out, std::string_view cmd, int32_t req_id, ByteView payload = {},
         bool compress = false);

void ext_error(Buffer &out, std::string_view cmd, int32_t req_id, int32_t code);

void ext_event(Buffer &out, std::string_view cmd, ByteView payload = {}, bool compress = false);

void get_player(Buffer &out, const Player &p, int32_t seq_id, int64_t now_ms);
void public_player(Buffer &out, const Player &p, int32_t seq_id, int64_t presence = 1);
void chat_event(Buffer &out, int64_t player_id, std::string_view name, int64_t chat_type,
                std::string_view text, int64_t ts_ms);

void proto_string(Buffer &out, std::string_view s);
void proto_varint(Buffer &out, int64_t v);

}
