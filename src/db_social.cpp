

#include "sf2/db.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include "sf2/gamedata.hpp"
#include "sf2/protobuf.hpp"

#include "sf2/json.hpp"
#include "sf2/log.hpp"

namespace sf2 {
namespace {

class ClanTransaction {
    sqlite3 *db_;
    bool outer_, active_;

  public:
    explicit ClanTransaction(sqlite3 *db)
        : db_(db), outer_(sqlite3_get_autocommit(db) != 0),
          active_(sqlite3_exec(db, outer_ ? "BEGIN IMMEDIATE" : "SAVEPOINT clan_write", nullptr,
                               nullptr, nullptr) == SQLITE_OK) {}

    explicit operator bool() const {
        return active_;
    }

    bool commit() {
        if (!active_ || sqlite3_exec(db_, outer_ ? "COMMIT" : "RELEASE clan_write", nullptr,
                                     nullptr, nullptr) != SQLITE_OK)
            return false;
        active_ = false;
        return true;
    }

    ~ClanTransaction() {
        if (active_)
            sqlite3_exec(db_, outer_ ? "ROLLBACK" : "ROLLBACK TO clan_write; RELEASE clan_write",
                         nullptr, nullptr, nullptr);
    }
};

int64_t wall_now() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string encode_named(const std::vector<NamedCount> &v) {
    json::Value a = json::Value::make_array();
    for (const NamedCount &e : v) {
        json::Value pair = json::Value::make_array();
        pair.push(json::Value::of(e.name));
        pair.push(json::Value::of(e.count));
        a.push(std::move(pair));
    }
    return json::dump(a);
}

std::string encode_strings(const std::vector<std::string> &v) {
    json::Value a = json::Value::make_array();
    for (const std::string &e : v)
        a.push(json::Value::of(e));
    return json::dump(a);
}

std::string col_str(sqlite3_stmt *st, int i) {
    const unsigned char *p = sqlite3_column_text(st, i);
    if (!p)
        return {};
    return std::string(reinterpret_cast<const char *>(p), size_t(sqlite3_column_bytes(st, i)));
}

void bind_sv(sqlite3_stmt *st, int i, std::string_view s) {
    sqlite3_bind_text(st, i, s.data(), int(s.size()), SQLITE_TRANSIENT);
}

constexpr size_t kMaxIdBatch = 200;

}

bool Db::set_items(int64_t id, const std::vector<NamedCount> &v) {
    sqlite3_stmt *st = cached("UPDATE players SET items=?2, updated_at=?3 WHERE id=?1");
    if (!st)
        return false;
    const std::string blob = encode_named(v);
    sqlite3_bind_int64(st, 1, id);
    bind_sv(st, 2, blob);
    sqlite3_bind_int64(st, 3, wall_now());
    return exec_write(st);
}

bool Db::set_perks(int64_t id, const std::vector<NamedCount> &v) {
    sqlite3_stmt *st = cached("UPDATE players SET perks=?2, updated_at=?3 WHERE id=?1");
    if (!st)
        return false;
    const std::string blob = encode_named(v);
    sqlite3_bind_int64(st, 1, id);
    bind_sv(st, 2, blob);
    sqlite3_bind_int64(st, 3, wall_now());
    return exec_write(st);
}

bool Db::set_gifts(int64_t id, const std::vector<std::string> &v) {
    sqlite3_stmt *st = cached("UPDATE players SET gifts=?2, updated_at=?3 WHERE id=?1");
    if (!st)
        return false;
    const std::string blob = encode_strings(v);
    sqlite3_bind_int64(st, 1, id);
    bind_sv(st, 2, blob);
    sqlite3_bind_int64(st, 3, wall_now());
    return exec_write(st);
}

bool Db::set_rank(int64_t id, int64_t rank) {
    sqlite3_stmt *st = cached("UPDATE players SET rank=?2, updated_at=?3 WHERE id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, rank);
    sqlite3_bind_int64(st, 3, wall_now());
    return exec_write(st);
}

namespace {

int64_t scalar(sqlite3_stmt *st) {
    int64_t v = 0;
    if (st && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    if (st)
        sqlite3_reset(st);
    return v;
}
}

bool Db::add_currency2(int64_t id, int64_t delta, int64_t &now_value) {
    now_value = 0;
    sqlite3_stmt *st =
        cached("UPDATE players SET currency2 = MAX(0, currency2 + ?2), updated_at=?3 "
               "WHERE id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, delta);
    sqlite3_bind_int64(st, 3, wall_now());
    if (!exec_write(st))
        return false;

    sqlite3_stmt *rd = cached("SELECT currency2 FROM players WHERE id=?1");
    if (!rd)
        return false;
    sqlite3_bind_int64(rd, 1, id);
    now_value = scalar(rd);
    return true;
}

bool Db::add_season_score(int64_t id, int64_t delta, int64_t &now_value) {
    now_value = 0;
    sqlite3_stmt *st =
        cached("UPDATE players SET season_score = MAX(0, season_score + ?2), updated_at=?3 "
               "WHERE id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, delta);
    sqlite3_bind_int64(st, 3, wall_now());
    if (!exec_write(st))
        return false;

    sqlite3_stmt *rd = cached("SELECT season_score FROM players WHERE id=?1");
    if (!rd)
        return false;
    sqlite3_bind_int64(rd, 1, id);
    now_value = scalar(rd);
    return true;
}

std::optional<ShortPlayer> Db::short_player(int64_t id) {
    if (is_raid_bot(id)) {
        RaidBattle battle;
        if (!RaidEngine::instance().snapshot_of_player(id, battle))
            return std::nullopt;
        ShortPlayer p;
        p.id = id;
        p.name = "бот";
        p.tag = "";
        if (const auto *def = GameData::instance().raid(battle.raid_id()))
            p.rank = std::max<int64_t>(1, def->dan_restriction);
        else
            p.rank = 1;
        p.presence = kPresenceInRaid;
        return p;
    }
    sqlite3_stmt *st =
        cached("SELECT id,name,tag,rank,clan_id FROM players WHERE id=?1 AND name<>''");
    if (!st)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<ShortPlayer> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ShortPlayer p;
        p.id = sqlite3_column_int64(st, 0);
        p.name = col_str(st, 1);
        p.tag = col_str(st, 2);
        p.rank = sqlite3_column_int64(st, 3);
        p.clan_id = sqlite3_column_int64(st, 4);
        out = std::move(p);
    }
    sqlite3_reset(st);
    return out;
}

std::vector<ShortPlayer> Db::short_players(const std::vector<int64_t> &ids) {
    std::vector<ShortPlayer> out;
    if (ids.empty())
        return out;
    out.reserve(std::min(ids.size(), kMaxIdBatch));
    size_t n = 0;
    for (int64_t id : ids) {
        if (++n > kMaxIdBatch)
            break;
        if (auto p = short_player(id))
            out.push_back(std::move(*p));
    }
    return out;
}

bool Db::save_raid(const RaidBattle &b, int64_t now_ms) {
    sqlite3_stmt *st = cached("INSERT INTO raid_battles(id,difficulty,hp_max,hp_left,started_ms,"
                              "duration_ms,season_id,closed,updated_at) "
                              "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) "
                              "ON CONFLICT(id) DO UPDATE SET hp_left=excluded.hp_left, "
                              "closed=excluded.closed, updated_at=excluded.updated_at");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, b.id);
    sqlite3_bind_int64(st, 2, b.boss.raid_id);
    sqlite3_bind_int64(st, 3, b.boss.hp_max);
    sqlite3_bind_int64(st, 4, b.boss.hp_left);
    sqlite3_bind_int64(st, 5, b.started_ms);
    sqlite3_bind_int64(st, 6, b.duration_ms);
    sqlite3_bind_int64(st, 7, b.season_id);
    sqlite3_bind_int64(st, 8, b.closed ? 1 : 0);
    sqlite3_bind_int64(st, 9, now_ms);
    if (!exec_write(st))
        return false;

    bool ok = true;
    for (const RaidParticipant &p : b.players)
        ok = save_raid_participant(b.id, p, p.reward.total_rating(), now_ms) && ok;
    return ok;
}

bool Db::save_raid_participant(int64_t raid_id, const RaidParticipant &p, int64_t rating,
                               int64_t now_ms) {
    if (is_raid_bot(p.pid))
        return true;
    sqlite3_stmt *st =
        cached("INSERT INTO raid_participants(raid_id,player_id,damage,rounds,place,"
               "finished,rating,chest,claimed_at,updated_at) "
               "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,0,?9) "
               "ON CONFLICT(raid_id,player_id) DO UPDATE SET damage=excluded.damage, "
               "rounds=excluded.rounds, place=excluded.place, finished=excluded.finished, "
               "rating=excluded.rating, chest=excluded.chest, updated_at=excluded.updated_at");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, raid_id);
    sqlite3_bind_int64(st, 2, p.pid);
    sqlite3_bind_int64(st, 3, p.damage);
    sqlite3_bind_int64(st, 4, p.rounds_used);
    sqlite3_bind_int64(st, 5, p.place);
    sqlite3_bind_int64(st, 6, p.finished ? 1 : 0);
    sqlite3_bind_int64(st, 7, rating);
    bind_sv(st, 8, p.reward.present ? p.reward.chest.name : std::string());
    sqlite3_bind_int64(st, 9, now_ms);
    return exec_write(st);
}

bool Db::claim_raid_reward(int64_t raid_id, int64_t player_id, int64_t now_ms) {
    sqlite3_stmt *st = cached("UPDATE raid_participants SET claimed_at=?3 "
                              "WHERE raid_id=?1 AND player_id=?2 AND claimed_at=0");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, raid_id);
    sqlite3_bind_int64(st, 2, player_id);
    sqlite3_bind_int64(st, 3, now_ms);
    if (!exec_write(st))
        return false;
    return sqlite3_changes(db_) > 0;
}

bool Db::add_raid_score(int64_t player_id, int64_t season_id, int64_t score, int64_t damage,
                        bool entered, bool won) {
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    auto current = current_season();
    if (!current || current->id != season_id)
        return false;
    sqlite3_stmt *st = cached("INSERT INTO raid_scores(player_id,season_id,score,damage,raids,wins,"
                              "best_damage,rounds,prev_place,updated_at) "
                              "VALUES(?1,?2,?3,?4,?5,?6,?4,?8,0,?7) "
                              "ON CONFLICT(player_id,season_id) DO UPDATE SET "
                              "score = score + excluded.score, "
                              "damage = damage + excluded.damage, "
                              "raids = raids + excluded.raids, "
                              "wins = wins + excluded.wins, "
                              "rounds = rounds + excluded.rounds, "
                              "best_damage = MAX(best_damage, excluded.best_damage), "
                              "updated_at = excluded.updated_at");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, season_id);
    sqlite3_bind_int64(st, 3, score);
    sqlite3_bind_int64(st, 4, damage);
    sqlite3_bind_int64(st, 5, entered ? 1 : 0);
    sqlite3_bind_int64(st, 6, won ? 1 : 0);
    sqlite3_bind_int64(st, 7, wall_now());

