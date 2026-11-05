

#include "sf2/handlers.hpp"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "sf2/crypto.hpp"
#include "sf2/gamedata.hpp"
#include "sf2/json.hpp"
#include "sf2/log.hpp"
#include "sf2/protobuf.hpp"
#include "sf2/raid.hpp"
#include "sf2/responses.hpp"
#include "sf2/social.hpp"
#include "sf2/server.hpp"
#include "sf2/validate.hpp"
#include "sf2/zlibx.hpp"

namespace sf2 {

Registry &Registry::instance() {
    static Registry r;
    return r;
}

void Registry::add(const HandlerDef &d) {
    if (d.cmd && *d.cmd)
        by_cmd_[d.cmd] = d;
    else
        by_sys_[sys_key(d.c, d.a)] = d;
}

void Registry::add_all(const HandlerDef *defs, size_t n) {
    for (size_t i = 0; i < n; ++i)
        add(defs[i]);
}

const HandlerDef *Registry::find(int c, int a, std::string_view cmd) const {
    if (!cmd.empty() && cmd.size() <= 64) {
        auto it = by_cmd_.find(std::string(cmd));
        if (it != by_cmd_.end() && it->second.c == c && it->second.a == a)
            return &it->second;
    }
    auto sit = by_sys_.find(sys_key(c, a));
    return sit == by_sys_.end() ? nullptr : &sit->second;
}

std::vector<std::string> Registry::commands() const {
    std::vector<std::string> out;
    out.reserve(by_cmd_.size());
    for (const auto &kv : by_cmd_)
        out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

thread_local Buffer t_out;

Buffer &out_buf() {
    t_out.clear();
    return t_out;
}

void reply(Ctx &x, const Buffer &b) {
    x.w.send(x.s, b.view());
}

constexpr std::string_view kTutorialVar = "ctt";
constexpr std::string_view kTutorialBits = "15";
constexpr size_t kVarKeyMax = 64;
constexpr size_t kVarValueMax = 1024;
constexpr int64_t kVarPerPlayer = 128;

bool var_key_ok(std::string_view k) {
    if (k.empty() || k.size() > kVarKeyMax)
        return false;
    for (unsigned char c : k)
        if (c < 0x21 || c > 0x7E)
            return false;
    return true;
}

bool var_value_ok(std::string_view v) {
    return v.size() <= kVarValueMax && validate::utf8_ok(v);
}

bool find_json(ByteView blob, std::string &out, int depth = 0) {
    if (depth > 3 || blob.empty())
        return false;

    size_t i = 0;
    if (blob.size() >= 3 && blob[0] == 0xEF && blob[1] == 0xBB && blob[2] == 0xBF)
        i = 3;
    while (i < blob.size() &&
           (blob[i] == ' ' || blob[i] == '\n' || blob[i] == '\r' || blob[i] == '\t'))
        ++i;
    if (i < blob.size() && blob[i] == '{') {
        out.assign(blob.sv().substr(i));
        return true;
    }
    if (blob.size() > 2 && blob[0] == 0x78) {
        std::vector<uint8_t> inflated;
        if (zx::inflate_bounded(blob, 64 * 1024, inflated))
            return find_json(ByteView(inflated), out, depth + 1);
    }
    bool found = false;
    pb::scan(blob, [&](const pb::Field &f) {
        if (f.wire == pb::Wire::Bytes && find_json(f.data, out, depth + 1)) {
            found = true;
            return false;
        }
        return true;
    });
    return found;
}

struct Credentials {
    std::string login;
    std::string password;
    std::string email;
};

Credentials parse_credentials(ByteView blob) {
    Credentials c;
    std::string text;
    if (!find_json(blob, text))
        return c;
    if (text.size() > 16 * 1024)
        return c;

    json::Value v;
    json::ParseLimits lim;
    lim.max_depth = 6;
    lim.max_items = 512;
    lim.max_string = 512;
    std::string err;
    if (!json::parse(text, v, lim, err) || !v.is_object())
        return c;

    auto str_of = [&](const char *key) -> std::string {
        const json::Value *f = v.find(key);
        return (f && f->is_string()) ? f->s : std::string();
    };
    c.login = str_of("login");
    if (c.login.empty())
        c.login = str_of("uuid");
    c.password = str_of("password");
    if (c.password.empty())
        c.password = str_of("pass");
    c.email = str_of("email");
    return c;
}

bool account_key_ok(std::string_view s) {
    if (s.size() < 8 || s.size() > 128)
        return false;
    for (char ch : s) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == ':')
            continue;
        return false;
    }
    return true;
}

std::string sanitize_zone(std::string_view z) {
    std::string out;
    for (char ch : z) {
        if (out.size() >= 48)
            break;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_')
            out += ch;
    }
    return out.empty() ? std::string("sf2-sfs-lobby") : out;
}

std::string display_name(const Player &p) {
    if (!p.name.empty())
        return p.name;
    return "guest_" + std::to_string(p.id);
}

