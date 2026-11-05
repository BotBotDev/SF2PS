
#include "sf2/db.hpp"
#include "sf2/profile_stats_schema.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "sf2/crypto.hpp"
#include "sf2/json.hpp"
#include "sf2/log.hpp"

namespace sf2 {
namespace {

constexpr int kSchemaVersion = 14;
constexpr int64_t kFirstPlayerId = 5'000'000;

const char *const kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS schema_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS daily_quests (
 player_id INTEGER NOT NULL, slot INTEGER NOT NULL, season_id INTEGER NOT NULL,
 quest_id INTEGER NOT NULL, started_ms INTEGER NOT NULL, completed_ms INTEGER NOT NULL DEFAULT 0,
 progress INTEGER NOT NULL DEFAULT 0, target INTEGER NOT NULL, renewed_ms INTEGER NOT NULL DEFAULT 0,
 generation INTEGER NOT NULL DEFAULT 1, loot BLOB NOT NULL DEFAULT X'',
 PRIMARY KEY(player_id,slot)
);
CREATE TABLE IF NOT EXISTS daily_battle_progress (
 player_id INTEGER NOT NULL, slot INTEGER NOT NULL, generation INTEGER NOT NULL,
 battle_id INTEGER NOT NULL, value INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(player_id,slot,generation,battle_id)
);
CREATE TABLE IF NOT EXISTS daily_events (
 player_id INTEGER NOT NULL,battle_id INTEGER NOT NULL,kind INTEGER NOT NULL,round INTEGER NOT NULL,
 PRIMARY KEY(player_id,battle_id,kind,round)
);
CREATE TABLE IF NOT EXISTS player_battle_pass (
 player_id INTEGER NOT NULL, name TEXT NOT NULL, claimed INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(player_id,name)
);
CREATE TABLE IF NOT EXISTS player_mutes (
 player_id INTEGER NOT NULL, channel INTEGER NOT NULL,
 until_ms INTEGER NOT NULL, reason TEXT NOT NULL DEFAULT '',
 PRIMARY KEY(player_id,channel)
);
CREATE TABLE IF NOT EXISTS server_reboot_notice (
 id INTEGER PRIMARY KEY CHECK(id=1), seq INTEGER NOT NULL, at_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS player_moderation (
    player_id INTEGER PRIMARY KEY,
    kick_seq INTEGER NOT NULL DEFAULT 0,
    kick_kind INTEGER NOT NULL DEFAULT 3,
    reason TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS players (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    login_uuid    TEXT    NOT NULL UNIQUE,
    email         TEXT    NOT NULL DEFAULT '',
    name          TEXT    NOT NULL DEFAULT '',
    tag           TEXT    NOT NULL DEFAULT '',
    rank          INTEGER NOT NULL DEFAULT 1,

    level         INTEGER NOT NULL DEFAULT 1,
    xp            INTEGER NOT NULL DEFAULT 0,
    world         INTEGER NOT NULL DEFAULT 1,
    energy        INTEGER NOT NULL DEFAULT 10,
    rating        INTEGER NOT NULL DEFAULT 0,
    coins         INTEGER NOT NULL DEFAULT 500,

    gems          INTEGER NOT NULL DEFAULT 0,
    currency2     INTEGER NOT NULL DEFAULT 0,

    season_id     INTEGER NOT NULL DEFAULT 0,
    season_rank   INTEGER NOT NULL DEFAULT 0,
    season_score  INTEGER NOT NULL DEFAULT 0,

    kills         INTEGER NOT NULL DEFAULT 0,
    wins          INTEGER NOT NULL DEFAULT 0,
    losses        INTEGER NOT NULL DEFAULT 0,
    draws         INTEGER NOT NULL DEFAULT 0,
    streak        INTEGER NOT NULL DEFAULT 0,

    inventory     TEXT    NOT NULL DEFAULT '{"v":2,"lot":0,"eq":[]}',
    items         TEXT    NOT NULL DEFAULT '[]',
    perks         TEXT    NOT NULL DEFAULT '[]',
    gifts         TEXT    NOT NULL DEFAULT '[]',
    quests        TEXT    NOT NULL DEFAULT '[]',
    opponents     TEXT    NOT NULL DEFAULT '[]',
    match_history TEXT    NOT NULL DEFAULT '[]',

    ts1           INTEGER NOT NULL DEFAULT 0,
    ts2           INTEGER NOT NULL DEFAULT 0,

    pw_salt       BLOB,
    pw_hash       BLOB,
    pw_iters      INTEGER NOT NULL DEFAULT 0,

    banned_until  INTEGER NOT NULL DEFAULT 0,
    created_at    INTEGER NOT NULL DEFAULT 0,
    updated_at    INTEGER NOT NULL DEFAULT 0,
    last_login_at INTEGER NOT NULL DEFAULT 0
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_players_nametag
    ON players(name, tag) WHERE name <> '';
CREATE INDEX IF NOT EXISTS idx_players_season
    ON players(season_id, season_score DESC);

CREATE TABLE IF NOT EXISTS player_vars (
    player_id  INTEGER NOT NULL,
    key        TEXT    NOT NULL,
    value      TEXT    NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (player_id, key)
);

CREATE TABLE IF NOT EXISTS raid_battles (
    id           INTEGER PRIMARY KEY,
    difficulty   INTEGER NOT NULL DEFAULT 1,
    hp_max       INTEGER NOT NULL DEFAULT 0,
    hp_left      INTEGER NOT NULL DEFAULT 0,
    started_ms   INTEGER NOT NULL DEFAULT 0,
    duration_ms  INTEGER NOT NULL DEFAULT 0,
    season_id    INTEGER NOT NULL DEFAULT 0,
    closed       INTEGER NOT NULL DEFAULT 0,
    updated_at   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS raid_participants (
    raid_id      INTEGER NOT NULL,
    player_id    INTEGER NOT NULL,
    damage       INTEGER NOT NULL DEFAULT 0,
    rounds       INTEGER NOT NULL DEFAULT 0,
    place        INTEGER NOT NULL DEFAULT 0,
    finished     INTEGER NOT NULL DEFAULT 0,
    rating       INTEGER NOT NULL DEFAULT 0,
    chest        TEXT    NOT NULL DEFAULT '',
    claimed_at   INTEGER NOT NULL DEFAULT 0,
    updated_at   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (raid_id, player_id)
);
CREATE INDEX IF NOT EXISTS idx_raid_part_player
    ON raid_participants(player_id, claimed_at DESC);

CREATE TABLE IF NOT EXISTS chat_history (
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 channel INTEGER NOT NULL CHECK(channel IN (1,3)), scope_id INTEGER NOT NULL,
 player_id INTEGER NOT NULL, name TEXT NOT NULL, text TEXT NOT NULL,
 sent_at INTEGER NOT NULL,
 CHECK((channel=1 AND scope_id=0) OR (channel=3 AND scope_id>0))
);
CREATE INDEX IF NOT EXISTS idx_chat_history_scope ON chat_history(channel,scope_id,id);

CREATE TABLE IF NOT EXISTS raid_scores (
    player_id    INTEGER NOT NULL,
    season_id    INTEGER NOT NULL,
    score        INTEGER NOT NULL DEFAULT 0,
    damage       INTEGER NOT NULL DEFAULT 0,
    raids        INTEGER NOT NULL DEFAULT 0,
    wins         INTEGER NOT NULL DEFAULT 0,
    best_damage  INTEGER NOT NULL DEFAULT 0,
    rounds       INTEGER NOT NULL DEFAULT 0,
    first_places INTEGER NOT NULL DEFAULT 0,
    prev_place   INTEGER NOT NULL DEFAULT 0,
    updated_at   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (player_id, season_id)
);
CREATE INDEX IF NOT EXISTS idx_raid_scores_board
    ON raid_scores(season_id, score DESC, player_id ASC);

CREATE TABLE IF NOT EXISTS league_seasons (
    id INTEGER PRIMARY KEY,
    ends_ms INTEGER NOT NULL,
    content_version INTEGER NOT NULL UNIQUE,
    closed INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS league_groups (
    id INTEGER PRIMARY KEY,
    season_id INTEGER NOT NULL,
    dan INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_league_groups ON league_groups(season_id,dan,id);
CREATE TABLE IF NOT EXISTS league_members (
    player_id INTEGER NOT NULL,
    season_id INTEGER NOT NULL,
    league_id INTEGER NOT NULL,
    PRIMARY KEY(player_id,season_id)
);
CREATE INDEX IF NOT EXISTS idx_league_members ON league_members(league_id,player_id);
CREATE TABLE IF NOT EXISTS player_medals (
    player_id INTEGER NOT NULL,
    season_id INTEGER NOT NULL,
    earned_ms INTEGER NOT NULL,
    place INTEGER NOT NULL,
    PRIMARY KEY(player_id,season_id)
);
CREATE TABLE IF NOT EXISTS season_notices (
    player_id INTEGER NOT NULL,
    season_id INTEGER NOT NULL,
    league_id INTEGER NOT NULL,
    reward BLOB NOT NULL,
    restored BLOB NOT NULL,
    delivered_ms INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(player_id,season_id)
);
CREATE INDEX IF NOT EXISTS idx_season_notices_pending ON season_notices(player_id,season_id) WHERE delivered_ms=0;
CREATE TABLE IF NOT EXISTS clan_season_scores (
    season_id INTEGER NOT NULL,
    clan_id INTEGER NOT NULL,
    score INTEGER NOT NULL,
    name TEXT NOT NULL,
    emblem TEXT NOT NULL,
    level INTEGER NOT NULL,
    member_count INTEGER NOT NULL,
    PRIMARY KEY(season_id, clan_id)
);
CREATE INDEX IF NOT EXISTS idx_clan_season_board
    ON clan_season_scores(season_id, score DESC, clan_id ASC);

CREATE TABLE IF NOT EXISTS local_developer_gifts (
    grant_id TEXT PRIMARY KEY,
    player_id INTEGER NOT NULL,
    player_uuid TEXT NOT NULL,
    bonus INTEGER NOT NULL CHECK(bonus>0),
    created_at INTEGER NOT NULL,
    claimed_at INTEGER
);
DROP TRIGGER IF EXISTS welcome_bonus_new_player;

CREATE TABLE IF NOT EXISTS clans (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    name          TEXT    NOT NULL UNIQUE,
    emblem        TEXT    NOT NULL DEFAULT '0,0,0',
    description   TEXT    NOT NULL DEFAULT '',
    rating        INTEGER NOT NULL DEFAULT 0,
    weekly_rating INTEGER NOT NULL DEFAULT 0,
    type          INTEGER NOT NULL DEFAULT 1,
    min_rank      INTEGER NOT NULL DEFAULT 0,
    bonus         INTEGER NOT NULL DEFAULT 0,
    level         INTEGER NOT NULL DEFAULT 1,
    created_at    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_clans_rating ON clans(rating DESC, id ASC);

CREATE TABLE IF NOT EXISTS clan_members (
    player_id    INTEGER PRIMARY KEY,
    clan_id      INTEGER NOT NULL,
    role         INTEGER NOT NULL DEFAULT 3,
    contribution INTEGER NOT NULL DEFAULT 0,
    joined_at    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_clan_members
    ON clan_members(clan_id, contribution DESC, player_id ASC);

CREATE TABLE IF NOT EXISTS clan_kick_cooldowns (
    clan_id   INTEGER NOT NULL,
    player_id INTEGER NOT NULL,
    kind      INTEGER NOT NULL DEFAULT 1,
    until_ms  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (clan_id, player_id, kind)
);
)SQL";

const char *const kAddColumns[] = {
    "ALTER TABLE players ADD COLUMN clan_id INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE players ADD COLUMN raid_score INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE raid_scores ADD COLUMN rounds INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE raid_scores ADD COLUMN first_places INTEGER NOT NULL DEFAULT 0",
    "ALTER TABLE clans ADD COLUMN level INTEGER NOT NULL DEFAULT 1",
};

const char *const kSelectCols =
    "id,login_uuid,email,name,tag,rank,"
    "level,xp,world,energy,rating,coins,"
    "gems,currency2,season_id,season_rank,season_score,"
    "kills,wins,losses,draws,streak,"
    "inventory,items,perks,gifts,quests,opponents,match_history,"
    "ts1,ts2,pw_salt,pw_hash,pw_iters,banned_until,created_at,updated_at,"
    "clan_id,raid_score";

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string col_text(sqlite3_stmt *st, int i) {
    const unsigned char *p = sqlite3_column_text(st, i);
    if (!p)
        return {};
    return std::string(reinterpret_cast<const char *>(p), size_t(sqlite3_column_bytes(st, i)));
}

std::vector<uint8_t> col_blob(sqlite3_stmt *st, int i) {
    const void *p = sqlite3_column_blob(st, i);
    int n = sqlite3_column_bytes(st, i);
    if (!p || n <= 0)
        return {};
    const uint8_t *q = static_cast<const uint8_t *>(p);
    return {q, q + n};
}

json::ParseLimits store_limits() {
    json::ParseLimits l;
    l.max_depth = 8;
    l.max_items = 8000;
    l.max_string = 256;
    return l;
}

std::vector<NamedCount> parse_named(const std::string &text) {
    std::vector<NamedCount> out;
    json::Value v;
    std::string err;
    if (!json::parse(text, v, store_limits(), err) || !v.is_array())
        return out;
    for (const auto &e : *v.arr) {
        if (!e.is_array() || e.size() != 2)
            continue;
        const json::Value &n = (*e.arr)[0];
        const json::Value &c = (*e.arr)[1];
        if (!n.is_string() || !c.is_int())
            continue;
        out.push_back({n.s, c.i});
    }
    return out;
}

std::vector<NamedCount> parse_spec(const std::string &spec, const char *what) {
    std::vector<NamedCount> out;
    size_t at = 0;
    while (at < spec.size()) {
        size_t comma = spec.find(',', at);
        std::string piece =
            spec.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = (comma == std::string::npos) ? spec.size() : comma + 1;

        size_t a = piece.find_first_not_of(" \t");
        if (a == std::string::npos)
            continue;
        size_t b = piece.find_last_not_of(" \t");
        piece = piece.substr(a, b - a + 1);
        if (piece.empty())
            continue;

        const size_t colon = piece.rfind(':');
        if (colon == std::string::npos) {
            LOG_WARN("%s: expected name:count in '%s'", what, piece.c_str());
            continue;
        }
        std::string name = piece.substr(0, colon);
        char *end = nullptr;
        long long n = std::strtoll(piece.c_str() + colon + 1, &end, 10);
        if (name.empty() || name.size() > 64 || !end || *end || n <= 0 || n > 1000000) {
            LOG_WARN("%s: bad entry '%s'", what, piece.c_str());
            continue;
        }
        out.push_back(NamedCount{std::move(name), int64_t(n)});
        if (out.size() >= 64)
            break;
    }
    return out;
}

std::string dump_named(const std::vector<NamedCount> &v) {
    json::Value a = json::Value::make_array();
    for (const auto &e : v) {
        json::Value pair = json::Value::make_array();
        pair.push(json::Value::of(e.name));
        pair.push(json::Value::of(e.count));
        a.push(std::move(pair));
    }
    return json::dump(a);
}

std::vector<std::string> parse_strings(const std::string &text) {
    std::vector<std::string> out;
    json::Value v;
    std::string err;
    if (!json::parse(text, v, store_limits(), err) || !v.is_array())
        return out;
    for (const auto &e : *v.arr)
        if (e.is_string())
            out.push_back(e.s);
    return out;
}

std::vector<Opponent> parse_opponents(const std::string &text) {
    std::vector<Opponent> out;
    json::Value v;
    std::string err;
    if (!json::parse(text, v, store_limits(), err) || !v.is_array())
        return out;
    for (const auto &e : *v.arr) {
        if (!e.is_array() || e.size() < 2)
            continue;
        const auto &a = *e.arr;
        Opponent o;
        o.pid = a[0].is_int() ? a[0].i : 0;
        o.rank = a[1].is_int() ? a[1].i : 0;
        if (a.size() > 2 && a[2].is_int()) {
            o.has_rating = true;
            o.rating = a[2].i;
        }
        if (a.size() > 3 && a[3].is_int())
            o.our_rank = a[3].i;
        out.push_back(o);
    }
    return out;
}

std::vector<MatchEntry> parse_matches(const std::string &text) {
    std::vector<MatchEntry> out;
    json::Value v;
    std::string err;
    if (!json::parse(text, v, store_limits(), err) || !v.is_array())
        return out;
    for (const auto &e : *v.arr) {
        if (!e.is_array() || e.size() < 3)
            continue;
        const auto &a = *e.arr;
        if (!a[0].is_int() || !a[1].is_int() || !a[2].is_int())
            continue;
        out.push_back({a[0].i, a[1].i, a[2].i});
    }
    return out;
}

std::vector<Quest> parse_quests(const std::string &text) {
    std::vector<Quest> out;
    json::Value v;
    std::string err;
    if (!json::parse(text, v, store_limits(), err) || !v.is_array())
        return out;
    auto gi = [](const json::Value &o, const char *k, int64_t d = 0) {
        const json::Value *x = o.find(k);
        return (x && x->is_int()) ? x->i : d;
    };
    auto gs = [](const json::Value &o, const char *k) {
        const json::Value *x = o.find(k);
        return (x && x->is_string()) ? x->s : std::string();
    };
    auto gnc = [](const json::Value &o, const char *k) {
        NamedCount nc;
        const json::Value *x = o.find(k);
        if (x && x->is_array() && x->size() == 2) {
            if ((*x->arr)[0].is_string())
                nc.name = (*x->arr)[0].s;
            if ((*x->arr)[1].is_int())
                nc.count = (*x->arr)[1].i;
        }
        return nc;
    };
    for (const auto &e : *v.arr) {
        if (!e.is_object())
            continue;
        Quest q;
        q.qid = gi(e, "qid");
        q.ts_start = gi(e, "ts_start");
        q.ts_end = gi(e, "ts_end");
        q.cur = gi(e, "cur");
        q.max = gi(e, "max");
        q.name = gs(e, "name");
        q.req = gnc(e, "req");
        q.reward = gnc(e, "reward");
        q.amount = gi(e, "amount");
        out.push_back(std::move(q));
    }
    return out;
}

}

bool db_bootstrap(const Config &cfg, std::string &err) {
    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(cfg.db_path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             nullptr);
    if (rc != SQLITE_OK) {
        err = std::string("cannot open database: ") + sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, int(cfg.db_busy_ms));

    char *msg = nullptr;
    if (cfg.db_wal &&
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &msg) != SQLITE_OK) {
        err = std::string("WAL: ") + (msg ? msg : "?");
        sqlite3_free(msg);
        sqlite3_close(db);
        return false;
    }
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, &msg) != SQLITE_OK) {
        err = std::string("schema: ") + (msg ? msg : "?");
        sqlite3_free(msg);
        sqlite3_close(db);
        return false;
    }
    for (const char *alter : kAddColumns)
        sqlite3_exec(db, alter, nullptr, nullptr, nullptr);

    if (sqlite3_exec(db, kProfileStatsSchema, nullptr, nullptr, &msg) != SQLITE_OK) {
        err = std::string("profile stats migration: ") + (msg ? msg : "?");
        sqlite3_free(msg);
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        return false;
    }

    std::string seed = "INSERT INTO sqlite_sequence(name, seq) SELECT 'players', " +
                       std::to_string(kFirstPlayerId) +
                       " WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name='players');";
    sqlite3_exec(db, seed.c_str(), nullptr, nullptr, nullptr);

    std::string ver = "INSERT OR REPLACE INTO schema_meta(key,value) VALUES('version','" +
                      std::to_string(kSchemaVersion) + "');";
    sqlite3_exec(db, ver.c_str(), nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA optimize;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return true;
}

struct Db::Stmts {
    sqlite3_stmt *by_uuid = nullptr;
    sqlite3_stmt *by_id = nullptr;
    sqlite3_stmt *insert = nullptr;
    sqlite3_stmt *set_identity = nullptr;
    sqlite3_stmt *set_inventory = nullptr;
    sqlite3_stmt *set_progress = nullptr;
    sqlite3_stmt *set_currency = nullptr;
    sqlite3_stmt *set_secret = nullptr;
    sqlite3_stmt *touch_login = nullptr;
    sqlite3_stmt *name_taken = nullptr;
    sqlite3_stmt *leaderboard = nullptr;

    ~Stmts() {
        for (sqlite3_stmt *s : {by_uuid, by_id, insert, set_identity, set_inventory, set_progress,
                                set_currency, set_secret, touch_login, name_taken, leaderboard})
            if (s)
                sqlite3_finalize(s);
    }
};

Db::Db(const Config &cfg) : cfg_(cfg), st_(std::make_unique<Stmts>()) {}

Db::~Db() {
    for (auto &kv : cache_)
        sqlite3_finalize(kv.second);
    cache_.clear();
    st_.reset();
    if (db_)
        sqlite3_close(db_);
}

sqlite3_stmt *Db::cached(const char *sql) {
    auto it = cache_.find(sql);
    if (it != cache_.end()) {
        sqlite3_reset(it->second);
        sqlite3_clear_bindings(it->second);
        return it->second;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err_ = std::string("prepare: ") + sqlite3_errmsg(db_);
        return nullptr;
    }
    cache_.emplace(sql, st);
    return st;
}

sqlite3_stmt *Db::prep(const char *sql, std::string &err) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = std::string("prepare failed: ") + sqlite3_errmsg(db_) + " | " + sql;
        return nullptr;
    }
    return st;
}

bool Db::open(std::string &err) {
    int rc = sqlite3_open_v2(cfg_.db_path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        err = std::string("open: ") + sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_busy_timeout(db_, int(cfg_.db_busy_ms));
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=-8000;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);

    const std::string sel = std::string("SELECT ") + kSelectCols + " FROM players ";
    if (!(st_->by_uuid = prep((sel + "WHERE login_uuid=?1").c_str(), err)))
        return false;
    if (!(st_->by_id = prep((sel + "WHERE id=?1").c_str(), err)))
        return false;

    if (!(st_->insert = prep("INSERT INTO players(login_uuid,email,pw_salt,pw_hash,pw_iters,"
                             "created_at,updated_at,last_login_at,items,perks,gems,level,coins) "
                             "VALUES(?1,?2,?3,?4,?5,?6,?6,?6,?7,?8,?9,52,0)",
                             err)))
        return false;
    if (!(st_->set_identity =
              prep("UPDATE players SET name=?2, tag=?3, updated_at=?4 WHERE id=?1", err)))
        return false;
    if (!(st_->set_inventory =
              prep("UPDATE players SET inventory=?2, updated_at=?3 WHERE id=?1", err)))
        return false;
    if (!(st_->set_progress =
              prep("UPDATE players SET level=?2,xp=?3,world=?4,energy=?5,rating=?6,coins=?7,"
                   "updated_at=?8 WHERE id=?1",
                   err)))
        return false;
    if (!(st_->set_currency =
              prep("UPDATE players SET gems=?2, currency2=?3, updated_at=?4 WHERE id=?1", err)))
        return false;
    if (!(st_->set_secret =
              prep("UPDATE players SET pw_salt=?2, pw_hash=?3, pw_iters=?4, updated_at=?5 "
                   "WHERE id=?1",
                   err)))
        return false;
    if (!(st_->touch_login = prep("UPDATE players SET last_login_at=?2 WHERE id=?1", err)))
        return false;
    if (!(st_->name_taken = prep("SELECT 1 FROM players WHERE name=?1 AND tag=?2 LIMIT 1", err)))
        return false;
    if (!(st_->leaderboard =
              prep("SELECT id, season_score FROM players WHERE season_id=?1 AND name<>'' "
                   "ORDER BY season_score DESC, id ASC LIMIT ?2",
                   err)))
        return false;
    return true;
}

std::optional<Player> Db::read_row(sqlite3_stmt *st) {
    Player p;
    int i = 0;
    p.id = sqlite3_column_int64(st, i++);
    p.login_uuid = col_text(st, i++);
    p.email = col_text(st, i++);
    p.name = col_text(st, i++);
    p.tag = col_text(st, i++);
    p.rank = sqlite3_column_int64(st, i++);
    p.progress.level = sqlite3_column_int64(st, i++);
    p.progress.xp = sqlite3_column_int64(st, i++);
    p.progress.world = sqlite3_column_int64(st, i++);
    p.progress.energy = sqlite3_column_int64(st, i++);
    p.progress.rating = sqlite3_column_int64(st, i++);
    p.progress.coins = sqlite3_column_int64(st, i++);
    p.gems = sqlite3_column_int64(st, i++);
    p.currency2 = sqlite3_column_int64(st, i++);
    p.season_id = sqlite3_column_int64(st, i++);
    p.season_rank = sqlite3_column_int64(st, i++);
    p.season_score = sqlite3_column_int64(st, i++);
    p.kills = sqlite3_column_int64(st, i++);
    p.wins = sqlite3_column_int64(st, i++);
    p.losses = sqlite3_column_int64(st, i++);
    p.draws = sqlite3_column_int64(st, i++);
    p.streak = sqlite3_column_int64(st, i++);
    p.inventory_json = col_text(st, i++);
    p.items = parse_named(col_text(st, i++));
    p.perks = parse_named(col_text(st, i++));
    p.gifts = parse_strings(col_text(st, i++));
    p.quests = parse_quests(col_text(st, i++));
    p.opponents = parse_opponents(col_text(st, i++));
    p.match_history = parse_matches(col_text(st, i++));
    p.ts1 = sqlite3_column_int64(st, i++);
    p.ts2 = sqlite3_column_int64(st, i++);
    p.pw_salt = col_blob(st, i++);
    p.pw_hash = col_blob(st, i++);
    p.pw_iters = uint32_t(sqlite3_column_int64(st, i++));
    p.banned_until = sqlite3_column_int64(st, i++);
    p.created_at = sqlite3_column_int64(st, i++);
    p.updated_at = sqlite3_column_int64(st, i++);
    p.clan_id = sqlite3_column_int64(st, i++);
    p.raid_score = sqlite3_column_int64(st, i++);

    if (auto *previous = cached("SELECT MAX(id) FROM league_seasons WHERE closed=1 AND id<?1")) {
        sqlite3_bind_int64(previous, 1, p.season_id);
        if (sqlite3_step(previous) == SQLITE_ROW && sqlite3_column_type(previous, 0) != SQLITE_NULL)
            p.season_rank = sqlite3_column_int64(previous, 0);
        sqlite3_reset(previous);
    }
    p.medals = player_medals(p.id);
    p.mutes = mutes(p.id);
    load_player_progress(p);
    p.has_secret = !p.pw_hash.empty() && p.pw_iters > 0;
    if (p.inventory_json.empty())
        p.inventory_json = R"({"v":2,"lot":0,"eq":[]})";
    p.inventory_z.clear();
    return p;
}

std::optional<Player> Db::by_uuid(std::string_view uuid) {
    sqlite3_stmt *st = st_->by_uuid;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, uuid.data(), int(uuid.size()), SQLITE_TRANSIENT);
    std::optional<Player> out;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = read_row(st);
    sqlite3_reset(st);
    return out;
}

std::optional<Player> Db::by_id(int64_t id) {
    sqlite3_stmt *st = st_->by_id;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, id);
    std::optional<Player> out;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = read_row(st);
    sqlite3_reset(st);
    return out;
}

bool Db::exec_write(sqlite3_stmt *st) {
    int rc = sqlite3_step(st);
    bool ok = (rc == SQLITE_DONE);
    if (!ok)
        err_ = sqlite3_errmsg(db_);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return ok;
}

std::optional<Player> Db::create(std::string_view uuid, std::string_view email,
                                 std::string_view password, const Config &cfg) {
    std::vector<uint8_t> salt, hash;
    uint32_t iters = 0;
    if (!password.empty()) {
        salt.resize(16);
        crypto::random_bytes(salt.data(), salt.size());
        iters = cfg.pbkdf2_iters;
        crypto::Digest d = crypto::pbkdf2(password, ByteView(salt), iters);
        hash.assign(d.begin(), d.end());
    }

    sqlite3_stmt *st = st_->insert;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, uuid.data(), int(uuid.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, email.data(), int(email.size()), SQLITE_TRANSIENT);
    if (salt.empty()) {
        sqlite3_bind_null(st, 3);
        sqlite3_bind_null(st, 4);
    } else {
        sqlite3_bind_blob(st, 3, salt.data(), int(salt.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(st, 4, hash.data(), int(hash.size()), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(st, 5, iters);
    sqlite3_bind_int64(st, 6, now_ms());

    const std::string items = dump_named(parse_spec(cfg.new_player_items, "new_player_items"));
    const std::string perks = dump_named(parse_spec(cfg.new_player_perks, "new_player_perks"));

    LOG_INFO("new account %.*s: granting items=%s perks=%s gems=%lld", int(uuid.size()),
             uuid.data(), items.c_str(), perks.c_str(), (long long)cfg.new_player_gems);
    sqlite3_bind_text(st, 7, items.data(), int(items.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, perks.data(), int(perks.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, cfg.new_player_gems);

    if (!exec_write(st)) {

        return by_uuid(uuid);
    }
    return by_uuid(uuid);
}

bool Db::set_identity(int64_t id, std::string_view name, std::string_view tag) {
    sqlite3_stmt *st = st_->set_identity;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, name.data(), int(name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, tag.data(), int(tag.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, now_ms());
    return exec_write(st);
}

bool Db::set_inventory(int64_t id, std::string_view inv) {
    sqlite3_stmt *st = st_->set_inventory;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, inv.data(), int(inv.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now_ms());
    return exec_write(st);
}

bool Db::set_progress(int64_t id, const Progress &p) {
    sqlite3_stmt *st = st_->set_progress;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, p.level);
    sqlite3_bind_int64(st, 3, p.xp);
    sqlite3_bind_int64(st, 4, p.world);
    sqlite3_bind_int64(st, 5, p.energy);
    sqlite3_bind_int64(st, 6, p.rating);
    sqlite3_bind_int64(st, 7, p.coins);
    sqlite3_bind_int64(st, 8, now_ms());
    return exec_write(st);
}

std::optional<int64_t> Db::gem_balance(int64_t player_id) {
    auto st = cached("SELECT gems FROM players WHERE id=?");
    if (!st)
        return {};
    sqlite3_bind_int64(st, 1, player_id);
    std::optional<int64_t> result;
    if (sqlite3_step(st) == SQLITE_ROW)
        result = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    return result;
}

bool Db::set_currencies(int64_t id, int64_t gems, int64_t currency2) {
    sqlite3_stmt *st = st_->set_currency;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, gems);
    sqlite3_bind_int64(st, 3, currency2);
    sqlite3_bind_int64(st, 4, now_ms());
    return exec_write(st);
}

bool Db::set_secret(int64_t id, ByteView salt, ByteView hash, uint32_t iters) {
    sqlite3_stmt *st = st_->set_secret;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_blob(st, 2, salt.data(), int(salt.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 3, hash.data(), int(hash.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, iters);
    sqlite3_bind_int64(st, 5, now_ms());
    return exec_write(st);
}

bool Db::touch_login(int64_t id, int64_t when) {
    sqlite3_stmt *st = st_->touch_login;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, when);
    return exec_write(st);
}

bool Db::name_taken(std::string_view name, std::string_view tag) {
    sqlite3_stmt *st = st_->name_taken;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_text(st, 1, name.data(), int(name.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, tag.data(), int(tag.size()), SQLITE_TRANSIENT);
    bool taken = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_reset(st);
    return taken;
}

std::vector<LeaderEntry> Db::season_leaderboard(int64_t season_id, int limit) {
    std::vector<LeaderEntry> out;
    sqlite3_stmt *st = st_->leaderboard;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    sqlite3_bind_int64(st, 1, season_id);
    sqlite3_bind_int(st, 2, limit);
    out.reserve(size_t(limit));
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back({sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1)});
    sqlite3_reset(st);
    return out;
}

std::optional<std::string> Db::get_var(int64_t id, std::string_view key) {
    sqlite3_stmt *st = cached("SELECT value FROM player_vars WHERE player_id=?1 AND key=?2");
    if (!st)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, key.data(), int(key.size()), SQLITE_TRANSIENT);
    std::optional<std::string> out;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = col_text(st, 0);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return out;
}

bool Db::set_var(int64_t id, std::string_view key, std::string_view value, int64_t now_ms) {
    sqlite3_stmt *st = cached("INSERT OR REPLACE INTO player_vars(player_id,key,value,updated_at) "
                              "VALUES(?1,?2,?3,?4)");
    if (!st)
        return false;
    const char *vp = value.empty() ? "" : value.data();
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, key.data(), int(key.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, vp, int(value.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, now_ms);
    return exec_write(st);
}

int64_t Db::var_count(int64_t id) {
    sqlite3_stmt *st = cached("SELECT COUNT(*) FROM player_vars WHERE player_id=?1");
    if (!st)
        return 0;
    sqlite3_bind_int64(st, 1, id);
    int64_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return n;
}

}