    sqlite3_bind_int64(st, 8, (!entered && damage > 0) ? 1 : 0);
    if (!exec_write(st))
        return false;

    sqlite3_stmt *m = cached("UPDATE players SET raid_score = "
                             "(SELECT score FROM raid_scores WHERE player_id=?1 AND season_id=?2) "
                             "WHERE id=?1");
    if (!m)
        return false;
    sqlite3_bind_int64(m, 1, player_id);
    sqlite3_bind_int64(m, 2, season_id);
    return exec_write(m) && tx.commit();
}

std::vector<std::pair<int64_t, int64_t>> Db::mutes(int64_t id) {
    std::vector<std::pair<int64_t, int64_t>> out;
    auto *st = cached("SELECT channel,until_ms FROM player_mutes WHERE player_id=?1 AND "
                      "until_ms>?2 ORDER BY channel");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, wall_now());
    while (sqlite3_step(st) == SQLITE_ROW)
        out.emplace_back(sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1));
    sqlite3_reset(st);
    return out;
}

std::optional<std::pair<int64_t, int64_t>> Db::reboot_notice() {
    auto *st = cached("SELECT seq,at_ms FROM server_reboot_notice WHERE id=1");
    if (!st)
        return {};
    std::optional<std::pair<int64_t, int64_t>> out;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = std::make_pair(sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1));
    sqlite3_reset(st);
    return out;
}

std::optional<ModerationState> Db::moderation(int64_t id) {
    auto *st =
        cached("SELECT "
               "p.banned_until,COALESCE(m.kick_seq,0),COALESCE(m.kick_kind,3),COALESCE(m.reason,'')"
               " FROM players p LEFT JOIN player_moderation m ON m.player_id=p.id WHERE p.id=?1");
    if (!st)
        return {};
    sqlite3_bind_int64(st, 1, id);
    std::optional<ModerationState> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ModerationState m;
        m.banned_until = sqlite3_column_int64(st, 0);
        m.kick_seq = sqlite3_column_int64(st, 1);
        m.kick_kind = sqlite3_column_int(st, 2);
        const auto *reason = sqlite3_column_text(st, 3);
        if (reason)
            m.reason = reinterpret_cast<const char *>(reason);
        out = std::move(m);
    }
    sqlite3_reset(st);
    return out;
}