void on_handshake(Ctx &x) {
    if (x.s.state != SessionState::New) {
        x.w.close_session(x.s, "duplicate handshake");
        return;
    }
    if (x.req.params) {
        x.s.api_version = validate::sanitize_text(x.req.params->gets("api"), 24);
        x.s.client_kind = validate::sanitize_text(x.req.params->gets("cl"), 24);
    }
    x.s.handshake_token = crypto::random_hex(16);
    x.s.state = SessionState::Handshaked;
    x.s.handshaked_ms = x.now_ms;

    LOG_DEBUG("[%s] handshake api=%s client=%s", x.s.remote().c_str(), x.s.api_version.c_str(),
              x.s.client_kind.c_str());

    Buffer &b = out_buf();
    resp::handshake(b, x.s.handshake_token);
    reply(x, b);
}

void on_check_client_version(Ctx &x) {
    const uint64_t revision = pb::get_varint(x.req.body, 1, 0);
    x.s.client_revision = revision > 0 && revision <= INT32_MAX ? uint32_t(revision) : 0;
    const bool accepted = x.s.client_revision > 0 && x.s.client_revision >= x.cfg.client_revision;
    auto value = json::Value::make_object();
    value.put("revision", json::Value::of(int64_t(x.cfg.client_revision)));
    value.put("accepted", json::Value::of(accepted));
    value.put("url", json::Value::of(std::string()));
    Buffer body, packet;
    resp::proto_string(body, json::dump(value));
    resp::ext(packet, "check_client_version", x.req.req_id, body.view(), true);
    reply(x, packet);
    LOG_DEBUG("client revision=%u required=%u accepted=%d", x.s.client_revision,
              x.cfg.client_revision, int(accepted));
}

enum class AuthResult { Ok, BadCredentials, Banned };

bool hex32_ok(std::string_view s) {
    if (s.size() != 32)
        return false;
    for (char ch : s) {
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))
            continue;
        return false;
    }
    return true;
}

AuthResult authenticate(Ctx &x, const Player &p, const Credentials &cred) {
    const bool banned = p.banned_until == -1 || p.banned_until > x.wall_ms;
    if (x.cfg.auth_mode == "open")
        return banned ? AuthResult::Banned : AuthResult::Ok;

    if (!hex32_ok(cred.password))
        return AuthResult::BadCredentials;
    std::string expected_input = x.s.handshake_token + cred.login;
    std::string expected = crypto::md5_hex(expected_input);
    ByteView got(reinterpret_cast<const uint8_t *>(cred.password.data()), cred.password.size());
    ByteView want(reinterpret_cast<const uint8_t *>(expected.data()), expected.size());
    if (!crypto::const_time_eq(got, want))
        return AuthResult::BadCredentials;
    return banned ? AuthResult::Banned : AuthResult::Ok;
}

