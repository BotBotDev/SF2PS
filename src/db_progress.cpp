#include "sf2/db.hpp"
#include "sf2/gamedata.hpp"
#include "sf2/protobuf.hpp"
#include "sf2/raid.hpp"
#include "sf2/social.hpp"
#include "sf2/crypto.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <set>
#include <ctime>

namespace sf2 {
namespace {
struct Tx {
    sqlite3 *db;
    bool active = false;

    Tx(sqlite3 *d) : db(d) {
        active =
            sqlite3_exec(db, "SAVEPOINT progress_write", nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    ~Tx() {
        if (active)
            sqlite3_exec(db, "ROLLBACK TO progress_write; RELEASE progress_write", nullptr, nullptr,
                         nullptr);
    }

    bool commit() {
        if (!active ||
            sqlite3_exec(db, "RELEASE progress_write", nullptr, nullptr, nullptr) != SQLITE_OK)
            return false;
        active = false;
        return true;
    }

    explicit operator bool() const {
        return active;
    }
};

int64_t now_ms() {
    timespec t{};
    clock_gettime(CLOCK_REALTIME, &t);
    return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

struct Slot {
    int64_t slot = 0, season = 0, id = 0, start = 0, done = 0, progress = 0, target = 0,
            renewed = 0, generation = 0;
    std::vector<uint8_t> loot;
};

std::vector<Slot> read_slots(sqlite3 *db, int64_t id) {
    std::vector<Slot> out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(
            db,
            "SELECT "
            "slot,season_id,quest_id,started_ms,completed_ms,progress,target,renewed_ms,generation,"
            "loot FROM daily_quests WHERE player_id=? ORDER BY slot",
            -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Slot s;
        int64_t *fields[] = {&s.slot,     &s.season, &s.id,      &s.start,     &s.done,
                             &s.progress, &s.target, &s.renewed, &s.generation};
        for (int i = 0; i < 9; ++i)
            *fields[i] = sqlite3_column_int64(st, i);
        const auto *b = static_cast<const uint8_t *>(sqlite3_column_blob(st, 9));
        int n = sqlite3_column_bytes(st, 9);
        if (b && n > 0)
            s.loot.assign(b, b + n);
        out.push_back(std::move(s));
    }
    sqlite3_finalize(st);
    return out;
}

RaidChest chest(const std::string &name, const LootRoll &r) {
    RaidChest c;
    c.name = name;
    c.items = r.currency;
    c.spheres = r.consumables;
    c.gems = r.gems;
    c.rating = r.rating;
    return c;
}

LootRoll read_loot(const std::vector<uint8_t> &bytes) {
    LootRoll out;
    pb::scan(ByteView(bytes), [&](const pb::Field &f) {
        if (f.wire == pb::Wire::Bytes && (f.number == 2 || f.number == 3)) {
            NamedCount item;
            item.name = std::string(pb::get_bytes(f.data, 1).sv());
            item.count = int64_t(pb::get_varint(f.data, 2, 0));
            if (!item.name.empty() && item.count > 0)
                (f.number == 2 ? out.currency : out.consumables).push_back(std::move(item));
        } else if (f.number == 4)
            out.gems = int64_t(f.num);
        else if (f.number == 5)
            out.rating = int64_t(f.num);
        return true;
    });
    return out;
}

int64_t count(const Player &p, const std::string &name) {
    for (const auto &n : p.items)
        if (n.name == name)
            return n.count;
    return 0;
}
}

int Db::pay_raid_round(int64_t id, int64_t price, const std::vector<std::string> &potions,
                       Player &out) {
    if (price < 0 || potions.size() > 16)
        return 10003;
    Tx tx(db_);
    if (!tx)
        return 10003;
    auto p = by_id(id);
    if (!p)
        return 10000;
    if (p->gems < price)
        return 10004;
    std::set<std::string> unique;
    for (const auto &name : potions) {
        if (!GameData::instance().potion(name) || !unique.insert(name).second)
            return 10003;
        auto item = std::find_if(p->perks.begin(), p->perks.end(),
                                 [&](const auto &v) { return v.name == name; });
        if (item == p->perks.end() || item->count <= 0)
            return 10003;
        --item->count;
    }
    p->gems -= price;
    if ((price && !set_currencies(id, p->gems, p->currency2)) ||
        (!potions.empty() && !set_perks(id, p->perks)) || !tx.commit())
        return 10003;
    out = std::move(*p);
    return 0;
}

void Db::load_player_progress(Player &p) {
    auto slots = read_slots(db_, p.id);
    Buffer daily;

    pb::v(daily, 1, uint64_t(p.season_id));
    for (const auto &q : slots)
        if (q.season == p.season_id) {
            pb::Sub row(daily, 2);
            pb::v(daily, 1, uint64_t(q.id));
            {
                pb::Sub t(daily, 2);
                pb::v(daily, 1, uint64_t(q.start));
            }
            if (q.done) {
                pb::Sub t(daily, 3);
                pb::v(daily, 1, uint64_t(q.done));
            }
            pb::v(daily, 4, uint64_t(q.progress));
            pb::v(daily, 5, uint64_t(q.target));
            if (!q.loot.empty())
                pb::bin(daily, 6, ByteView(q.loot));
            if (q.renewed) {
                pb::Sub t(daily, 7);
                pb::v(daily, 1, uint64_t(q.renewed));
            }
        }
    if (!slots.empty())
        p.daily_payload = daily.vec();
    if (const auto *pass = GameData::instance().battle_pass().active(now_ms())) {
        p.bp_name = pass->name;
        auto *st = cached("SELECT claimed FROM player_battle_pass WHERE player_id=?1 AND name=?2");
        if (st) {
            sqlite3_bind_int64(st, 1, p.id);
            sqlite3_bind_text(st, 2, p.bp_name.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW)
                p.bp_claimed = uint64_t(sqlite3_column_int64(st, 0));
            sqlite3_reset(st);
        }
    }
}

bool Db::apply_progress_loot(Player &p, const LootRoll &loot) {
    auto merge = [](auto &dst, const auto &add) {
        for (const auto &n : add) {
            auto it = std::find_if(dst.begin(), dst.end(),
                                   [&](const auto &v) { return v.name == n.name; });
            if (it == dst.end())
                dst.push_back(n);
            else
                it->count += n.count;
        }
    };
    merge(p.items, loot.currency);
    merge(p.perks, loot.consumables);
    p.gems += loot.gems;
    if (!set_items(p.id, p.items) || !set_perks(p.id, p.perks) ||
        !set_currencies(p.id, p.gems, p.currency2))
        return false;
    if (loot.rating) {
        int64_t clan = 0, total = 0;
        if (!add_currency2(p.id, loot.rating, p.currency2) ||
            !add_season_score(p.id, loot.rating, p.season_score) ||
            !add_raid_score(p.id, p.season_id, loot.rating, 0, false, false))
            return false;
        if (p.clan_id && !clan_add_contribution(p.id, loot.rating, clan, total))
            return false;
        p.raid_score += loot.rating;
    }
    return true;
}

bool Db::refresh_daily(int64_t id, int64_t now, int64_t renew_id, bool force, int64_t offered,
                       int *error) {
    if (error)
        *error = 10008;
    Tx tx(db_);
    if (!tx)
        return false;
    auto p = by_id(id);
    if (!p)
        return false;
    const auto &settings = GameData::instance().daily();
    if (settings.quests.empty())
        return tx.commit();
    auto rows = read_slots(db_, id);
    const auto season = current_season();
    int64_t season_end = season ? season->ends_ms : INT64_MAX;
    bool found = renew_id == 0;
    for (int slot = 0; slot < 3; ++slot) {
        auto old =
            std::find_if(rows.begin(), rows.end(), [&](const auto &q) { return q.slot == slot; });
        bool replace = old == rows.end() || old->season != p->season_id;
        int64_t renewed = old == rows.end() ? 0 : old->renewed;
        if (old != rows.end() && old->season != p->season_id)
            renewed = 0;
        if (!replace) {
            const auto *rule = settings.quest(old->id);
            replace = !rule || !rule->eligible(p->rank) ||
                      (old->done && now >= std::min(old->done + settings.replace_ms, season_end));
        }
        if (old != rows.end() && old->id == renew_id && old->season == p->season_id) {
            found = true;
            if (force) {
                if (!old->done || settings.cd_unit_s <= 0 || settings.cd_cost <= 0)
                    return false;
                int64_t remaining =
                    (std::min(old->done + settings.replace_ms, season_end) / 1000) - (now / 1000);
                int64_t cost =
                    remaining > 0 ? (remaining / settings.cd_unit_s + 1) * settings.cd_cost : 0;
                if (offered < cost)
                    return false;
                if (cost && !spend_gems(id, cost, p->gems)) {
                    if (error)
                        *error = 10004;
                    return false;
                }
            } else {
                if (renewed && now < renewed + settings.renew_ms)
                    return false;
                renewed = now;
            }
            replace = true;
        }
        if (!replace) {

            if (old->loot.empty() && !old->done) {
                const auto *rule = settings.quest(old->id);
                LootRoll roll;
                if (!rule || !GameData::instance().roll(rule->reward, roll))
                    return false;
                Buffer b;
                proto::build_loot(b, chest(rule->reward, roll));
                old->loot = b.vec();
                auto *save =
                    cached("UPDATE daily_quests SET loot=?3 WHERE player_id=?1 AND slot=?2");
                if (!save)
                    return false;
                sqlite3_bind_int64(save, 1, id);
                sqlite3_bind_int64(save, 2, slot);
                sqlite3_bind_blob(save, 3, old->loot.data(), int(old->loot.size()),
                                  SQLITE_TRANSIENT);
                if (!exec_write(save))
                    return false;
            }
            continue;
        }
        std::set<std::string> groups;
        for (const auto &other : rows)
            if (other.slot != slot && other.season == p->season_id)
                if (const auto *rule = settings.quest(other.id))
                    groups.insert(rule->group);
        std::vector<const DailyRule *> candidates;
        for (const auto &rule : settings.quests)
            if (rule.eligible(p->rank) && !groups.count(rule.group) &&
                (old == rows.end() || rule.id != old->id))
                candidates.push_back(&rule);
        if (candidates.empty())
            for (const auto &rule : settings.quests)
                if (rule.eligible(p->rank) && !groups.count(rule.group))
                    candidates.push_back(&rule);
        if (candidates.empty())
            return false;
        uint32_t random = 0;
        crypto::random_bytes(reinterpret_cast<uint8_t *>(&random), sizeof random);
        const auto &chosen = *candidates[random % candidates.size()];
        Slot q;
        q.slot = slot;
        q.season = p->season_id;
        q.id = chosen.id;
        q.start = now;
        q.target = chosen.target;
        q.renewed = renewed;
        q.generation = old == rows.end() ? 1 : old->generation + 1;
        LootRoll reward;
        if (!GameData::instance().roll(chosen.reward, reward))
            return false;
        Buffer preview;
        proto::build_loot(preview, chest(chosen.reward, reward));
        q.loot = preview.vec();
        auto *st =
            cached("INSERT INTO "
                   "daily_quests(player_id,slot,season_id,quest_id,started_ms,target,renewed_ms,"
                   "generation,loot) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) ON "
                   "CONFLICT(player_id,slot) DO UPDATE SET "
                   "season_id=excluded.season_id,quest_id=excluded.quest_id,started_ms=excluded."
                   "started_ms,completed_ms=0,progress=0,target=excluded.target,renewed_ms="
                   "excluded.renewed_ms,generation=excluded.generation,loot=excluded.loot");
        if (!st)
            return false;
        int64_t values[] = {id, slot, q.season, q.id, now, q.target, q.renewed, q.generation};
        for (int i = 0; i < 8; ++i)
            sqlite3_bind_int64(st, i + 1, values[i]);
        sqlite3_bind_blob(st, 9, q.loot.data(), int(q.loot.size()), SQLITE_TRANSIENT);
        if (!exec_write(st))
            return false;
        if (old == rows.end())
            rows.push_back(q);
        else
            *old = q;
    }
    if (!found)
        return false;
    return tx.commit();
}

bool Db::record_daily(int64_t id, const DailyEvent &event, int64_t now) {
    Tx tx(db_);
    if (!tx)
        return false;
    auto p = by_id(id);
    if (!p)
        return false;
    auto *once = cached(
        "INSERT OR IGNORE INTO daily_events(player_id,battle_id,kind,round) VALUES(?1,?2,?3,?4)");
    if (!once)
        return false;
    sqlite3_bind_int64(once, 1, id);
    sqlite3_bind_int64(once, 2, event.battle);
    sqlite3_bind_int(once, 3, event.finished ? 1 : 0);
    sqlite3_bind_int64(once, 4, event.round);
    if (!exec_write(once))
        return false;
    if (!sqlite3_changes(db_))
        return tx.commit();
    auto rows = read_slots(db_, id);
    for (auto &q : rows) {
        const auto *rule = GameData::instance().daily().quest(q.id);
        if (q.done || q.season != p->season_id || !rule ||
            (rule->raid >= 0 && rule->raid != event.raid))
            continue;
        int64_t delta = 0;
        const auto &type = rule->trigger;
        if (event.finished) {
            if (event.won && type == "RaidWinPerSeason")
                delta = 1;
            if (event.won && type == "RaidPlacePerSeason" &&
                event.place == (rule->length > 0 ? rule->length : 1))
                delta = 1;
        } else {
            if (type == "DamagePerRound" || type == "DamagePerBattle" || type == "DamagePerSeason")
                delta = std::max<int64_t>(0, event.damage);
            else if (type == "CritPerBattle" || type == "CritPerSeason")
                delta = std::max<int64_t>(0, event.crits);
            else if (type == "SurvivePerBattle" || type == "SurvivePerSeason")
                delta = event.survived ? 1 : 0;
            else if (type == "StylePerBattle" || type == "StylePerSeason")
                delta = event.style >= rule->style ? 1 : 0;
            else if (type == "HitsPerBattle" || type == "HitsPerSeason") {
                for (const auto &hit : event.hits)
                    if (hit.first == rule->damage_type)
                        delta += hit.second;
            } else if (type == "ComboPerBattle" || type == "ComboPerSeason") {
                for (const auto &combo : event.combos)
                    if (combo.first >= rule->length)
                        delta += combo.second;
            }
        }
        if (delta <= 0)
            continue;
        int64_t progress = q.progress + delta;
        if (type == "DamagePerRound")
            progress = std::max(q.progress, delta);
        else if (type.find("PerBattle") != std::string::npos) {
            auto *st = cached(
                "INSERT INTO daily_battle_progress(player_id,slot,generation,battle_id,value) "
                "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(player_id,slot,generation,battle_id) DO UPDATE "
                "SET value=value+excluded.value RETURNING value");
            if (!st)
                return false;
            int64_t values[] = {id, q.slot, q.generation, event.battle, delta};
            for (int i = 0; i < 5; ++i)
                sqlite3_bind_int64(st, i + 1, values[i]);
            if (sqlite3_step(st) != SQLITE_ROW) {
                sqlite3_reset(st);
                return false;
            }
            progress = std::max<int64_t>(q.progress, sqlite3_column_int64(st, 0));
            sqlite3_reset(st);
        }
        q.progress = std::min(q.target, progress);
        if (q.progress >= q.target) {
            LootRoll roll;
            if (q.loot.empty()) {
                if (!GameData::instance().roll(rule->reward, roll))
                    return false;
                Buffer b;
                proto::build_loot(b, chest(rule->reward, roll));
                q.loot = b.vec();
            } else
                roll = read_loot(q.loot);
            if (!apply_progress_loot(*p, roll))
                return false;
            q.done = now;
        }
        auto *st = cached("UPDATE daily_quests SET progress=?3,completed_ms=?4,loot=?5 WHERE "
                          "player_id=?1 AND slot=?2");
        if (!st)
            return false;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, q.slot);
        sqlite3_bind_int64(st, 3, q.progress);
        sqlite3_bind_int64(st, 4, q.done);
        if (q.loot.empty())
            sqlite3_bind_zeroblob(st, 5, 0);
        else
            sqlite3_bind_blob(st, 5, q.loot.data(), int(q.loot.size()), SQLITE_TRANSIENT);
        if (!exec_write(st))
            return false;
    }
    return tx.commit();
}

int Db::claim_battle_pass(int64_t id, std::string_view name, int64_t index, int64_t now,
                          std::vector<RaidChest> &out) {
    out.clear();
    const auto &settings = GameData::instance().battle_pass();
    const auto *pass = settings.active(now);
    if (!pass)
        return 10200;
    if (pass->name != name)
        return 10201;
    if (index < -1 || index >= int64_t(pass->levels.size()))
        return 10205;
    Tx tx(db_);
    if (!tx)
        return 10200;
    auto p = by_id(id);
    if (!p)
        return 10000;
    uint64_t claimed = 0;
    auto *read = cached("SELECT claimed FROM player_battle_pass WHERE player_id=?1 AND name=?2");
    if (!read)
        return 10200;
    sqlite3_bind_int64(read, 1, id);
    sqlite3_bind_text(read, 2, pass->name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(read) == SQLITE_ROW)
        claimed = uint64_t(sqlite3_column_int64(read, 0));
    sqlite3_reset(read);
    const auto points = count(*p, settings.points);
    const bool premium = count(*p, settings.premium) > 0;
    std::vector<RaidChest> rewards;
    for (size_t i = 0; i < pass->levels.size(); ++i) {
        if (index >= 0 && size_t(index) != i)
            continue;
        const auto &level = pass->levels[i];
        if (claimed & (uint64_t(1) << i)) {
            if (index >= 0)
                return 10202;
            continue;
        }
        if (points < level.points) {
            if (index >= 0)
                return 10203;
            continue;
        }
        if (level.premium && !premium) {
            if (index >= 0)
                return 10204;
            continue;
        }
        LootRoll roll;
        if (!GameData::instance().roll(level.loot, roll) || roll.empty())
            return 10200;
        if (!apply_progress_loot(*p, roll))
            return 10200;
        rewards.push_back(chest(level.loot, roll));
        claimed |= uint64_t(1) << i;
    }
    if (rewards.empty())
        return 10202;
    auto *save = cached("INSERT INTO player_battle_pass(player_id,name,claimed) VALUES(?1,?2,?3) "
                        "ON CONFLICT(player_id,name) DO UPDATE SET claimed=excluded.claimed");
    if (!save)
        return 10200;
    sqlite3_bind_int64(save, 1, id);
    sqlite3_bind_text(save, 2, pass->name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(save, 3, int64_t(claimed));
    if (!exec_write(save) || !tx.commit())
        return 10200;
    out = std::move(rewards);
    return 0;
}
}