std::optional<Player> Db::buy_pack(int64_t id, std::string_view name, int64_t wall_ms,
                                   LootRoll &loot) {
    loot = {};
    const auto *pack = GameData::instance().pack(name);
    if (!pack || !pack->purchasable || pack->price < 0 || pack->container.empty())
        return {};
    if (pack->target_dan < 0 || pack->target_dan > 10)
        return {};
    time_t seconds = wall_ms / 1000;
    tm utc{};
    gmtime_r(&seconds, &utc);
    char date[32];
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &utc);
    if ((!pack->start.empty() && pack->start > date) || (!pack->end.empty() && pack->end <= date))
        return {};
    ClanTransaction tx(db_);
    if (!tx)
        return {};
    auto p = by_id(id);
    if (!p || p->progress.level < pack->min_level ||
        (pack->max_level > 0 && p->progress.level > pack->max_level) || p->gems < pack->price)
        return {};
    LootRoll rolled;
    if (pack->target_dan && p->rank >= pack->target_dan)
        return {};
    if (!GameData::instance().roll(pack->container, rolled) ||
        (!pack->target_dan && rolled.empty()))
        return {};
    p->gems -= pack->price;
    if (!apply_progress_loot(*p, rolled))
        return {};
    if (pack->target_dan) {
        if (!set_rank(id, pack->target_dan))
            return {};

        if (auto season = current_season()) {
            auto *leave = cached("DELETE FROM league_members WHERE player_id=?1 AND season_id=?2");
            if (!leave)
                return {};
            sqlite3_bind_int64(leave, 1, id);
            sqlite3_bind_int64(leave, 2, season->id);
            if (!exec_write(leave) || !ensure_season_row(id, season->id, wall_ms))
                return {};
        }
    }
    if (auto fresh = by_id(id))
        p = std::move(fresh);
    else
        return {};
    if (!tx.commit())
        return {};
    loot = std::move(rolled);
    return p;
}

std::vector<LeaderEntry> Db::raid_leaderboard(int64_t season_id, int limit) {
    std::vector<LeaderEntry> out;
    if (limit <= 0)
        return out;
    sqlite3_stmt *st = cached("SELECT s.player_id, s.score FROM raid_scores s "
                              "JOIN players p ON p.id = s.player_id "
                              "WHERE s.season_id=?1 AND p.name<>'' "
                              "ORDER BY s.score DESC, s.player_id ASC LIMIT ?2");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, season_id);
    sqlite3_bind_int(st, 2, limit);
    out.reserve(size_t(limit));
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back({sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1)});
    sqlite3_reset(st);
    return out;
}

std::vector<RaidRecord> Db::raid_history(int64_t player_id, int limit) {
    std::vector<RaidRecord> out;
    if (limit <= 0)
        return out;
    sqlite3_stmt *st =
        cached("SELECT raid_id,player_id,damage,rounds,place,rating,chest,claimed_at "
               "FROM raid_participants WHERE player_id=?1 "
               "ORDER BY updated_at DESC LIMIT ?2");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW) {
        RaidRecord r;
        r.raid_id = sqlite3_column_int64(st, 0);
        r.player_id = sqlite3_column_int64(st, 1);
        r.damage = sqlite3_column_int64(st, 2);
        r.rounds = sqlite3_column_int64(st, 3);
        r.place = sqlite3_column_int64(st, 4);
        r.rating = sqlite3_column_int64(st, 5);
        r.chest = col_str(st, 6);
        r.claimed_at = sqlite3_column_int64(st, 7);
        out.push_back(std::move(r));
    }
    sqlite3_reset(st);
    return out;
}

std::optional<Db::Season> Db::current_season() {
    sqlite3_stmt *st = cached("SELECT id,ends_ms,content_version,closed FROM league_seasons "
                              "WHERE closed=0 ORDER BY id DESC LIMIT 1");
    if (!st)
        return std::nullopt;
    std::optional<Season> result;
    if (sqlite3_step(st) == SQLITE_ROW)
        result = Season{sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1),
                        sqlite3_column_int64(st, 2), sqlite3_column_int(st, 3) != 0};
    sqlite3_reset(st);
    return result;
}

std::optional<Db::Season> Db::season_by_version(int64_t version) {
    sqlite3_stmt *st = cached("SELECT s.id,s.ends_ms,g.id,s.closed FROM league_seasons s "
                              "JOIN league_groups g ON g.season_id=s.id WHERE g.id=?1");
    if (!st)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, version);
    std::optional<Season> result;
    if (sqlite3_step(st) == SQLITE_ROW)
        result = Season{sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1),
                        sqlite3_column_int64(st, 2), sqlite3_column_int(st, 3) != 0};
    sqlite3_reset(st);
    return result;
}

std::vector<Medal> Db::player_medals(int64_t pid) {
    std::vector<Medal> out;
    auto *st = cached("SELECT season_id,earned_ms,place FROM player_medals WHERE player_id=?1 "
                      "ORDER BY season_id DESC");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, pid);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back({sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1),
                       sqlite3_column_int64(st, 2)});
    sqlite3_reset(st);
    return out;
}

std::vector<Db::SeasonNotice> Db::pending_season_notices(int64_t pid) {
    std::vector<SeasonNotice> out;
    auto *st = cached("SELECT season_id,league_id,reward,restored FROM season_notices WHERE "
                      "player_id=?1 AND delivered_ms=0 ORDER BY season_id LIMIT 16");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, pid);
    while (sqlite3_step(st) == SQLITE_ROW) {
        SeasonNotice n;
        n.season = sqlite3_column_int64(st, 0);
        n.league = sqlite3_column_int64(st, 1);
        for (int i = 2; i <= 3; ++i) {
            const auto *data = static_cast<const uint8_t *>(sqlite3_column_blob(st, i));
            int len = sqlite3_column_bytes(st, i);
            auto &bytes = i == 2 ? n.reward : n.restored;
            if (data && len > 0)
                bytes.assign(data, data + len);
        }
        out.push_back(std::move(n));
    }
    sqlite3_reset(st);
    return out;
}

bool Db::mark_season_notice(int64_t pid, int64_t season, int64_t now) {
    auto *st = cached("UPDATE season_notices SET delivered_ms=?3 WHERE player_id=?1 AND "
                      "season_id=?2 AND delivered_ms=0");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_int64(st, 2, season);
    sqlite3_bind_int64(st, 3, now);
    return exec_write(st) && sqlite3_changes(db_) == 1;
}