void on_login(Ctx &x) {
    Session &s = x.s;

    if (s.state != SessionState::Handshaked) {
        x.w.close_session(s, "login out of order");
        return;
    }
    if (s.client_revision < x.cfg.client_revision) {
        Buffer b;
        resp::login_error(b, 4, "CLIENT_UPDATE_REQUIRED");
        reply(x, b);
        x.w.close_session(s, "client update required");
        return;
    }
    if (!x.w.hub().allow_login(s.ip, x.now_ms)) {
        x.w.hub().stats.logins_failed.fetch_add(1, std::memory_order_relaxed);
        log::throttled(log::Level::Warn, "login-rate", 5000, "[%s] login rate limit",
                       s.remote().c_str());
        Buffer &b = out_buf();
        resp::login_error(b, 6, "too many login attempts");
        reply(x, b);
        x.w.close_session(s, "login flood");
        return;
    }

    if (x.req.params)
        s.zone = sanitize_zone(x.req.params->gets("zn"));
    else
        s.zone = "sf2-sfs-lobby";

    Credentials cred = parse_credentials(x.req.body);
    if (cred.password.empty() && x.req.params)
        cred.password = std::string(x.req.params->gets("pw"));

    if (!account_key_ok(cred.login)) {
        x.w.hub().stats.logins_failed.fetch_add(1, std::memory_order_relaxed);
        log::throttled(log::Level::Warn, "login-key", 5000,
                       "[%s] login rejected: missing or malformed account key", s.remote().c_str());
        Buffer &b = out_buf();
        resp::login_error(b, 4, "invalid credentials");
        reply(x, b);
        x.w.close_session(s, "bad login key");
        return;
    }
    if (!cred.email.empty() && !validate::check_email(cred.email))
        cred.email.clear();

    bool fresh = false;
    auto found = x.w.db().by_uuid(cred.login);
    if (!found) {
        found = x.w.db().create(cred.login, cred.email, "", x.cfg);
        fresh = true;
        if (!found) {
            LOG_ERROR("[%s] account creation failed: %s", s.remote().c_str(),
                      x.w.db().last_error());
            Buffer &b = out_buf();
            resp::login_error(b, 0, "server error");
            reply(x, b);
            x.w.close_session(s, "db error");
            return;
        }
    }

    Player p = std::move(*found);
    AuthResult ar = authenticate(x, p, cred);
    if (ar != AuthResult::Ok) {
        x.w.hub().stats.logins_failed.fetch_add(1, std::memory_order_relaxed);
        const char *msg = ar == AuthResult::Banned ? "account suspended" : "invalid credentials";
        log::audit("login_denied", "player=%lld session=%llu ip=%s reason=\"%s\"", (long long)p.id,
                   (unsigned long long)s.gen, s.ip.c_str(), msg);
        log::throttled(log::Level::Warn, "login-deny", 2000,
                       "[%s] login denied for account %lld: %s", s.remote().c_str(),
                       (long long)p.id, msg);
        if (ar == AuthResult::Banned) {
            auto moderation = x.w.db().moderation(p.id);
            x.w.kick(s, 1, p.banned_until == -1 ? -1 : p.banned_until - x.wall_ms,
                     moderation ? moderation->reason : "");
            return;
        }
        Buffer &b = out_buf();
        resp::login_error(b, 4, msg);
        reply(x, b);
        x.w.close_session(s, "login denied");
        return;
    }

    s.login_uuid = cred.login;
    s.player = std::move(p);
    s.player_id = s.player.id;
    s.has_player = true;
    s.state = SessionState::LoggedIn;
    if (auto m = x.w.db().moderation(s.player_id))
        s.moderation_seq = m->kick_seq;

    sync_season_clock(x.w);
    if (auto current = x.w.db().current_season()) {
        x.w.db().set_season(s.player_id, current->id);
        x.w.db().ensure_season_row(s.player_id, current->id, x.wall_ms);
        if (auto saved = x.w.db().by_id(s.player_id))
            s.player = std::move(*saved);
    }
    x.w.db().touch_login(s.player_id, x.wall_ms);
    x.w.hub().claim_player(s.player_id, &x.w, s.fd, s.gen);
    x.w.hub().stats.logins_ok.fetch_add(1, std::memory_order_relaxed);
    log::audit("login", "player=%lld session=%llu ip=%s new=%d", (long long)s.player_id,
               (unsigned long long)s.gen, s.ip.c_str(), int(fresh));

    LOG_INFO("[%s] login ok id=%lld name=%s zone=%s%s", s.remote().c_str(), (long long)s.player_id,
             s.player.name.empty() ? "<unnamed>" : s.player.name.c_str(), s.zone.c_str(),
             fresh ? " (new account)" : "");

    Buffer &b = out_buf();
    resp::login_ok(b, display_name(s.player), int32_t(s.player_id), s.zone);
    resp::join_zone(b, !s.player.name.empty());
    reply(x, b);

    if (s.named())
        on_player_online(x.w, s);
}

void on_get_player(Ctx &x) {
    if (x.s.named()) {
        x.w.db().refresh_daily(x.s.player_id, x.wall_ms);
        if (auto p = x.w.db().by_id(x.s.player_id))
            x.s.player = std::move(*p);
    }
    Buffer &b = out_buf();
    resp::get_player(b, x.s.player, x.req.req_id, x.wall_ms);
    reply(x, b);

    if (!x.s.season_state_pushed && x.s.named()) {
        x.s.season_state_pushed = true;
        push_season_state(x.w, x.s);
    }
    push_season_notices(x.w, x.s);
}

void on_create_player(Ctx &x) {
    Session &s = x.s;

    if (!s.player.name.empty()) {
        log::throttled(log::Level::Debug, "recreate", 5000,
                       "[%s] create_player ignored: account already named", s.remote().c_str());
        Buffer &b = out_buf();
        resp::get_player(b, s.player, x.req.req_id, x.wall_ms);
        reply(x, b);
        return;
    }

    std::string proposed;
    ByteView body = x.req.body;
    std::string_view f1 = pb::get_string(body, 1);
    std::string_view f2 = pb::get_string(body, 2);
    if (!f1.empty()) {
        size_t hash = f1.find('#');
        proposed = std::string(hash == std::string_view::npos ? f1 : f1.substr(0, hash));
    }
    if (proposed.empty() && !f2.empty())
        proposed = std::string(f2);

    validate::NameResult nr = validate::check_player_name(proposed, x.cfg);
    if (!nr.ok) {
        LOG_INFO("[%s] create_player rejected: %s", s.remote().c_str(),
                 nr.reason ? nr.reason : "invalid name");
        Buffer &b = out_buf();
        resp::get_player(b, s.player, x.req.req_id, x.wall_ms);
        reply(x, b);
        return;
    }

    std::string tag;
    bool stored = false;
    for (int attempt = 0; attempt < 8 && !stored; ++attempt) {
        tag = validate::make_tag();
        if (x.w.db().name_taken(nr.value, tag))
            continue;
        stored = x.w.db().set_identity(s.player.id, nr.value, tag);
    }
    if (stored) {
        s.player.name = nr.value;
        s.player.tag = tag;

        x.w.db().refresh_daily(s.player.id, x.wall_ms);
        if (auto p = x.w.db().by_id(s.player.id))
            s.player = std::move(*p);
        LOG_INFO("[%s] player %lld named %s#%s", s.remote().c_str(), (long long)s.player.id,
                 nr.value.c_str(), tag.c_str());
    } else {
        LOG_WARN("[%s] could not allocate a tag for %lld", s.remote().c_str(),
                 (long long)s.player.id);
    }

    Buffer &b = out_buf();
    resp::get_player(b, s.player, x.req.req_id, x.wall_ms);
    reply(x, b);
}

