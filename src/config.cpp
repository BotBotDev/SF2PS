
#include "sf2/config.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace sf2 {
namespace {

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool to_bool(const std::string &v, bool &out) {
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

bool to_u32(const std::string &v, uint32_t &out) {
    char *end = nullptr;
    unsigned long long n = std::strtoull(v.c_str(), &end, 10);
    if (!end || *end || n > 0xFFFFFFFFull)
        return false;
    out = uint32_t(n);
    return true;
}

bool to_dbl(const std::string &v, double &out) {
    char *end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    if (!end || *end || d < 0)
        return false;
    out = d;
    return true;
}

}

namespace {

bool parse_raid_override(const std::string &k, const std::string &v,
                         std::map<int64_t, Config::RaidOverride> &into, std::string &err) {
    if (k.compare(0, 5, "raid.") != 0)
        return false;
    const size_t dot = k.find('.', 5);
    if (dot == std::string::npos)
        return false;

    const std::string id_text = k.substr(5, dot - 5);
    if (id_text.empty())
        return false;
    for (char c : id_text)
        if (c < '0' || c > '9')
            return false;

    char *end = nullptr;
    const long long id = std::strtoll(id_text.c_str(), &end, 10);
    if (!end || *end)
        return false;

    const std::string field = k.substr(dot + 1);
    Config::RaidOverride &o = into[int64_t(id)];

    if (field == "damage_scale") {
        double x = 0;
        if (!to_dbl(v, x) || !(x > 0) || x > 100000) {
            err = "bad damage_scale for " + k;
            return true;
        }
        o.damage_scale = x;
        return true;
    }

    long long n = std::strtoll(v.c_str(), &end, 10);
    if (!end || *end || n < 0) {
        err = "bad integer for " + k;
        return true;
    }

    if (field == "hp")
        o.hp = n;
    else if (field == "party")
        o.party_size = n;
    else if (field == "duration_ms")
        o.duration_ms = n;
    else if (field == "cooldown_ms")
        o.cooldown_ms = n;
    else if (field == "dan")
        o.dan = n;
    else {
        err = "unknown raid override field: " + field;
        return true;
    }
    return true;
}
}

bool Config::set(const std::string &k, const std::string &v, std::string &err) {
    uint32_t u = 0;
    double d = 0;
    bool b = false;

    if (k.compare(0, 5, "raid.") == 0) {
        std::string sub;
        if (parse_raid_override(k, v, raid_overrides, sub)) {
            if (!sub.empty()) {
                err = sub;
                return false;
            }
            return true;
        }
    }

#define STR(key, field)                                                                            \
    if (k == key) {                                                                                \
        field = v;                                                                                 \
        return true;                                                                               \
    }
#define U32(key, field)                                                                            \
    if (k == key) {                                                                                \
        if (!to_u32(v, u)) {                                                                       \
            err = "bad integer for " + k;                                                          \
            return false;                                                                          \
        }                                                                                          \
        field = u;                                                                                 \
        return true;                                                                               \
    }
#define DBL(key, field)                                                                            \
    if (k == key) {                                                                                \
        if (!to_dbl(v, d)) {                                                                       \
            err = "bad number for " + k;                                                           \
            return false;                                                                          \
        }                                                                                          \
        field = d;                                                                                 \
        return true;                                                                               \
    }
#define BOOL(key, field)                                                                           \
    if (k == key) {                                                                                \
        if (!to_bool(v, b)) {                                                                      \
            err = "bad boolean for " + k;                                                          \
            return false;                                                                          \
        }                                                                                          \
        field = b;                                                                                 \
        return true;                                                                               \
    }
#define I64(key, field)                                                                            \
    if (k == key) {                                                                                \
        char *e = nullptr;                                                                         \
        long long n = std::strtoll(v.c_str(), &e, 10);                                             \
        if (!e || *e || n < 0) {                                                                   \
            err = "bad integer for " + k;                                                          \
            return false;                                                                          \
        }                                                                                          \
        field = int64_t(n);                                                                        \
        return true;                                                                               \
    }

    STR("bind_host", bind_host)
    if (k == "bind_port") {
        if (!to_u32(v, u) || u == 0 || u > 65535) {
            err = "bad port";
            return false;
        }
        bind_port = uint16_t(u);
        return true;
    }
    if (k == "threads") {
        if (!to_u32(v, u)) {
            err = "bad threads";
            return false;
        }
        threads = int(u);
        return true;
    }
    if (k == "backlog") {
        if (!to_u32(v, u)) {
            err = "bad backlog";
            return false;
        }
        backlog = int(u);
        return true;
    }
    BOOL("tcp_nodelay", tcp_nodelay)

    U32("max_connections", max_connections)
    U32("max_conns_per_ip", max_conns_per_ip)
    U32("max_frame_bytes", max_frame_bytes)
    U32("max_decompressed_bytes", max_decompressed_bytes)
    U32("max_write_queue_bytes", max_write_queue_bytes)
    U32("handshake_timeout_s", handshake_timeout_s)
    U32("login_timeout_s", login_timeout_s)
    U32("idle_timeout_s", idle_timeout_s)
    U32("client_revision", client_revision)

    DBL("rate_packets_per_s", rate_packets_per_s)
    U32("burst_packets", burst_packets)
    DBL("rate_heavy_per_s", rate_heavy_per_s)
    U32("burst_heavy", burst_heavy)
    DBL("rate_chat_per_s", rate_chat_per_s)
    U32("burst_chat", burst_chat)
    DBL("rate_lookup_per_s", rate_lookup_per_s)
    U32("burst_lookup", burst_lookup)
    DBL("rate_login_per_ip_s", rate_login_per_ip_s)
    U32("burst_login_ip", burst_login_ip)

    BOOL("enable_raids", enable_raids)
    BOOL("raid_bots_enabled", raid_bots_enabled)
    U32("raid_bot_fill_wait_ms", raid_bot_fill_wait_ms)
    U32("raid_bot_min_damage", raid_bot_min_damage)
    U32("raid_bot_max_damage", raid_bot_max_damage)
    U32("raid_match_wait_ms", raid_match_wait_ms)
    U32("raid_max_rounds", raid_max_rounds)
    U32("raid_min_round_ms", raid_min_round_ms)
    U32("raid_round_grace_ms", raid_round_grace_ms)
    U32("raid_max_round_damage", raid_max_round_damage)
    U32("raid_max_round_hits", raid_max_round_hits)
    U32("raid_round_rating", raid_round_rating)
    U32("raid_keep_finished_s", raid_keep_finished_s)
    U32("raid_max_short_players", raid_max_short_players)
    U32("raid_default_boss_hp", raid_default_boss_hp)
    U32("raid_fallback_party_size", raid_fallback_party_size)
    U32("raid_fallback_duration_ms", raid_fallback_duration_ms)
    U32("raid_fallback_cooldown_ms", raid_fallback_cooldown_ms)
    BOOL("raid_verify_rounds", raid_verify_rounds)
    BOOL("raid_verify_enforce", raid_verify_enforce)
    BOOL("raid_enforce_damage_threshold", raid_enforce_damage_threshold)
    BOOL("raid_enforce_round_expiry", raid_enforce_round_expiry)
    BOOL("raid_enforce_round_cooldown", raid_enforce_round_cooldown)
    U32("raid_max_finish_attempts", raid_max_finish_attempts)
    BOOL("raid_require_boss_dead", raid_require_boss_dead)
    BOOL("raid_allow_hp_override", raid_allow_hp_override)
    BOOL("push_profile_updates", push_profile_updates)
    STR("game_data_dir", game_data_dir)
    STR("anticheat_file", anticheat_file)
    BOOL("require_game_data", require_game_data)
    STR("new_player_items", new_player_items)
    STR("new_player_perks", new_player_perks)
    U32("new_player_gems", new_player_gems)
    STR("raid_reward_place1", raid_reward_place1)
    STR("raid_reward_place2", raid_reward_place2)
    STR("raid_reward_place3", raid_reward_place3)
    STR("raid_reward_other", raid_reward_other)

    U32("league_size", league_size)
    U32("league_period_s", league_period_s)
    I64("league_tz_offset_s", league_tz_offset_s)
    I64("league_content_version", league_content_version)
    U32("leaderboard_size", leaderboard_size)
    U32("season_id", season_id)
    BOOL("leaderboard_season_fallback", leaderboard_season_fallback)
    BOOL("leaderboard_echo_asked_season", leaderboard_echo_asked_season)
    BOOL("league_seed_on_login", league_seed_on_login)

    BOOL("enable_clans", enable_clans)
    U32("clan_max_members", clan_max_members)
    U32("clan_find_limit", clan_find_limit)

    if (k == "auth_mode") {
        if (v != "original" && v != "open") {
            err = "auth_mode must be original|open";
            return false;
        }
        auth_mode = v;
        return true;
    }
    U32("pbkdf2_iters", pbkdf2_iters)
    BOOL("single_session", single_session)

    BOOL("accept_client_inventory", accept_client_inventory)
    STR("inventory_allowlist_file", inventory_allowlist_file)
    BOOL("static_leaderboard", static_leaderboard)
    STR("get_var_value", get_var_value)
    U32("max_inventory_bytes", max_inventory_bytes)
    U32("max_inventory_items", max_inventory_items)
    BOOL("inventory_preserve_unknown", inventory_preserve_unknown)
    U32("max_inventory_keys", max_inventory_keys)
    U32("max_perks_per_item", max_perks_per_item)
    U32("max_name_chars", max_name_chars)
    U32("max_chat_chars", max_chat_chars)
    BOOL("enable_chat", enable_chat)
    BOOL("enable_demo_handlers", enable_demo_handlers)

    STR("db_path", db_path)
    U32("db_busy_ms", db_busy_ms)
    BOOL("db_wal", db_wal)
    STR("log_level", log_level)
    STR("admin_socket", admin_socket)

#undef STR
#undef U32
#undef DBL
#undef BOOL

    err = "unknown key: " + k;
    return false;
}

bool Config::load_file(const std::string &path, std::string &err) {
    std::ifstream f(path);
    if (!f)
        return true;
    config_path = path;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        size_t hash = line.find_first_of("#;");
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = path + ":" + std::to_string(lineno) + ": expected key = value";
            return false;
        }
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (!set(k, v, err)) {

            if (err.compare(0, 13, "unknown key: ") == 0) {
                unknown_keys.push_back(path + ":" + std::to_string(lineno) + ": " + k);
                err.clear();
                continue;
            }
            err = path + ":" + std::to_string(lineno) + ": " + err;
            return false;
        }
    }
    return true;
}

std::string Config::dump() const {
    std::ostringstream o;
    o << "bind=" << bind_host << ':' << bind_port << " threads=" << threads
      << " max_conn=" << max_connections << " per_ip=" << max_conns_per_ip << " auth=" << auth_mode
      << " client_inventory=" << (accept_client_inventory ? "accept" : "reject")
      << " chat=" << (enable_chat ? "on" : "off") << " raids=" << (enable_raids ? "on" : "off")
      << " game_data=" << game_data_dir << " raid_verify=" << (raid_verify_rounds ? "on" : "off")
      << " raid_rounds=" << raid_max_rounds << " raid_dmg_cap=" << raid_max_round_damage
      << " raid_overrides=" << raid_overrides.size() << " clans=" << (enable_clans ? "on" : "off")
      << " db=" << db_path;
    return o.str();
}

}