bool Db::award_season(const Season &season) {
    const auto &gd = GameData::instance();
    const auto &settings = gd.league();
    auto top = raid_leaderboard(season.id, int(std::clamp<int64_t>(settings.max_rows, 0, 100)));
    int64_t place = 0, last = -1;
    for (size_t i = 0; i < top.size(); ++i) {
        if (top[i].score <= 0)
            continue;
        if (top[i].score != last)
            place = int64_t(i) + 1;
        last = top[i].score;
        bool medal = false;
        for (auto cutoff : settings.medal_positions)
            if (place <= cutoff)
                medal = true;
        if (!medal)
            continue;
        auto *st = cached("INSERT INTO player_medals VALUES(?1,?2,?3,?4)");
        if (!st)
            return false;
        sqlite3_bind_int64(st, 1, top[i].player_id);
        sqlite3_bind_int64(st, 2, season.id);
        sqlite3_bind_int64(st, 3, season.ends_ms);
        sqlite3_bind_int64(st, 4, place);
        if (!exec_write(st))
            return false;
    }

    struct Standing {
        int64_t group = 0, dan = 0, place = 0, score = 0;
    };

    std::unordered_map<int64_t, Standing> standings;
    auto *groups = cached("SELECT id,dan FROM league_groups WHERE season_id=?1 ORDER BY id");
    if (!groups)
        return false;
    sqlite3_bind_int64(groups, 1, season.id);
    std::vector<std::pair<int64_t, int64_t>> group_ids;
    while (sqlite3_step(groups) == SQLITE_ROW)
        group_ids.push_back({sqlite3_column_int64(groups, 0), sqlite3_column_int64(groups, 1)});
    sqlite3_reset(groups);
    for (const auto &[group, dan] : group_ids)
        for (const auto &row : league_board(group, 100000))
            standings[row.pid] = {group, dan, row.place, row.score};
    auto *ids = cached("SELECT id FROM players ORDER BY id");
    if (!ids)
        return false;
    std::vector<int64_t> players;
    while (sqlite3_step(ids) == SQLITE_ROW)
        players.push_back(sqlite3_column_int64(ids, 0));
    sqlite3_reset(ids);
    auto merge = [](std::vector<NamedCount> &dst, const std::vector<NamedCount> &src) {
        for (const auto &a : src) {
            auto it = std::find_if(dst.begin(), dst.end(),
                                   [&](const auto &b) { return a.name == b.name; });
            if (it == dst.end())
                dst.push_back(a);
            else
                it->count = std::min<int64_t>((int64_t(1) << 40), it->count + a.count);
        }
    };
    for (int64_t pid : players) {
        auto p = by_id(pid);
        if (!p)
            return false;
        auto it = standings.find(pid);
        const Standing standing = it != standings.end() ? it->second : Standing{};
        const LeagueReward *tier = nullptr;
        for (const auto &dan : settings.dans) {
            if (dan.dan != standing.dan || standing.score < dan.min_reward_rating ||
                standing.score <= 0)
                continue;
            for (const auto &candidate : dan.rewards)
                if (standing.place <= candidate.pos_to &&
                    (!tier || candidate.pos_to < tier->pos_to))
                    tier = &candidate;
        }
        RaidChest chest;
        bool promoted = false;
        if (tier) {
            LootRoll loot;
            if (!gd.roll(tier->container, loot))
                return false;
            chest.name = tier->container;
            chest.items = loot.currency;
            chest.spheres = loot.consumables;
            chest.gems = loot.gems;
            chest.rating = loot.rating;
            merge(p->items, loot.currency);
            merge(p->perks, loot.consumables);
            p->gems += loot.gems;
            if (tier->dan_up)
                for (const auto &next : settings.dans)
                    if (next.dan == standing.dan + 1) {
                        p->rank = next.dan;
                        promoted = true;
                        break;
                    }
        }
        Buffer restored;
        if (!settings.restore_currency.empty() && settings.restore_value > 0) {
            auto key = std::find_if(p->items.begin(), p->items.end(), [&](const auto &x) {
                return x.name == settings.restore_currency;
            });
            int64_t have = key == p->items.end() ? 0 : key->count;
            if (have < settings.restore_value) {
                int64_t amount = settings.restore_value - have;
                if (key == p->items.end())
                    p->items.push_back({settings.restore_currency, settings.restore_value});
                else
                    key->count = settings.restore_value;
                pb::Sub value(restored, 2);
                pb::str(restored, 1, settings.restore_currency);
                pb::v(restored, 2, uint64_t(amount));
            }
        }
        if (!set_items(pid, p->items) || !set_perks(pid, p->perks) ||
            !set_currencies(pid, p->gems, p->currency2) || !set_rank(pid, p->rank))
            return false;
        Buffer reward;
        if (tier) {
            pb::Sub loot(reward, 1);
            proto::build_loot(reward, chest);
        }
        if (promoted)
            pb::v(reward, 2, 1);
        auto *add =
            cached("INSERT INTO season_notices(player_id,season_id,league_id,reward,restored) "
                   "VALUES(?1,?2,?3,?4,?5)");
        if (!add)
            return false;
        sqlite3_bind_int64(add, 1, pid);
        sqlite3_bind_int64(add, 2, season.id);
        sqlite3_bind_int64(add, 3, standing.group);
        if (reward.empty())
            sqlite3_bind_zeroblob(add, 4, 0);
        else
            sqlite3_bind_blob(add, 4, reward.data(), int(reward.size()), SQLITE_TRANSIENT);
        if (restored.empty())
            sqlite3_bind_zeroblob(add, 5, 0);
        else
            sqlite3_bind_blob(add, 5, restored.data(), int(restored.size()), SQLITE_TRANSIENT);
        if (!exec_write(add))
            return false;
    }
    return true;
}

bool Db::advance_season(int64_t seed, int64_t now, int64_t next_end, int64_t version) {
    if (seed <= 0 || next_end <= now || version <= 0)
        return false;
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    auto old = current_season();
    if (old && old->ends_ms > now)
        return tx.commit();
    const int64_t id = old ? old->id + 1 : seed;
    if (old) {
        if (!award_season(*old))
            return false;
        sqlite3_stmt *archive =
            cached("INSERT INTO clan_season_scores "
                   "SELECT ?1,id,rating,name,emblem,level,"
                   "(SELECT COUNT(*) FROM clan_members m WHERE m.clan_id=clans.id) FROM clans");
        if (!archive)
            return false;
        sqlite3_bind_int64(archive, 1, old->id);
        if (!exec_write(archive))
            return false;
        sqlite3_stmt *close = cached("UPDATE league_seasons SET closed=1 WHERE id=?1");
        if (!close)
            return false;
        sqlite3_bind_int64(close, 1, old->id);
        if (!exec_write(close))
            return false;
        if (sqlite3_exec(db_,
                         "UPDATE clans SET rating=0,weekly_rating=0; "
                         "UPDATE clan_members SET contribution=0",
                         nullptr, nullptr, nullptr) != SQLITE_OK)
            return false;
    }
    sqlite3_stmt *add =
        cached("INSERT INTO league_seasons(id,ends_ms,content_version) VALUES(?1,?2,?3)");
    if (!add)
        return false;
    sqlite3_bind_int64(add, 1, id);
    sqlite3_bind_int64(add, 2, next_end);
    sqlite3_bind_int64(add, 3, version);
    if (!exec_write(add))
        return false;
    sqlite3_stmt *players = cached(
        "UPDATE players SET season_id=?1, season_rank=CASE WHEN ?2 THEN ?3 ELSE season_rank END, "
        "ts1=0, ts2=CASE WHEN ?2 THEN CASE WHEN ts1>0 THEN ts1 ELSE ts2 END ELSE 0 END, "
        "season_score=CASE WHEN ?2 THEN 0 ELSE season_score END, "
        "raid_score=CASE WHEN ?2 THEN 0 ELSE raid_score END, "
        "currency2=CASE WHEN ?2 THEN 0 ELSE currency2 END");
    if (!players)
        return false;
    sqlite3_bind_int64(players, 1, id);
    sqlite3_bind_int(players, 2, old ? 1 : 0);
    sqlite3_bind_int64(players, 3, old ? old->id : 0);
    return exec_write(players) && tx.commit();
}