void on_ping(Ctx &x) {

    if (x.s.player_id && x.now_ms - x.s.last_seen_write_ms > 30000) {
        x.s.last_seen_write_ms = x.now_ms;
        x.w.db().touch_seen(x.s.player_id, x.wall_ms);
    }
    int64_t client_ms = int64_t(pb::get_varint(x.req.body, 1, 0));
    Buffer &b = out_buf();
    resp::ping(b, x.req.req_id, client_ms, x.wall_ms);
    reply(x, b);
}

void on_get_var(Ctx &x) {

    std::string_view key = pb::get_string(x.req.body, 1);
    LOG_INFO("get_var: player %lld key=%.*s", (long long)x.s.player_id, int(key.size()),
             key.data());
    std::string value(x.cfg.get_var_value);

    if (key == kTutorialVar) {

        value.assign(kTutorialBits);
    } else if (var_key_ok(key)) {
        if (auto stored = x.w.db().get_var(x.s.player_id, key))
            value = std::move(*stored);
    }

    Buffer pr;
    resp::proto_string(pr, value);
    Buffer &b = out_buf();
    resp::ext(b, "get_var", x.req.req_id, pr.view());
    reply(x, b);
}

void on_set_var(Ctx &x) {
    std::string_view key = pb::get_string(x.req.body, 1);
    std::string_view value = pb::get_string(x.req.body, 2);
    LOG_INFO("set_var: player %lld key=%.*s value=%.*s", (long long)x.s.player_id, int(key.size()),
             key.data(), int(value.size()), value.data());

    if (!var_key_ok(key) || !var_value_ok(value)) {
        log::throttled(log::Level::Debug, "set-var-bad", 5000,
                       "[%s] set_var rejected: key %zu bytes, value %zu bytes",
                       x.s.remote().c_str(), key.size(), value.size());
    } else if (key == kTutorialVar) {

        log::throttled(log::Level::Debug, "set-var-tutorial", 5000,
                       "[%s] set_var(%.*s) ignored: tutorials are disabled", x.s.remote().c_str(),
                       int(key.size()), key.data());
    } else if (!x.w.db().get_var(x.s.player_id, key).has_value() &&
               x.w.db().var_count(x.s.player_id) >= kVarPerPlayer) {
        log::throttled(log::Level::Warn, "set-var-limit", 5000,
                       "[%s] set_var limit reached for player %lld", x.s.remote().c_str(),
                       (long long)x.s.player_id);
    } else if (!x.w.db().set_var(x.s.player_id, key, value, x.wall_ms)) {
        LOG_WARN("[%s] set_var failed for key %.*s", x.s.remote().c_str(), int(key.size()),
                 key.data());
    }

    Buffer &b = out_buf();
    resp::ext(b, "set_var", x.req.req_id);
    reply(x, b);
}

void on_update_tutorial(Ctx &x) {

    uint64_t step = pb::get_varint(x.req.body, 1, 0);
    log::throttled(log::Level::Debug, "update-tutorial", 5000,
                   "[%s] update_tutorial step=%llu ignored: tutorial disabled",
                   x.s.remote().c_str(), (unsigned long long)step);

    Buffer &b = out_buf();
    resp::ext(b, "update_tutorial", x.req.req_id);
    reply(x, b);
}

void on_refresh_gems(Ctx &x) {
    if (auto balance = x.w.db().gem_balance(x.s.player_id))
        x.s.player.gems = *balance;
    Buffer pr;
    resp::proto_varint(pr, x.s.player.gems);
    Buffer &b = out_buf();
    resp::ext(b, "refresh_gems", x.req.req_id, pr.view());
    reply(x, b);
}

void on_refresh_free_gems(Ctx &x) {
    if (auto balance = x.w.db().gem_balance(x.s.player_id))
        x.s.player.gems = *balance;
    Buffer pr;
    resp::proto_varint(pr, x.s.player.gems);
    Buffer &b = out_buf();
    resp::ext(b, "refresh_free_gems", x.req.req_id, pr.view());
    reply(x, b);
}

void merge_gift_counts(std::vector<NamedCount> &dst, const std::vector<NamedCount> &add) {
    for (const NamedCount &a : add) {
        bool found = false;
        for (NamedCount &d : dst) {
            if (d.name == a.name) {
                d.count += a.count;
                found = true;
                break;
            }
        }
        if (!found && dst.size() < 256)
            dst.push_back(a);
    }
}

