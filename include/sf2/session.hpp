

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sf2/bytes.hpp"
#include "sf2/config.hpp"
#include "sf2/player.hpp"
#include "sf2/ratelimit.hpp"

namespace sf2 {

enum class SessionState : uint8_t {
    New = 0,
    Handshaked,
    LoggedIn,
    Closing,
};

struct Session {
    int fd = -1;
    uint64_t gen = 0;
    std::string ip;
    uint16_t port = 0;

    SessionState state = SessionState::New;

    uint64_t connected_ms = 0;
    uint64_t last_rx_ms = 0;
    uint64_t handshaked_ms = 0;
    uint64_t last_seen_write_ms = 0;

    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;
    size_t tx_off = 0;
    bool want_write = false;

    uint32_t client_revision = 0;
    std::string handshake_token;
    std::string api_version;
    std::string client_kind;
    std::string zone;
    std::string login_uuid;

    Player player;
    bool has_player = false;
    int64_t player_id = 0;

    bool season_state_pushed = false;
    bool gem_sync_ready = false;
    bool gems_sent = false;
    int64_t last_gems_sent = 0;
    int64_t moderation_seq = 0, reboot_seq = 0;
    std::vector<std::pair<int64_t, int64_t>> mute_state;
    bool access_revoked = false;
    uint64_t disconnect_at_ms = 0;

    TokenBucket bkt_packets, bkt_heavy, bkt_chat, bkt_lookup;

    uint64_t packets_in = 0, packets_out = 0, bytes_in = 0, bytes_out = 0;
    uint32_t protocol_errors = 0;

    std::string remote() const {
        return ip + ":" + std::to_string(port);
    }

    bool logged_in() const noexcept {
        return state == SessionState::LoggedIn;
    }

    bool named() const noexcept {
        return has_player && !player.name.empty();
    }

    void configure(const Config &c) {
        bkt_packets.configure(c.rate_packets_per_s, c.burst_packets);
        bkt_heavy.configure(c.rate_heavy_per_s, c.burst_heavy);
        bkt_chat.configure(c.rate_chat_per_s, c.burst_chat);
        bkt_lookup.configure(c.rate_lookup_per_s, c.burst_lookup);
    }
};

}