std::vector<LeaderEntry> Db::clan_leaderboard(int64_t season, int limit) {
    std::vector<LeaderEntry> out;
    if (limit <= 0)
        return out;
    const auto current = current_season();
    sqlite3_stmt *st =
        current && current->id == season
            ? cached("SELECT id,rating FROM clans ORDER BY rating DESC,id ASC LIMIT ?2")
            : cached("SELECT clan_id,score FROM clan_season_scores WHERE season_id=?1 "
                     "ORDER BY score DESC,clan_id ASC LIMIT ?2");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, season);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back({sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1)});
    sqlite3_reset(st);
    return out;
}

std::vector<LeagueEntry> Db::league_board(int64_t group_id, int limit) {
    std::vector<LeagueEntry> out;
    if (limit <= 0)
        return out;
    sqlite3_stmt *st =
        cached("SELECT s.player_id, s.score, s.prev_place FROM raid_scores s "
               "JOIN league_members m ON m.player_id=s.player_id AND m.season_id=s.season_id "
               "JOIN players p ON p.id = s.player_id "
               "WHERE m.league_id=?1 AND p.name<>'' "
               "ORDER BY s.score DESC, s.player_id ASC LIMIT ?2");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, group_id);
    sqlite3_bind_int(st, 2, limit);
    int64_t seen = 0, place = 0, last = -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        LeagueEntry e;
        e.pid = sqlite3_column_int64(st, 0);
        e.score = sqlite3_column_int64(st, 1);
        e.prev_place = sqlite3_column_int64(st, 2);
        ++seen;
        if (e.score != last) {
            place = seen;
            last = e.score;
        }
        e.place = place;
        out.push_back(e);
    }
    sqlite3_reset(st);
    return out;
}

std::vector<int64_t> Db::raid_seasons() {
    std::vector<int64_t> out;
    sqlite3_stmt *st =
        cached("SELECT DISTINCT season_id FROM raid_scores ORDER BY season_id DESC LIMIT 32");
    if (!st)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(st, 0));
    sqlite3_reset(st);
    return out;
}

bool Db::ensure_season_row(int64_t player_id, int64_t season_id, int64_t now_ms) {
    if (player_id <= 0 || season_id <= 0)
        return false;
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    auto current = current_season();
    if (!current || current->id != season_id)
        return false;
    sqlite3_stmt *st = cached(
        "INSERT OR IGNORE INTO raid_scores(player_id,season_id,updated_at) VALUES(?1,?2,?3)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, season_id);
    sqlite3_bind_int64(st, 3, now_ms);
    if (!exec_write(st))
        return false;
    sqlite3_stmt *member =
        cached("SELECT league_id FROM league_members WHERE player_id=?1 AND season_id=?2");
    if (!member)
        return false;
    sqlite3_bind_int64(member, 1, player_id);
    sqlite3_bind_int64(member, 2, season_id);
    int64_t group = 0;
    if (sqlite3_step(member) == SQLITE_ROW)
        group = sqlite3_column_int64(member, 0);
    sqlite3_reset(member);
    if (!group) {
        auto player = by_id(player_id);
        if (!player)
            return false;
        const int64_t dan = std::clamp<int64_t>(player->rank, 1, 10);
        int64_t capacity = 150;
        for (const auto &d : GameData::instance().league().dans)
            if (d.dan == dan && d.max_size > 0)
                capacity = d.max_size;
        sqlite3_stmt *free =
            cached("SELECT g.id FROM league_groups g WHERE season_id=?1 AND dan=?2 AND "
                   "(SELECT COUNT(*) FROM league_members m WHERE m.league_id=g.id) < ?3 ORDER BY "
                   "g.id LIMIT 1");
        if (!free)
            return false;
        sqlite3_bind_int64(free, 1, season_id);
        sqlite3_bind_int64(free, 2, dan);
        sqlite3_bind_int64(free, 3, capacity);
        if (sqlite3_step(free) == SQLITE_ROW)
            group = sqlite3_column_int64(free, 0);
        sqlite3_reset(free);
        if (!group) {
            const int64_t base = season_id * 1000000000LL + dan * 10000000LL;
            sqlite3_stmt *create = cached("INSERT INTO league_groups(id,season_id,dan) "
                                          "SELECT COALESCE(MAX(id),?3)+1,?1,?2 FROM league_groups "
                                          "WHERE season_id=?1 AND dan=?2");
            if (!create)
                return false;
            sqlite3_bind_int64(create, 1, season_id);
            sqlite3_bind_int64(create, 2, dan);
            sqlite3_bind_int64(create, 3, base);
            if (!exec_write(create))
                return false;
            group = sqlite3_last_insert_rowid(db_);
        }
        sqlite3_stmt *join = cached("INSERT INTO league_members VALUES(?1,?2,?3)");
        if (!join)
            return false;
        sqlite3_bind_int64(join, 1, player_id);
        sqlite3_bind_int64(join, 2, season_id);
        sqlite3_bind_int64(join, 3, group);
        if (!exec_write(join))
            return false;
    }
    sqlite3_stmt *update = cached("UPDATE players SET ts1=?2, ts2=CASE WHEN ts2 IN "
                                  "(SELECT id FROM league_groups) THEN ts2 ELSE 0 END WHERE id=?1");
    if (!update)
        return false;
    sqlite3_bind_int64(update, 1, player_id);
    sqlite3_bind_int64(update, 2, group);
    return exec_write(update) && tx.commit();
}

bool Db::append_chat(int64_t channel, int64_t scope, int64_t player, std::string_view name,
                     std::string_view text, int64_t sent_at) {
    if (!((channel == 1 && scope == 0) || (channel == 3 && scope > 0)))
        return false;
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    auto *st = cached("INSERT INTO chat_history(channel,scope_id,player_id,name,text,sent_at) "
                      "VALUES(?1,?2,?3,?4,?5,?6)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, scope);
    sqlite3_bind_int64(st, 3, player);
    bind_sv(st, 4, name);
    bind_sv(st, 5, text);
    sqlite3_bind_int64(st, 6, sent_at);
    if (!exec_write(st))
        return false;
    st = cached(
        "DELETE FROM chat_history WHERE channel=?1 AND scope_id=?2 AND id NOT IN "
        "(SELECT id FROM chat_history WHERE channel=?1 AND scope_id=?2 ORDER BY id DESC LIMIT 50)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, scope);
    return exec_write(st) && tx.commit();
}

std::vector<Db::ChatMessage> Db::chat_history(int64_t channel, int64_t scope) {
    std::vector<ChatMessage> out;
    auto *st = cached("SELECT id,player_id,name,text,sent_at FROM chat_history WHERE channel=?1 "
                      "AND scope_id=?2 ORDER BY id DESC LIMIT 50");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, scope);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ChatMessage m;
        m.id = sqlite3_column_int64(st, 0);
        m.player_id = sqlite3_column_int64(st, 1);
        m.name = reinterpret_cast<const char *>(sqlite3_column_text(st, 2));
        m.text = reinterpret_cast<const char *>(sqlite3_column_text(st, 3));
        m.sent_at = sqlite3_column_int64(st, 4);
        out.push_back(std::move(m));
    }
    sqlite3_reset(st);
    std::reverse(out.begin(), out.end());
    return out;
}

bool Db::add_first_place(int64_t player_id, int64_t season_id) {
    sqlite3_stmt *st = cached("UPDATE raid_scores SET first_places = first_places + 1 "
                              "WHERE player_id=?1 AND season_id=?2");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, season_id);
    return exec_write(st);
}