void on_receive_gift(Ctx &x) {

    std::string name;
    for (uint32_t field : {1u, 2u}) {
        std::string_view candidate = pb::get_string(x.req.body, field);
        if (!candidate.empty() && candidate.size() <= 64) {
            name.assign(candidate);
            break;
        }
    }

    if (name.empty() || std::find(x.s.player.gifts.begin(), x.s.player.gifts.end(), name) !=
                            x.s.player.gifts.end()) {
        log::throttled(log::Level::Warn, "gift-claimed", 5000,
                       "[%s] receive_gift: '%s' already received by player %lld",
                       x.s.remote().c_str(), name.c_str(), (long long)x.s.player_id);
        Buffer &b = out_buf();
        resp::ext(b, "receive_gift", x.req.req_id);
        reply(x, b);
        return;
    }

    LootRoll roll;
    if (!GameData::instance().gift_container(name) || !GameData::instance().roll(name, roll)) {
        log::throttled(log::Level::Warn, "gift-container", 5000,
                       "[%s] receive_gift: no gift container named '%s'", x.s.remote().c_str(),
                       name.c_str());
        Buffer &b = out_buf();
        resp::ext(b, "receive_gift", x.req.req_id);
        reply(x, b);
        return;
    }

    RaidChest chest;
    chest.name = name;
    chest.items = roll.currency;
    chest.spheres = roll.consumables;

    merge_gift_counts(x.s.player.items, chest.items);
    merge_gift_counts(x.s.player.perks, chest.spheres);
    x.s.player.gems += roll.gems;
    x.s.player.gifts.push_back(name);

    x.w.db().set_items(x.s.player_id, x.s.player.items);
    x.w.db().set_perks(x.s.player_id, x.s.player.perks);
    x.w.db().set_currencies(x.s.player_id, x.s.player.gems, x.s.player.currency2);
    x.w.db().set_gifts(x.s.player_id, x.s.player.gifts);

    Buffer pr;
    {
        pb::Sub loot(pr, 1);
        proto::build_loot(pr, chest);
    }
    {
        pb::Sub pd(pr, 2);
        proto::build_player_info(pr, x.s.player);
    }
    Buffer &b = out_buf();
    resp::ext(b, "receive_gift", x.req.req_id, pr.view(), true);
    reply(x, b);

    LOG_INFO("receive_gift: player %lld claimed '%s' (%lld gems)", (long long)x.s.player_id,
             name.c_str(), (long long)roll.gems);
}

void on_buy_pack(Ctx &x) {
    sync_session_season(x.w, x.s);
    const auto old_rank = x.s.player.rank;
    LootRoll roll;
    auto p = x.w.db().buy_pack(x.s.player.id, pb::get_string(x.req.body, 1), x.wall_ms, roll);
    Buffer pr;
    if (p) {
        x.s.player = std::move(*p);
        RaidChest chest;
        const auto *pack = GameData::instance().pack(pb::get_string(x.req.body, 1));
        chest.name = pack->container;
        chest.items = roll.currency;
        chest.spheres = roll.consumables;
        chest.gems = roll.gems;
        chest.rating = roll.rating;
        {
            pb::Sub loot(pr, 1);
            proto::build_loot(pr, chest);
        }
        {
            pb::Sub player(pr, 2);
            proto::build_player_info(pr, x.s.player);
        }
        LOG_INFO("buy_pack: player %lld bought '%s' for %lld gems", (long long)x.s.player.id,
                 pack->name.c_str(), (long long)pack->price);
    }
    Buffer &b = out_buf();
    if (p)
        resp::ext(b, "buy_pack", x.req.req_id, pr.view(), true);
    else {
        const auto *pack = GameData::instance().pack(pb::get_string(x.req.body, 1));
        auto fresh = x.w.db().by_id(x.s.player.id);
        resp::ext_error(b, "buy_pack", x.req.req_id,
                        pack && fresh && pack->price > fresh->gems ? 10004 : 10008);
    }
    reply(x, b);
    if (p && x.s.player.rank != old_rank)
        push_season_state(x.w, x.s);
}

void on_daily_quest(Ctx &x) {
    const bool renew = x.req.cmd != "daily_quest_refresh";
    const bool force = x.req.cmd == "daily_quest_renew_force";
    const int64_t id = renew ? int64_t(pb::get_varint(x.req.body, 1, 0)) : 0;
    const int64_t offered = force ? int64_t(pb::get_varint(x.req.body, 2, 0)) : 0;
    int error = 10008;
    Buffer &packet = out_buf();
    if ((renew && id <= 0) ||
        !x.w.db().refresh_daily(x.s.player_id, x.wall_ms, id, force, offered, &error)) {
        resp::ext_error(packet, x.req.cmd, x.req.req_id, error);
        reply(x, packet);
        return;
    }
    if (auto p = x.w.db().by_id(x.s.player_id))
        x.s.player = std::move(*p);
    if (force) {
        Buffer profile, ledger;
        proto::build_player_info(profile, x.s.player);
        resp::ext_event(ledger, "ledger_update_event", profile.view(), true);
        reply(x, ledger);
    }
    resp::ext(packet, x.req.cmd, x.req.req_id, ByteView(x.s.player.daily_payload), true);
    reply(x, packet);
}

