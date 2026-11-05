

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sf2 {

struct Config {

    std::string bind_host = "0.0.0.0";
    uint16_t bind_port = 9933;
    int threads = 0;
    int backlog = 1024;
    bool tcp_nodelay = true;

    uint32_t max_connections = 8192;
    uint32_t max_conns_per_ip = 16;
    uint32_t max_frame_bytes = 64 * 1024;
    uint32_t max_decompressed_bytes = 256 * 1024;
    uint32_t max_write_queue_bytes = 1024 * 1024;
    uint32_t handshake_timeout_s = 10;
    uint32_t login_timeout_s = 30;
    uint32_t idle_timeout_s = 120;

    double rate_packets_per_s = 60.0;
    uint32_t burst_packets = 120;
    double rate_heavy_per_s = 2.0;
    uint32_t burst_heavy = 6;
    double rate_chat_per_s = 1.0;
    uint32_t burst_chat = 1;
    double rate_lookup_per_s = 5.0;
    uint32_t burst_lookup = 15;
    double rate_login_per_ip_s = 0.2;
    uint32_t burst_login_ip = 10;

    uint32_t client_revision = 0;

    std::string auth_mode = "original";
    uint32_t pbkdf2_iters = 120000;
    bool single_session = true;

    std::string new_player_items = "Key1:3";

    std::string new_player_perks = "Sphere1_Name:10,Sphere2_Name:5,Sphere3_Name:5";
    uint32_t new_player_gems = 0;

    bool accept_client_inventory = true;
    std::string inventory_allowlist_file;
    bool static_leaderboard = false;
    std::string get_var_value = "9";
    uint32_t max_inventory_bytes = 64 * 1024;
    uint32_t max_inventory_items = 64;

    bool inventory_preserve_unknown = true;
    uint32_t max_inventory_keys = 32;
    uint32_t max_perks_per_item = 12;
    uint32_t max_name_chars = 20;
    uint32_t max_chat_chars = 80;
    bool enable_chat = true;
    bool enable_demo_handlers = false;

    bool enable_raids = true;
    bool raid_bots_enabled = true;
    uint32_t raid_bot_fill_wait_ms = 10000;
    uint32_t raid_bot_min_damage = 100;
    uint32_t raid_bot_max_damage = 1000;
    uint32_t raid_match_wait_ms = 3000;
    uint32_t raid_max_rounds = 20;

    uint32_t raid_min_round_ms = 1500;
    uint32_t raid_round_grace_ms = 60000;
    uint32_t raid_max_round_damage = 200000;
    uint32_t raid_max_round_hits = 512;
    uint32_t raid_round_rating = 0;
    uint32_t raid_keep_finished_s = 300;
    uint32_t raid_max_short_players = 64;

    uint32_t raid_default_boss_hp = 10095;

    bool raid_verify_rounds = true;
    bool raid_verify_enforce = false;
    bool raid_enforce_damage_threshold = true;
    bool raid_enforce_round_expiry = true;

    bool raid_enforce_round_cooldown = false;

    uint32_t raid_max_finish_attempts = 3;

    bool raid_require_boss_dead = true;

    bool raid_allow_hp_override = false;

    bool push_profile_updates = true;

    uint32_t raid_fallback_party_size = 6;
    uint32_t raid_fallback_duration_ms = 763000;
    uint32_t raid_fallback_cooldown_ms = 100000;

    std::string raid_reward_place1;
    std::string raid_reward_place2;
    std::string raid_reward_place3;
    std::string raid_reward_other;

    struct RaidOverride {
        int64_t hp = -1, party_size = -1, duration_ms = -1;
        int64_t cooldown_ms = -1, dan = -1;
        double damage_scale = -1.0;
    };

    std::map<int64_t, RaidOverride> raid_overrides;

    uint32_t league_size = 100;
    uint32_t league_period_s = 86400;

    int64_t league_tz_offset_s = 10800;
    int64_t league_content_version = 0;
    uint32_t leaderboard_size = 100;

    uint32_t season_id = 1292;

    bool leaderboard_season_fallback = false;
    bool leaderboard_echo_asked_season = false;
    bool league_seed_on_login = true;

    bool enable_clans = true;
    uint32_t clan_max_members = 60;
    uint32_t clan_find_limit = 50;

    std::string game_data_dir = "config";
    std::string anticheat_file = "config/anticheat.bin";

    bool require_game_data = true;

    std::string config_path;

    std::vector<std::string> unknown_keys;
    std::string db_path = "players.db";
    uint32_t db_busy_ms = 5000;
    bool db_wal = true;

    std::string log_level = "info";
    std::string admin_socket;

    bool load_file(const std::string &path, std::string &err);
    bool set(const std::string &key, const std::string &value, std::string &err);
    std::string dump() const;
};

}