int64_t Db::newest_raid_season() {
    sqlite3_stmt *st = cached("SELECT season_id FROM raid_scores WHERE score > 0 "
                              "ORDER BY season_id DESC LIMIT 1");
    if (!st)
        return 0;
    int64_t v = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    return v;
}

Db::RaidStats Db::raid_profile_stats(int64_t player_id) {
    RaidStats out;
    auto *st = cached("SELECT raids,wins,rounds,first_places,best_rating FROM raid_profile_stats "
                      "WHERE player_id=?1");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, player_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        out.raids = sqlite3_column_int64(st, 0);
        out.wins = sqlite3_column_int64(st, 1);
        out.rounds = sqlite3_column_int64(st, 2);
        out.first_places = sqlite3_column_int64(st, 3);
        out.score = sqlite3_column_int64(st, 4);
    }
    sqlite3_reset(st);
    return out;
}

Db::RaidStats Db::raid_stats(int64_t player_id, int64_t season_id) {
    RaidStats out;
    sqlite3_stmt *st = cached("SELECT score,damage,raids,wins,best_damage,rounds,first_places "
                              "FROM raid_scores "
                              "WHERE player_id=?1 AND season_id=?2");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, season_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        out.score = sqlite3_column_int64(st, 0);
        out.damage = sqlite3_column_int64(st, 1);
        out.raids = sqlite3_column_int64(st, 2);
        out.wins = sqlite3_column_int64(st, 3);
        out.best_damage = sqlite3_column_int64(st, 4);
        out.rounds = sqlite3_column_int64(st, 5);
        out.first_places = sqlite3_column_int64(st, 6);
    }
    sqlite3_reset(st);
    return out;
}

bool Db::set_season(int64_t player_id, int64_t season_id) {
    sqlite3_stmt *st = cached("UPDATE players SET season_id=?2 WHERE id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, season_id);
    return exec_write(st);
}

bool Db::touch_seen(int64_t player_id, int64_t now_ms) {
    sqlite3_stmt *st = cached("UPDATE players SET last_login_at=?2 WHERE id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, now_ms);
    return exec_write(st);
}

bool Db::snapshot_league_places(int64_t season_id, int) {
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    auto current = current_season();
    if (!current || current->id != season_id)
        return false;
    sqlite3_stmt *st =
        cached("WITH places AS (SELECT s.player_id,s.season_id, "
               "RANK() OVER (PARTITION BY m.league_id ORDER BY s.score DESC) AS place "
               "FROM raid_scores s JOIN league_members m ON m.player_id=s.player_id AND "
               "m.season_id=s.season_id "
               "JOIN players p ON p.id=s.player_id WHERE s.season_id=?1 AND p.name<>'') "
               "UPDATE raid_scores SET prev_place=(SELECT place FROM places p WHERE "
               "p.player_id=raid_scores.player_id AND p.season_id=raid_scores.season_id) "
               "WHERE season_id=?1 AND player_id IN (SELECT player_id FROM places)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, season_id);
    return exec_write(st) && tx.commit();
}

std::optional<Clan> Db::clan_by_id(int64_t clan_id, int member_limit) {
    sqlite3_stmt *st =
        cached("SELECT id,name,emblem,description,rating,weekly_rating,type,min_rank,"
               "level,(SELECT COUNT(*) FROM clan_members m WHERE m.clan_id=clans.id) "
               "FROM clans WHERE id=?1");
    if (!st)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, clan_id);
    std::optional<Clan> out;
    if (sqlite3_step(st) == SQLITE_ROW) {
        Clan c;
        c.id = sqlite3_column_int64(st, 0);
        c.name = col_str(st, 1);
        c.emblem = col_str(st, 2);
        c.description = col_str(st, 3);
        c.rating = sqlite3_column_int64(st, 4);
        c.weekly_rating = sqlite3_column_int64(st, 5);
        c.join_type = sqlite3_column_int64(st, 6);
        c.min_rank = sqlite3_column_int64(st, 7);
        c.level = sqlite3_column_int64(st, 8);
        c.member_count = sqlite3_column_int64(st, 9);
        out = std::move(c);
    }
    sqlite3_reset(st);
    if (!out || member_limit <= 0)
        return out;

    sqlite3_stmt *ms =
        cached("SELECT m.player_id, p.name, m.role, m.contribution FROM clan_members m "
               "JOIN players p ON p.id = m.player_id WHERE m.clan_id=?1 "
               "ORDER BY m.role ASC, m.contribution DESC, m.player_id ASC LIMIT ?2");
    if (!ms)
        return out;
    sqlite3_bind_int64(ms, 1, clan_id);
    sqlite3_bind_int(ms, 2, member_limit);
    while (sqlite3_step(ms) == SQLITE_ROW) {
        ClanMember m;
        m.pid = sqlite3_column_int64(ms, 0);
        m.name = col_str(ms, 1);
        m.role = sqlite3_column_int64(ms, 2);
        m.contribution = sqlite3_column_int64(ms, 3);
        out->members.push_back(std::move(m));
    }
    sqlite3_reset(ms);
    return out;
}

std::vector<Clan> Db::clans_short(const std::vector<int64_t> &ids) {
    std::vector<Clan> out;
    size_t n = 0;
    sqlite3_stmt *st = cached(
        "SELECT id,name,emblem,level,rating,"
        "(SELECT COUNT(*) FROM clan_members m WHERE m.clan_id=clans.id) "
        "FROM clans WHERE id=?1 "
        "UNION ALL SELECT clan_id,name,emblem,level,score,member_count FROM clan_season_scores "
        "WHERE clan_id=?1 AND NOT EXISTS(SELECT 1 FROM clans WHERE id=?1) "
        "AND season_id=(SELECT MAX(season_id) FROM clan_season_scores WHERE clan_id=?1) LIMIT 1");
    if (!st)
        return out;
    for (int64_t id : ids) {
        if (++n > kMaxIdBatch)
            break;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) != SQLITE_ROW)
            continue;
        Clan c;
        c.id = sqlite3_column_int64(st, 0);
        c.name = col_str(st, 1);
        c.emblem = col_str(st, 2);
        c.level = sqlite3_column_int64(st, 3);
        c.rating = sqlite3_column_int64(st, 4);
        c.member_count = sqlite3_column_int64(st, 5);
        out.push_back(std::move(c));
    }
    sqlite3_reset(st);
    return out;
}