void on_bp_refresh(Ctx &x) {
    if (auto p = x.w.db().by_id(x.s.player_id))
        x.s.player = std::move(*p);
    Buffer profile;
    proto::build_player_info(profile, x.s.player);
    Buffer &packet = out_buf();
    resp::ext(packet, "bp_refresh", x.req.req_id, profile.view(), true);
    reply(x, packet);
}

void on_bp_claim(Ctx &x) {
    const auto name = pb::get_string(x.req.body, 1);
    const int64_t index = int32_t(pb::get_varint(x.req.body, 2, 0));
    std::vector<RaidChest> rewards;
    const int error = x.w.db().claim_battle_pass(x.s.player_id, name, index, x.wall_ms, rewards);
    Buffer &packet = out_buf();
    if (error) {
        resp::ext_error(packet, "bp_claim_reward", x.req.req_id, error);
        reply(x, packet);
        return;
    }
    if (auto p = x.w.db().by_id(x.s.player_id))
        x.s.player = std::move(*p);
    Buffer body;
    for (const auto &reward : rewards) {
        pb::Sub loot(body, 1);
        proto::build_loot(body, reward);
    }
    {
        pb::Sub player(body, 2);
        proto::build_player_info(body, x.s.player);
    }
    resp::ext(packet, "bp_claim_reward", x.req.req_id, body.view(), true);
    reply(x, packet);
    push_season_state(x.w, x.s);
}

void on_update_client_profile(Ctx &x) {
    ByteView packed = pb::get_bytes(x.req.body, 1);

    if (!packed.empty()) {
        std::vector<uint8_t> raw;
        if (!zx::inflate_bounded(packed, x.cfg.max_inventory_bytes, raw)) {
            log::throttled(log::Level::Warn, "inv-zlib", 5000,
                           "[%s] update_client_profile: bad or oversized payload",
                           x.s.remote().c_str());
        } else {
            std::string_view text(reinterpret_cast<const char *>(raw.data()), raw.size());
            if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB &&
                uint8_t(text[2]) == 0xBF)
                text.remove_prefix(3);

            validate::InventoryResult ir = validate::check_inventory(text, x.cfg);
            if (!ir.ok) {
                log::throttled(log::Level::Warn, "inv-bad", 5000,
                               "[%s] rejected inventory from player %lld: %s", x.s.remote().c_str(),
                               (long long)x.s.player.id, ir.reason ? ir.reason : "invalid");
            } else {
                std::string canonical = json::dump(ir.value);
                if (canonical != x.s.player.inventory_json) {
                    if (x.w.db().set_inventory(x.s.player.id, canonical)) {
                        x.s.player.set_inventory(std::move(canonical));
                        LOG_DEBUG("[%s] inventory saved for %lld", x.s.remote().c_str(),
                                  (long long)x.s.player.id);
                    }
                }
            }
        }
    }

    Buffer &b = out_buf();
    resp::ext(b, "update_client_profile", x.req.req_id);
    reply(x, b);
}

void on_leaderboard(Ctx &x) {
    const int64_t asked = int64_t(pb::get_varint(x.req.body, 1, 0));
    const int64_t kind = int64_t(pb::get_varint(x.req.body, 2, 0));
    const int64_t season = asked > 0 ? asked : x.s.player.season_id;
    const int limit =
        int(std::min<uint32_t>(100, x.cfg.leaderboard_size ? x.cfg.leaderboard_size : 100));
    std::vector<LeaderEntry> rows;
    if (kind == 1)
        rows = x.w.db().raid_leaderboard(season, limit);
    else if (kind == 2)
        rows = x.w.db().clan_leaderboard(season, limit);
    LOG_DEBUG("leaderboard player=%lld asked=%lld season=%lld kind=%lld rows=%zu",
              (long long)x.s.player.id, (long long)asked, (long long)season, (long long)kind,
              rows.size());
    Buffer pr;
    pb::v(pr, 1, uint64_t(season));
    for (const LeaderEntry &e : rows) {
        pb::Sub sub(pr, 2);
        pb::v(pr, 1, uint64_t(e.player_id));
        pb::v(pr, 2, uint64_t(e.score));
    }
    Buffer &b = out_buf();
    resp::ext(b, "get_season_leaderboard", x.req.req_id, pr.view(), true);
    reply(x, b);
}

void on_get_public_player(Ctx &x) {
    int64_t target = int64_t(pb::get_varint(x.req.body, 1, 0));
    Buffer &b = out_buf();

    if (target <= 0 || is_raid_bot(target)) {
        resp::ext(b, "get_public_player", x.req.req_id);
        reply(x, b);
        return;
    }

    auto found = x.w.db().by_id(target);
    if (!found || found->name.empty()) {
        resp::ext(b, "get_public_player", x.req.req_id);
        reply(x, b);
        return;
    }

    Player pub;
    pub.id = found->id;
    pub.name = found->name;
    pub.tag = found->tag;
    pub.rank = found->rank;
    pub.clan_id = x.w.db().clan_of(target);
    pub.progress.world = found->progress.world;
    pub.kills = found->kills;
    pub.wins = found->wins;
    pub.losses = found->losses;
    pub.draws = found->draws;
    pub.streak = found->streak;
    pub.season_score = found->season_score;
    pub.inventory_json = found->inventory_json;
    pub.match_history = found->match_history;
    pub.medals = found->medals;

    const Db::RaidStats rs = x.w.db().raid_stats(target, found->season_id);
    const Db::RaidStats lifetime = x.w.db().raid_profile_stats(target);
    pub.kills = lifetime.raids;
    pub.wins = lifetime.wins;
    pub.losses = lifetime.rounds;
    pub.draws = lifetime.first_places;

    pub.streak = rs.rounds > 0 ? rs.damage / rs.rounds : 0;
    pub.season_score = lifetime.score;

    int64_t presence = kPresenceOffline;
    if (target == x.s.player.id || x.w.hub().is_online(target)) {
        presence =
            RaidEngine::instance().battle_of(target) || RaidEngine::instance().is_queued(target)
                ? kPresenceInRaid
                : kPresenceOnline;
    }
    resp::public_player(b, pub, x.req.req_id, presence);
    reply(x, b);
}

void on_chat(Ctx &x) {
    if (!x.cfg.enable_chat)
        return;

    int64_t type = int64_t(pb::get_varint(x.req.body, 1, 0));
    ByteView raw = pb::get_bytes(x.req.body, 2);

    if (type < 1 || type > 3)
        return;

    for (const auto &mute : x.w.db().mutes(x.s.player_id)) {
        if (mute.first != type)
            continue;
        Buffer body, packet;
        pb::v(body, 1, uint64_t(type));
        pb::v(body, 2, uint64_t(mute.second));
        resp::ext_event(packet, "mute_event", body.view());
        reply(x, packet);
        Buffer &ack = out_buf();
        resp::ext(ack, "chat", x.req.req_id);
        reply(x, ack);
        return;
    }
    validate::NameResult tr = validate::check_chat_text(raw.sv(), x.cfg);
    if (!tr.ok)
        return;

    LOG_INFO("chat %lld <%s> %s", (long long)x.s.player.id, x.s.player.name.c_str(),
             tr.value.c_str());

    Buffer b;
    resp::chat_event(b, x.s.player.id, x.s.player.name, type, tr.value, x.wall_ms);
    auto packet = std::make_shared<const std::vector<uint8_t>>(b.vec());
    if (type == 1) {
        if (!x.w.db().append_chat(type, 0, x.s.player.id, x.s.player.name, tr.value, x.wall_ms))
            return;
        x.w.hub().broadcast(packet, 0);
    } else if (type == 3 && x.cfg.enable_clans) {
        const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
        if (!clan_id)
            return;
        auto role = x.w.db().clan_member_role(clan_id, x.s.player_id);
        if (!role || *role < kClanLeader || *role > kClanNovice)
            return;
        const auto &lim = GameData::instance().clan();
        const char *role_names[] = {"", "ClanLeader", "ClanOfficer", "ClanAdept", "ClanNovice"};
        const int64_t fallback_ranks[] = {0, 40, 30, 20, 10};
        int64_t rank = fallback_ranks[*role], minimum = 10;
        for (const auto &entry : lim.roles)
            if (entry.first == role_names[*role])
                rank = entry.second;
        for (const auto &entry : lim.permissions)
            if (entry.first == "WriteToChat")
                minimum = entry.second;
        if (rank < minimum)
            return;
        if (validate::sanitize_text(tr.value, size_t(lim.chat_max)) != tr.value)
            return;
        if (!x.w.db().append_chat(type, clan_id, x.s.player.id, x.s.player.name, tr.value,
                                  x.wall_ms))
            return;
        for (int64_t pid : x.w.db().clan_member_ids(clan_id, int(x.cfg.clan_max_members)))
            x.w.hub().send_to_player(pid, packet);
    } else if (type == 2 && x.cfg.enable_raids) {
        RaidBattle raid;
        if (!RaidEngine::instance().snapshot_of_player(x.s.player_id, raid) || raid.closed)
            return;
        const auto *me = raid.find(x.s.player_id);
        if (!me || me->left)
            return;
        for (const auto &member : raid.players)
            if (!member.left)
                x.w.hub().send_to_player(member.pid, packet);
    }
}