std::vector<int64_t> Db::clan_search(std::string_view query, bool recommended, int limit) {
    std::vector<int64_t> out;
    if (limit <= 0)
        return out;

    sqlite3_stmt *st;
    if (query.empty()) {
        st = cached("SELECT id FROM clans ORDER BY rating DESC, id ASC LIMIT ?1");
        if (!st)
            return out;
        sqlite3_bind_int(st, 1, limit);
    } else {
        st = cached("SELECT id FROM clans WHERE instr(lower(name), lower(?1)) > 0 "
                    "ORDER BY rating DESC, id ASC LIMIT ?2");
        if (!st)
            return out;
        bind_sv(st, 1, query);
        sqlite3_bind_int(st, 2, limit);
    }
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(st, 0));
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    (void)recommended;
    return out;
}

std::vector<int64_t> Db::clan_find(int limit) {
    return clan_search({}, true, limit);
}

bool Db::clan_name_taken(std::string_view name) {
    sqlite3_stmt *st = cached("SELECT 1 FROM clans WHERE name=?1 COLLATE NOCASE");
    if (!st)
        return true;
    bind_sv(st, 1, name);
    bool taken = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return taken;
}

int64_t Db::best_raid_rating(int64_t player_id) {
    sqlite3_stmt *st =
        cached("SELECT MAX(p.season_score,p.currency2,COALESCE((SELECT MAX(score) "
               "FROM raid_scores WHERE player_id=p.id),0)) FROM players p WHERE p.id=?1");
    if (!st)
        return 0;
    sqlite3_bind_int64(st, 1, player_id);
    int64_t score = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        score = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    return score;
}

int64_t Db::clan_of(int64_t player_id) {
    sqlite3_stmt *st = cached("SELECT clan_id FROM clan_members WHERE player_id=?1");
    if (!st)
        return 0;
    sqlite3_bind_int64(st, 1, player_id);
    int64_t id = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        id = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    return id;
}

bool Db::clan_recalculate_rating(int64_t clan_id, int64_t *delta) {
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    auto clan = clan_by_id(clan_id, 0);
    if (!clan)
        return false;
    sqlite3_stmt *st = cached("SELECT contribution FROM clan_members WHERE clan_id=?1 "
                              "ORDER BY contribution DESC,player_id ASC");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, clan_id);
    const auto &limits = GameData::instance().clan();
    long double rating = 0;
    int64_t position = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        ++position;
        double weight = limits.rating_weights.empty() ? 1.0 : limits.weight_for_position(position);
        rating += static_cast<long double>(sqlite3_column_int64(st, 0)) * weight;
    }
    sqlite3_reset(st);
    const int64_t total = rating >= static_cast<long double>(std::numeric_limits<int64_t>::max())
                              ? std::numeric_limits<int64_t>::max()
                              : int64_t(std::round(std::max(0.0L, rating)));
    st = cached("UPDATE clans SET rating=?2 WHERE id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, clan_id);
    sqlite3_bind_int64(st, 2, total);
    if (!exec_write(st) || !tx.commit())
        return false;
    if (delta)
        *delta = total - clan->rating;
    return true;
}

bool Db::clan_add_contribution(int64_t player_id, int64_t delta, int64_t &clan_id,
                               int64_t &member_total) {
    clan_id = 0;
    member_total = 0;
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    sqlite3_stmt *st = cached("UPDATE clan_members SET contribution = MAX(0, contribution + ?2) "
                              "WHERE player_id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, delta);
    if (!exec_write(st))
        return false;
    if (sqlite3_changes(db_) == 0)
        return false;

    sqlite3_stmt *rd = cached("SELECT clan_id, contribution FROM clan_members WHERE player_id=?1");
    if (!rd)
        return false;
    sqlite3_bind_int64(rd, 1, player_id);
    bool ok = false;
    if (sqlite3_step(rd) == SQLITE_ROW) {
        clan_id = sqlite3_column_int64(rd, 0);
        member_total = sqlite3_column_int64(rd, 1);
        ok = true;
    }
    sqlite3_reset(rd);
    if (!ok || !clan_id)
        return ok;

    int64_t rating_delta = 0;
    if (!clan_recalculate_rating(clan_id, &rating_delta))
        return false;
    sqlite3_stmt *cs = cached("UPDATE clans SET weekly_rating=MAX(0,weekly_rating+?2) WHERE id=?1");
    if (!cs)
        return false;
    sqlite3_bind_int64(cs, 1, clan_id);
    sqlite3_bind_int64(cs, 2, rating_delta);
    return exec_write(cs) && tx.commit();
}

std::vector<int64_t> Db::clan_member_ids(int64_t clan_id, int limit) {
    std::vector<int64_t> out;
    if (!clan_id || limit <= 0)
        return out;
    sqlite3_stmt *st = cached("SELECT player_id FROM clan_members WHERE clan_id=?1 "
                              "ORDER BY player_id ASC LIMIT ?2");
    if (!st)
        return out;
    sqlite3_bind_int64(st, 1, clan_id);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(st, 0));
    sqlite3_reset(st);
    return out;
}

std::optional<int64_t> Db::clan_member_role(int64_t clan_id, int64_t player_id) {
    sqlite3_stmt *st = cached("SELECT role FROM clan_members WHERE clan_id=?1 AND player_id=?2");
    if (!st)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, clan_id);
    sqlite3_bind_int64(st, 2, player_id);
    std::optional<int64_t> out;
    if (sqlite3_step(st) == SQLITE_ROW)
        out = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return out;
}

int64_t Db::clan_create(std::string_view name, std::string_view emblem,
                        std::string_view description, int64_t join_type, int64_t min_rank,
                        int64_t leader, int64_t price, int64_t *remaining_gems) {
    ClanTransaction tx(db_);
    if (!tx || leader <= 0 || price < 0 || clan_of(leader) != 0 || clan_name_taken(name))
        return 0;
    int64_t balance = 0;
    if (!spend_gems(leader, price, balance))
        return 0;
    sqlite3_stmt *st =
        cached("INSERT INTO clans(name,emblem,description,type,min_rank,level,created_at) "
               "VALUES(?1,?2,?3,?4,?5,1,?6)");
    if (!st)
        return 0;
    bind_sv(st, 1, name);
    bind_sv(st, 2, emblem);
    bind_sv(st, 3, description);
    sqlite3_bind_int64(st, 4, join_type);
    sqlite3_bind_int64(st, 5, min_rank);
    sqlite3_bind_int64(st, 6, wall_now());
    if (!exec_write(st))
        return 0;
    const int64_t id = sqlite3_last_insert_rowid(db_);
    if (leader > 0 && !clan_join(id, leader, kClanLeader)) {
        clan_delete(id);
        return 0;
    }
    if (!tx.commit())
        return 0;
    if (remaining_gems)
        *remaining_gems = balance;
    return id;
}