void on_get_chat_history(Ctx &x) {
    if (!x.cfg.enable_chat)
        return;
    const int64_t channel = int64_t(pb::get_varint(x.req.body, 1, 0));
    if (channel != 1 && channel != 3) {
        Buffer b;
        resp::ext_error(b, "get_chat_history", x.req.req_id, 10005);
        reply(x, b);
        return;
    }
    int64_t scope = 0;
    if (channel == 3 && x.cfg.enable_clans)
        scope = x.w.db().clan_of(x.s.player_id);

    auto messages = (channel == 3 && !scope) ? std::vector<Db::ChatMessage>{}
                                             : x.w.db().chat_history(channel, scope);
    auto value = json::Value::make_object();
    value.put("channel", json::Value::of(channel));
    value.put("scope", json::Value::of(scope));
    value.put("revision", json::Value::of(messages.empty() ? int64_t(0) : messages.back().id));
    auto rows = json::Value::make_array();
    for (const auto &m : messages) {
        auto row = json::Value::make_object();
        row.put("id", json::Value::of(m.id));
        row.put("player", json::Value::of(m.player_id));
        row.put("name", json::Value::of(m.name));
        row.put("text", json::Value::of(m.text));
        row.put("time", json::Value::of(m.sent_at));
        rows.push(std::move(row));
    }
    value.put("messages", std::move(rows));
    Buffer body, packet;
    resp::proto_string(body, json::dump(value));
    resp::ext(packet, "get_chat_history", x.req.req_id, body.view(), true);
    reply(x, packet);
}

void on_unknown_ext(Ctx &x) {
    std::string_view cmd = x.req.cmd;

    if (cmd.empty() || cmd.size() > 48 || !validate::is_identifier(cmd, 48)) {
        log::throttled(log::Level::Debug, "unknown-cmd", 10000,
                       "[%s] dropping unknown extension command", x.s.remote().c_str());
        return;
    }
    log::throttled(log::Level::Debug, "unknown-ack", 10000,
                   "[%s] unknown command %.*s -> empty ack", x.s.remote().c_str(), int(cmd.size()),
                   cmd.data());
    Buffer &b = out_buf();
    resp::ext(b, cmd, x.req.req_id);
    reply(x, b);
}

void on_chat_motd(Ctx &x) {
    Buffer pr;
    resp::proto_string(pr, "Welcome to the private server!");
    Buffer &b = out_buf();
    resp::ext(b, "get_chat_motd", x.req.req_id, pr.view());
    reply(x, b);
}

void on_claim_daily(Ctx &x) {
    Buffer pr;
    resp::proto_varint(pr, 0);
    Buffer &b = out_buf();
    resp::ext(b, "claim_daily_reward", x.req.req_id, pr.view());
    reply(x, b);
}

const HandlerDef kCore[] = {

    {0, 0, "", Need::Any, Cost::Cheap, on_handshake},
    {0, 1, "", Need::Handshaked, Cost::Heavy, on_login},

    {1, 13, "ping", Need::LoggedIn, Cost::Cheap, on_ping},
    {1, 13, "get_player", Need::LoggedIn, Cost::Heavy, on_get_player},
    {1, 13, "create_player", Need::LoggedIn, Cost::Heavy, on_create_player},
    {1, 13, "get_var", Need::LoggedIn, Cost::Cheap, on_get_var},
    {1, 13, "set_var", Need::LoggedIn, Cost::Lookup, on_set_var},
    {1, 13, "update_tutorial", Need::LoggedIn, Cost::Cheap, on_update_tutorial},
    {1, 13, "refresh_gems", Need::LoggedIn, Cost::Cheap, on_refresh_gems},
    {1, 13, "refresh_free_gems", Need::LoggedIn, Cost::Cheap, on_refresh_free_gems},
    {1, 13, "daily_quest_refresh", Need::Named, Cost::Lookup, on_daily_quest},
    {1, 13, "daily_quest_renew", Need::Named, Cost::Heavy, on_daily_quest},
    {1, 13, "daily_quest_renew_force", Need::Named, Cost::Heavy, on_daily_quest},
    {1, 13, "bp_refresh", Need::Named, Cost::Lookup, on_bp_refresh},
    {1, 13, "bp_claim_reward", Need::Named, Cost::Heavy, on_bp_claim},
    {1, 13, "buy_pack", Need::Named, Cost::Heavy, on_buy_pack},
    {1, 13, "receive_gift", Need::Named, Cost::Heavy, on_receive_gift},
    {1, 13, "update_client_profile", Need::LoggedIn, Cost::Heavy, on_update_client_profile},
    {1, 13, "get_season_leaderboard", Need::LoggedIn, Cost::Lookup, on_leaderboard},
    {1, 13, "get_public_player", Need::LoggedIn, Cost::Lookup, on_get_public_player},
    {1, 13, "check_client_version", Need::Handshaked, Cost::Lookup, on_check_client_version},
    {1, 13, "get_chat_history", Need::Named, Cost::Lookup, on_get_chat_history},
    {1, 13, "chat", Need::Named, Cost::Chat, on_chat},

    {1, 13, "", Need::LoggedIn, Cost::Lookup, on_unknown_ext},
};

const HandlerDef kDemo[] = {
    {1, 13, "get_chat_motd", Need::LoggedIn, Cost::Cheap, on_chat_motd},
    {1, 13, "claim_daily_reward", Need::LoggedIn, Cost::Lookup, on_claim_daily},
};

}

void register_core_handlers(const Config &cfg) {
    Registry &r = Registry::instance();
    r.add_all(kCore, sizeof kCore / sizeof kCore[0]);
    if (cfg.enable_demo_handlers)
        r.add_all(kDemo, sizeof kDemo / sizeof kDemo[0]);
}

}