bool Db::clan_set_role(int64_t clan_id, int64_t player_id, int64_t role) {
    sqlite3_stmt *st = cached("UPDATE clan_members SET role=?3 WHERE clan_id=?1 AND player_id=?2");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, clan_id);
    sqlite3_bind_int64(st, 2, player_id);
    sqlite3_bind_int64(st, 3, role);
    if (!exec_write(st))
        return false;
    return sqlite3_changes(db_) > 0;
}

bool Db::clan_set_leader(int64_t clan_id, int64_t new_leader, int64_t old_leader) {
    ClanTransaction tx(db_);
    if (!tx || old_leader == new_leader || clan_member_role(clan_id, old_leader) != kClanLeader ||
        !clan_member_role(clan_id, new_leader))
        return false;
    if (!clan_set_role(clan_id, old_leader, kClanOfficer) ||
        !clan_set_role(clan_id, new_leader, kClanLeader))
        return false;
    return tx.commit();
}

bool Db::clan_update_profile(int64_t clan_id, std::string_view description, std::string_view emblem,
                             int64_t join_type, int64_t min_rank) {
    sqlite3_stmt *st = cached("UPDATE clans SET description=?2, emblem=?3, type=?4, min_rank=?5 "
                              "WHERE id=?1");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, clan_id);
    bind_sv(st, 2, description);
    bind_sv(st, 3, emblem);
    sqlite3_bind_int64(st, 4, join_type);
    sqlite3_bind_int64(st, 5, min_rank);
    if (!exec_write(st))
        return false;
    return sqlite3_changes(db_) > 0;
}

bool Db::clan_remove_member(int64_t clan_id, int64_t player_id) {
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    sqlite3_stmt *st = cached("DELETE FROM clan_members WHERE clan_id=?1 AND player_id=?2");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, clan_id);
    sqlite3_bind_int64(st, 2, player_id);
    if (!exec_write(st))
        return false;
    if (sqlite3_changes(db_) == 0)
        return false;

    sqlite3_stmt *ps = cached("UPDATE players SET clan_id=0 WHERE id=?1 AND clan_id=?2");
    if (!ps)
        return false;
    sqlite3_bind_int64(ps, 1, player_id);
    sqlite3_bind_int64(ps, 2, clan_id);
    return exec_write(ps) && clan_recalculate_rating(clan_id) && tx.commit();
}

bool Db::clan_delete(int64_t clan_id) {
    ClanTransaction tx(db_);
    if (!tx)
        return false;
    for (const char *sql :
         {"UPDATE players SET clan_id=0 WHERE clan_id=?1",
          "DELETE FROM clan_members WHERE clan_id=?1",
          "DELETE FROM clan_kick_cooldowns WHERE clan_id=?1", "DELETE FROM clans WHERE id=?1"}) {
        sqlite3_stmt *st = cached(sql);
        if (!st)
            return false;
        sqlite3_bind_int64(st, 1, clan_id);
        if (!exec_write(st))
            return false;
    }
    return tx.commit();
}

bool Db::spend_gems(int64_t player_id, int64_t cost, int64_t &now_gems) {
    if (cost < 0)
        return false;
    now_gems = 0;
    sqlite3_stmt *st = cached("UPDATE players SET gems = gems - ?2, updated_at=?3 "
                              "WHERE id=?1 AND gems >= ?2");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, cost);
    sqlite3_bind_int64(st, 3, wall_now());
    if (!exec_write(st))
        return false;
    const bool paid = sqlite3_changes(db_) > 0;

    sqlite3_stmt *rd = cached("SELECT gems FROM players WHERE id=?1");
    if (!rd)
        return false;
    sqlite3_bind_int64(rd, 1, player_id);
    if (sqlite3_step(rd) == SQLITE_ROW)
        now_gems = sqlite3_column_int64(rd, 0);
    sqlite3_reset(rd);
    sqlite3_clear_bindings(rd);
    return paid;
}

bool Db::clan_kick_set(int64_t clan_id, int64_t player_id, int kind, int64_t until_ms) {
    sqlite3_stmt *st =
        cached("INSERT OR REPLACE INTO clan_kick_cooldowns(clan_id,player_id,kind,until_ms) "
               "VALUES(?1,?2,?3,?4)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, clan_id);
    sqlite3_bind_int64(st, 2, player_id);
    sqlite3_bind_int64(st, 3, kind);
    sqlite3_bind_int64(st, 4, until_ms);
    return exec_write(st);
}

bool Db::clan_kick_until(int64_t clan_id, int64_t player_id, int kind, int64_t &until_ms) {
    until_ms = 0;
    sqlite3_stmt *st = cached("SELECT until_ms FROM clan_kick_cooldowns "
                              "WHERE clan_id=?1 AND player_id=?2 AND kind=?3");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, clan_id);
    sqlite3_bind_int64(st, 2, player_id);
    sqlite3_bind_int64(st, 3, kind);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        until_ms = sqlite3_column_int64(st, 0);
        found = true;
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return found;
}

bool Db::clan_join(int64_t clan_id, int64_t player_id, int64_t role, int64_t max_members) {
    ClanTransaction tx(db_);
    if (!tx || role < kClanLeader || role > kClanNovice || max_members <= 0)
        return false;
    sqlite3_stmt *st =
        cached("INSERT INTO clan_members(player_id,clan_id,role,contribution,joined_at) "
               "SELECT ?1,c.id,?3,0,?4 FROM clans c JOIN players p ON p.id=?1 "
               "WHERE c.id=?2 AND (SELECT COUNT(*) FROM clan_members WHERE clan_id=c.id) < ?5 "
               "AND NOT EXISTS(SELECT 1 FROM clan_members WHERE player_id=?1)");
    if (!st)
        return false;
    sqlite3_bind_int64(st, 1, player_id);
    sqlite3_bind_int64(st, 2, clan_id);
    sqlite3_bind_int64(st, 3, role);
    sqlite3_bind_int64(st, 4, wall_now());
    sqlite3_bind_int64(st, 5, max_members);
    if (!exec_write(st) || sqlite3_changes(db_) != 1)
        return false;
    sqlite3_stmt *ps = cached("UPDATE players SET clan_id=?2 WHERE id=?1");
    if (!ps)
        return false;
    sqlite3_bind_int64(ps, 1, player_id);
    sqlite3_bind_int64(ps, 2, clan_id);
    return exec_write(ps) && tx.commit();
}

}
