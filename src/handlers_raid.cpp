

#include <ctime>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "sf2/handlers.hpp"
#include "sf2/schedule.hpp"
#include "sf2/log.hpp"
#include "sf2/protobuf.hpp"
#include "sf2/gamedata.hpp"
#include "sf2/raid.hpp"
#include "sf2/raid_report.hpp"
#include "sf2/responses.hpp"
#include "sf2/server.hpp"
#include "sf2/social.hpp"
#include "sf2/validate.hpp"

namespace sf2 {
namespace {

using Packet = std::shared_ptr<const std::vector<uint8_t>>;
std::recursive_mutex g_season_ops;

thread_local Buffer t_out;
thread_local Buffer t_pr;

Buffer &out_buf() {
    t_out.clear();
    return t_out;
}

Buffer &proto_buf() {
    t_pr.clear();
    return t_pr;
}

void reply(Ctx &x, const Buffer &b) {
    x.w.send(x.s, b.view());
}

void reply_ext(Ctx &x, std::string_view cmd, ByteView body = {}, bool compress = false) {
    Buffer &b = out_buf();
    resp::ext(b, cmd, x.req.req_id, body, compress);
    reply(x, b);
}

Packet make_event(std::string_view cmd, ByteView body, bool compress = false) {
    Buffer b;
    resp::ext_event(b, cmd, body, compress);
    return std::make_shared<const std::vector<uint8_t>>(b.vec());
}

void push(Worker &w, int64_t pid, const Packet &p) {
    if (p)
        w.hub().send_to_player(pid, p);
}

void push_party(Worker &w, const RaidBattle &b, const Packet &p, int64_t exclude = 0) {
    if (!p)
        return;
    for (const RaidParticipant &q : b.players)
        if (q.pid != exclude)
            w.hub().send_to_player(q.pid, p);
}

int64_t presence_of(Worker &w, int64_t pid) {
    if (is_raid_bot(pid))
        return kPresenceInRaid;
    if (!w.hub().is_online(pid))
        return kPresenceOffline;
    RaidEngine &e = RaidEngine::instance();
    if (e.battle_of(pid) || e.is_queued(pid))
        return kPresenceInRaid;
    return kPresenceOnline;
}

bool short_of(Worker &w, int64_t pid, ShortPlayer &out) {
    auto row = w.db().short_player(pid);
    if (!row)
        return false;
    out = std::move(*row);
    out.presence = presence_of(w, pid);
    return true;
}

Packet player_event(Worker &w, int64_t pid) {
    ShortPlayer sp;
    if (!short_of(w, pid, sp))
        return nullptr;
    Buffer pr;
    proto::build_short_player(pr, sp);
    return make_event("refresh_player_event", pr.view());
}

std::vector<int64_t> presence_audience(Worker &w, const Session &s) {
    std::vector<int64_t> out;
    RaidBattle b;
    if (RaidEngine::instance().snapshot_of_player(s.player_id, b))
        for (const RaidParticipant &p : b.players)
            if (p.pid != s.player_id)
                out.push_back(p.pid);

    if (s.player.clan_id) {
        for (int64_t id : w.db().clan_member_ids(s.player.clan_id, 100))
            if (id != s.player_id)
                out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return w.hub().online_of(out);
}

void announce_presence(Worker &w, Session &s) {
    Packet p = player_event(w, s.player_id);
    if (!p)
        return;
    for (int64_t pid : presence_audience(w, s))
        push(w, pid, p);
}

Packet raid_player_event(const RaidParticipant &p) {
    Buffer pr;
    proto::build_raid_player(pr, p);
    return make_event("refresh_raid_player_event", pr.view());
}

Packet raid_start_event(const RaidBattle &b) {
    Buffer pr;
    proto::build_raid_info(pr, b);
    return make_event("raid_battle_start_event", pr.view());
}

int64_t league_id_of(const Player &p) {
    return p.season_id;
}

int64_t next_attempt(Ctx &x) {
    return RaidEngine::instance().next_attempt_ms(x.s.player.id, x.wall_ms);
}

int64_t league_ends_ms(const Config &cfg, int64_t wall_ms) {
    const std::string &cron = GameData::instance().league().schedule_cron;
    int64_t when = 0;
    if (!cron.empty() && cron_next_ms(cron, cfg.league_tz_offset_s, wall_ms, when))
        return when;
    const int64_t period = int64_t(cfg.league_period_s ? cfg.league_period_s : 86400) * 1000;
    return ((wall_ms / period) + 1) * period;
}

int64_t league_version(const Config &cfg, int64_t league_id) {
    if (cfg.league_content_version > 0)
        return league_id * 1000000000LL + cfg.league_content_version % 1000000000LL;

    return league_id * 1000000000LL + 30000168LL;
}

void on_get_league(Ctx &x) {

    const int64_t version = int64_t(pb::get_varint(x.req.body, 1, 0));
    Buffer pr;
    const auto season = x.w.db().season_by_version(version);
    LOG_DEBUG("get_league player=%lld requested=%lld current=%lld previous=%lld found=%d",
              (long long)x.s.player_id, (long long)version, (long long)x.s.player.ts1,
              (long long)x.s.player.ts2, int(bool(season)));
    if (season && (version == x.s.player.ts1 || version == x.s.player.ts2)) {
        LeagueBoard board;
        board.league_id = season->id;
        board.ends_ms = season->ends_ms;
        board.content_version = season->content_version;
        board.entries = x.w.db().league_board(version, 150);
        pb::Sub sub(pr, 1);
        proto::build_league(pr, board);
    }
    reply_ext(x, "get_league", pr.view(), true);
}

Packet league_event(Ctx &x) {
    const int64_t league = league_id_of(x.s.player);
    LeagueBoard board;
    board.league_id = league;
    const auto current = x.w.db().current_season();
    if (!current)
        return nullptr;
    board.ends_ms = current->ends_ms;
    board.content_version = x.s.player.ts1;
    board.entries = x.w.db().league_board(x.s.player.ts1, 150);
    if (board.entries.empty())
        return nullptr;

    Buffer pr;
    proto::build_league(pr, board);
    return make_event("refresh_league_event", pr.view(), true);
}

Packet profile_event(Ctx &x) {
    Buffer pr;
    proto::build_player(pr, x.s.player, x.wall_ms);
    return make_event("get_player", pr.view(), true);
}

void push_profile(Ctx &x) {
    if (!x.cfg.push_profile_updates)
        return;
    if (Packet p = profile_event(x))
        push(x.w, x.s.player.id, p);
    Buffer gems;
    resp::proto_varint(gems, x.s.player.gems);
    push(x.w, x.s.player.id, make_event("refresh_gems", gems.view()));
}

Packet clan_rating_event(int64_t pid, int64_t rating) {
    Buffer pr;
    pb::v(pr, 1, uint64_t(pid));
    pb::v(pr, 2, uint64_t(rating));
    return make_event("clan_member_rating_event", pr.view());
}

void spend_charges(Ctx &x, const std::vector<NamedCount> &spent) {
    if (spent.empty())
        return;
    bool changed = false;
    for (const NamedCount &s : spent) {
        for (NamedCount &p : x.s.player.perks) {
            if (p.name != s.name)
                continue;
            const int64_t take = std::min(s.count, p.count);
            if (take <= 0)
                break;
            p.count -= take;
            changed = true;
            LOG_INFO("[%s] player %lld spent %lld x %s (%lld left)", x.s.remote().c_str(),
                     (long long)x.s.player.id, (long long)take, s.name.c_str(), (long long)p.count);
            break;
        }
    }
    if (changed) {
        x.w.db().set_perks(x.s.player.id, x.s.player.perks);
        push_profile(x);
    }
}

void credit_rating(Ctx &x, int64_t delta, int64_t damage, bool entered_raid, bool won) {
    if (delta <= 0 && damage <= 0)
        return;
    std::lock_guard<std::recursive_mutex> operation(g_season_ops);
    sync_season_clock(x.w);
    sync_session_season(x.w, x.s);

    int64_t currency = x.s.player.currency2;
    int64_t clan_id = 0, member_total = 0;
    if (delta > 0) {
        x.w.db().add_currency2(x.s.player.id, delta, currency);
        x.s.player.currency2 = currency;

        int64_t season = x.s.player.season_score;
        x.w.db().add_season_score(x.s.player.id, delta, season);
        x.s.player.season_score = season;

        x.w.db().clan_add_contribution(x.s.player.id, delta, clan_id, member_total);
    }
    x.w.db().add_raid_score(x.s.player.id, league_id_of(x.s.player), delta, damage, entered_raid,
                            won);
    x.s.player.raid_score += delta;

    if (Packet p = clan_rating_event(x.s.player.id, clan_id ? member_total : currency)) {
        if (clan_id) {
            for (int64_t pid : x.w.db().clan_member_ids(clan_id, int(x.cfg.clan_max_members)))
                push(x.w, pid, p);
        } else
            push(x.w, x.s.player.id, p);
    }
    if (Packet p = league_event(x))
        push(x.w, x.s.player.id, p);
}

void merge_counts(std::vector<NamedCount> &dst, const std::vector<NamedCount> &add) {
    for (const NamedCount &a : add) {
        bool found = false;
        for (NamedCount &d : dst) {
            if (d.name == a.name) {
                if (d.count > (int64_t(1) << 40)) {
                    found = true;
                    break;
                }
                d.count += a.count;
                found = true;
                break;
            }
        }
        if (!found && dst.size() < 256)
            dst.push_back(a);
    }
}

struct RewardPolicy {
    bool enforce_threshold = true;
};

RewardPolicy g_reward_policy;

bool build_reward(const RaidBattle &b, const RaidParticipant &p, RaidReward &out) {
    const GameData &gd = GameData::instance();
    const RaidDef *def = gd.raid(b.raid_id());
    if (!def || def->rewards.tiers.empty())
        return false;

    out = RaidReward{};

    if (g_reward_policy.enforce_threshold && b.boss.hp_max > 0 &&
        def->rewards.threshold_damage_fraction > 0) {

        const double scale = b.damage_scale > 0 ? b.damage_scale : 1.0;
        const double share = double(p.damage) * scale / double(b.boss.hp_max);
        if (share < def->rewards.threshold_damage_fraction) {
            out.present = false;
            return true;
        }
    }

    const RewardTier *tier = def->rewards.for_place(p.place);
    if (!tier || tier->container.empty()) {
        out.present = false;
        return true;
    }

    LootRoll roll;
    gd.roll(tier->container, roll);

    out.present = true;
    out.chest.name = tier->container;
    out.chest.items = roll.currency;
    out.chest.spheres = roll.consumables;
    out.gems = roll.gems;
    out.rating_base = def->rewards.base_rating;
    out.rating_bonus = int64_t(llround(double(def->rewards.pos_rating) * tier->pos_rating_scaler));
    out.damage = int64_t(llround(double(p.damage) * def->rewards.damage_rating_scaler));
    return true;
}

void grant_reward(Ctx &x, int64_t raid_id, const RaidReward &r, int64_t place) {
    if (!r.present)
        return;

    if (!x.w.db().claim_raid_reward(raid_id, x.s.player.id, x.wall_ms)) {
        LOG_DEBUG("raid %lld: reward for %lld already claimed", (long long)raid_id,
                  (long long)x.s.player.id);
        return;
    }

    merge_counts(x.s.player.items, r.chest.items);
    merge_counts(x.s.player.perks, r.chest.spheres);
    x.w.db().set_items(x.s.player.id, x.s.player.items);
    x.w.db().set_perks(x.s.player.id, x.s.player.perks);
    if (r.gems > 0) {
        x.s.player.gems += r.gems;
        x.w.db().set_currencies(x.s.player.id, x.s.player.gems, x.s.player.currency2);
    }

    credit_rating(x, r.total_rating(), 0, false, true);
    if (place == 1)
        x.w.db().add_first_place(x.s.player.id, league_id_of(x.s.player));
    push_profile(x);

    log::audit("raid_reward",
               "player=%lld battle=%lld place=%lld damage=%lld rating=%lld gems=%lld chest=%s",
               (long long)x.s.player.id, (long long)raid_id, (long long)place, (long long)r.damage,
               (long long)r.total_rating(), (long long)r.gems, r.chest.name.c_str());
    LOG_INFO("raid %lld: player %lld place %lld damage %lld chest %s rating +%lld",
             (long long)raid_id, (long long)x.s.player.id, (long long)place, (long long)r.damage,
             r.chest.name.c_str(), (long long)r.total_rating());
}

void persist(Worker &w, const RaidBattle &b, int64_t wall_ms) {
    w.db().save_raid(b, wall_ms);
}

void on_join_raid_matcher(Ctx &x) {

    const int64_t raid_id = int64_t(pb::get_varint(x.req.body, 1, 1));

    if (const RaidDef *def = GameData::instance().raid(raid_id)) {
        if (def->dan_restriction > 0 && x.s.player.rank < def->dan_restriction) {
            log::throttled(log::Level::Debug, "raid-dan", 5000,
                           "[%s] raid %lld needs rank %lld, player has %lld", x.s.remote().c_str(),
                           (long long)raid_id, (long long)def->dan_restriction,
                           (long long)x.s.player.rank);
            reply_ext(x, "join_raid_matcher");
            return;
        }
    }

    RaidEngine::MatchResult m =
        RaidEngine::instance().join_matcher(x.s.player.id, raid_id, x.now_ms, x.wall_ms);

    if (m.rejected) {
        log::throttled(log::Level::Debug, "raid-id", 5000, "[%s] raid matcher refused RaidID %lld",
                       x.s.remote().c_str(), (long long)raid_id);
        reply_ext(x, "join_raid_matcher");
        return;
    }

    if (m.already_in) {
        LOG_DEBUG("[%s] player %lld is already in raid %lld; re-sending it", x.s.remote().c_str(),
                  (long long)x.s.player.id, (long long)m.battle.id);
        reply_ext(x, "join_raid_matcher");
        push(x.w, x.s.player.id, raid_start_event(m.battle));
        return;
    }

    reply_ext(x, "join_raid_matcher");

    Packet mine = player_event(x.w, x.s.player.id);

    if (m.formed) {
        persist(x.w, m.battle, x.wall_ms);
        for (const RaidParticipant &q : m.battle.players) {
            if (q.pid == x.s.player.id)
                continue;
            if (Packet p = player_event(x.w, q.pid))
                push(x.w, x.s.player.id, p);
            if (mine)
                push(x.w, q.pid, mine);
        }
        push_party(x.w, m.battle, raid_start_event(m.battle));
        return;
    }

    for (int64_t pid : m.waiting) {
        if (Packet p = player_event(x.w, pid))
            push(x.w, x.s.player.id, p);
        if (mine)
            push(x.w, pid, mine);
    }
}

void on_get_raid_matcher(Ctx &x) {
    const auto state = RaidEngine::instance().matcher_status(x.s.player.id);
    std::string json = "{\"raid_id\":" + std::to_string(state.raid_id) +
                       ",\"capacity\":" + std::to_string(state.capacity) + ",\"players\":[";
    for (size_t i = 0; i < state.players.size(); ++i) {
        if (i)
            json += ',';
        json += std::to_string(state.players[i]);
    }
    json += "]}";
    Buffer body;
    resp::proto_string(body, json);
    reply_ext(x, "get_raid_matcher", body.view());
}

void on_leave_raid_matcher(Ctx &x) {

    RaidEngine::instance().leave_matcher(x.s.player.id);
    reply_ext(x, "leave_raid_matcher");
    announce_presence(x.w, x.s);
}

void on_raid_get_info(Ctx &x) {
    const int64_t want = int64_t(pb::get_varint(x.req.body, 1, 0));

    RaidBattle b;
    bool have = false;

    if (RaidEngine::instance().snapshot_of_player(x.s.player.id, b))
        have = (want == 0 || want == b.id);

    if (!have) {
        reply_ext(x, "raid_battle_get_info");
        return;
    }

    Buffer &pr = proto_buf();
    {
        pb::Sub s(pr, 1);
        proto::build_raid_info(pr, b);
    }
    {
        pb::Sub s(pr, 2);
        pb::v(pr, 1, uint64_t(next_attempt(x)));
    }
    reply_ext(x, "raid_battle_get_info", pr.view(), true);
}

bool charge_entry(Ctx &x, const RaidDef &def) {
    if (def.costs.empty())
        return true;

    for (const RaidCost &c : def.costs) {
        int64_t have = 0;
        for (const NamedCount &it : x.s.player.items)
            if (it.name == c.currency) {
                have = it.count;
                break;
            }
        if (have < c.value) {
            LOG_INFO("[%s] player %lld cannot enter raid %lld: needs %lld %s, has %lld",
                     x.s.remote().c_str(), (long long)x.s.player.id, (long long)def.id,
                     (long long)c.value, c.currency.c_str(), (long long)have);
            return false;
        }
    }
    for (const RaidCost &c : def.costs)
        for (NamedCount &it : x.s.player.items)
            if (it.name == c.currency) {
                it.count -= c.value;
                break;
            }

    x.w.db().set_items(x.s.player.id, x.s.player.items);
    push_profile(x);
    LOG_INFO("[%s] player %lld paid %s x%lld to enter raid %lld", x.s.remote().c_str(),
             (long long)x.s.player.id, def.costs[0].currency.c_str(), (long long)def.costs[0].value,
             (long long)def.id);
    return true;
}

void on_raid_player_start(Ctx &x) {

    RaidBattle current;
    if (RaidEngine::instance().snapshot_of_player(x.s.player.id, current)) {
        const RaidParticipant *me = current.find(x.s.player.id);
        if (me && !me->joined) {
            const RaidDef *def = GameData::instance().raid(current.raid_id());
            if (def && !charge_entry(x, *def)) {
                reply_ext(x, "raid_battle_player_start");
                return;
            }
        }
    }

    RaidBattle battle;
    RaidEngine::Status st = RaidEngine::instance().player_start(x.s.player.id, battle);
    if (st != RaidEngine::Status::Ok && st != RaidEngine::Status::AlreadyFinished) {
        log::throttled(log::Level::Debug, "raid-start", 5000, "[%s] raid_battle_player_start: %s",
                       x.s.remote().c_str(), RaidEngine::status_text(st));
        reply_ext(x, "raid_battle_player_start");
        return;
    }

    Buffer &pr = proto_buf();
    for (const NamedCount &it : x.s.player.items) {
        pb::Sub s(pr, 1);
        pb::str(pr, 1, it.name);
        pb::v(pr, 2, uint64_t(it.count));
    }
    reply_ext(x, "raid_battle_player_start", pr.view(), true);

    if (st == RaidEngine::Status::Ok) {
        const RaidParticipant *me = battle.find(x.s.player.id);
        if (me && me->rounds_used == 0)
            x.w.db().add_raid_score(x.s.player.id, league_id_of(x.s.player), 0, 0, true, false);
    }
    log::audit("raid_enter", "player=%lld session=%llu battle=%lld raid=%lld status=\"%s\"",
               (long long)x.s.player.id, (unsigned long long)x.s.gen, (long long)battle.id,
               (long long)battle.raid_id(), RaidEngine::status_text(st));
    LOG_DEBUG("raid %lld: player %lld entered (%zu in party)", (long long)battle.id,
              (long long)x.s.player.id, battle.players.size());

    announce_presence(x.w, x.s);
}

void on_raid_start_round(Ctx &x) {
    RaidBattle b;
    std::vector<std::string> potions;
    pb::scan(x.req.body, [&](const pb::Field &field) {
        if (field.number == 2 && field.wire == pb::Wire::Bytes && potions.size() < 17)
            potions.emplace_back(field.data.sv());
        return true;
    });
    const auto client_perks = x.s.player.perks;
    int failure_code = 10004;
    RaidEngine::Status st = RaidEngine::instance().start_round(
        x.s.player.id, x.now_ms, x.wall_ms, b, int64_t(pb::get_varint(x.req.body, 1, 0)),
        [&](int64_t price) {
            Player paid;
            failure_code = x.w.db().pay_raid_round(x.s.player.id, price, potions, paid);
            if (failure_code)
                return false;
            x.s.player = std::move(paid);
            for (const auto &name : potions)
                LOG_INFO("raid %lld player %lld consumed %s", (long long)b.id,
                         (long long)x.s.player.id, name.c_str());
            return true;
        });
    const RaidParticipant *audit_round = b.find(x.s.player.id);
    log::audit("round_start",
               "player=%lld session=%llu battle=%lld raid=%lld round=%lld final=%d "
               "start_utc_ms=%lld status=\"%s\"",
               (long long)x.s.player.id, (unsigned long long)x.s.gen, (long long)b.id,
               (long long)b.raid_id(), audit_round ? (long long)audit_round->rounds_used + 1 : 0,
               int(b.boss.hp_left <= 0), (long long)x.wall_ms, RaidEngine::status_text(st));
    if (st != RaidEngine::Status::Ok) {
        log::throttled(log::Level::Debug, "raid-round", 5000,
                       "[%s] raid_battle_start_round refused: %s", x.s.remote().c_str(),
                       RaidEngine::status_text(st));
        Buffer &refused = out_buf();
        resp::ext_error(refused, "raid_battle_start_round", x.req.req_id,
                        st == RaidEngine::Status::PaymentFailed ? failure_code
                        : st == RaidEngine::Status::BattleOver  ? 10002
                                                                : 10008);
        reply(x, refused);

        if (st == RaidEngine::Status::OnCooldown || st == RaidEngine::Status::PaymentFailed) {
            Buffer pr;
            {
                pb::Sub sub(pr, 1);
                proto::build_raid_info(pr, b);
            }
            {
                pb::Sub sub(pr, 2);
                pb::v(pr, 1, uint64_t(next_attempt(x)));
            }
            push(x.w, x.s.player.id, make_event("raid_battle_get_info", pr.view(), true));
        }
        return;
    }

    if (pb::get_varint(x.req.body, 1, 0) > 0) {
        Player balance_profile = x.s.player;

        if (!potions.empty())
            balance_profile.perks = client_perks;
        Buffer profile;
        proto::build_player_info(profile, balance_profile);

        Buffer ledger;
        resp::ext_event(ledger, "ledger_update_event", profile.view(), true);
        reply(x, ledger);
    }

    if (const RaidParticipant *me = b.find(x.s.player.id))
        push_party(x.w, b, raid_player_event(*me));

    Buffer &pr = proto_buf();
    {
        pb::Sub s(pr, 1);
        proto::build_raid_boss(pr, b.boss);
    }
    reply_ext(x, "raid_battle_start_round", pr.view());
    if (!potions.empty()) {
        Buffer profile;
        proto::build_player_info(profile, x.s.player);
        push(x.w, x.s.player.id, make_event("ledger_update_event", profile.view(), true));
    }
}

void on_raid_finish_round(Ctx &x) {

    RaidBattle current;
    const int64_t raid_id =
        RaidEngine::instance().snapshot_of_player(x.s.player.id, current) ? current.raid_id() : 0;

    RoundReport report;
    parse_round_report(x.req.body, report, x.cfg.raid_max_round_hits);

    ChargeResolution charges = resolve_charges(report.charge_claimed, report.charges,
                                               x.s.player.perks, GameData::instance());
    if (report.charge_claimed > 0 || !report.charges.empty()) {
        std::string names;
        for (const NamedCount &c : charges.spent) {
            if (!names.empty())
                names += ", ";
            names += c.name + " x" + std::to_string(c.count);
        }
        LOG_INFO("[%s] player %lld charges: claimed %lld damage from [%s]%s, "
                 "accepted %lld",
                 x.s.remote().c_str(), (long long)x.s.player.id, (long long)report.charge_claimed,
                 names.empty() ? "nothing it owns" : names.c_str(),
                 charges.unowned ? " (some not owned)" : "", (long long)charges.accepted);
    }
    if (!report.extra.empty())
        LOG_INFO("[%s] round report also carried: %s", x.s.remote().c_str(),
                 report.extras().c_str());

    const int64_t ceiling = std::min<int64_t>(
        (report.claimed < 0 ? 0 : report.claimed) + charges.accepted, x.cfg.raid_max_round_damage);

    RoundVerdict verdict;
    if (x.cfg.raid_verify_rounds) {
        verdict = verify_round(report, raid_id, GameData::instance(), x.cfg.raid_max_round_damage);

        verdict.charge_damage = charges.accepted;
        verdict.damage =
            std::min<int64_t>(verdict.damage + charges.accepted, x.cfg.raid_max_round_damage);
    } else {
        verdict.damage = ceiling;
        verdict.hits_total = int(report.hits.size());
        verdict.note = "verification off";
    }

    if (verdict.verified && verdict.damage != ceiling)
        LOG_WARN("[%s] player %lld, raid %lld: claimed %lld, report supports "
                 "%lld (%.1f over %d hits, %d rejected: %s; %d crits) — %s",
                 x.s.remote().c_str(), (long long)x.s.player.id, (long long)raid_id,
                 (long long)report.claimed, (long long)verdict.damage, verdict.recomputed,
                 verdict.hits_total, verdict.hits_dropped, verdict.reasons().c_str(), verdict.crits,
                 x.cfg.raid_verify_enforce ? "credited the report"
                                           : "credited the claim (raid_verify_enforce=false)");

    const int64_t claimed = x.cfg.raid_verify_enforce ? verdict.damage : ceiling;

    int64_t applied = 0;
    RaidBattle b;
    RaidEngine::Status st =
        RaidEngine::instance().finish_round(x.s.player.id, claimed, x.now_ms, applied, b);

    const RaidParticipant *audit_before = current.find(x.s.player.id);
    const uint64_t opened = audit_before ? audit_before->round_opened_ms : 0;
    const uint64_t elapsed = opened && x.now_ms >= opened ? x.now_ms - opened : 0;
    log::audit(
        "round_finish",
        "player=%lld session=%llu battle=%lld raid=%lld round=%lld duration_ms=%llu "
        "start_utc_ms=%lld end_utc_ms=%lld ordinary_claimed=%lld charge_claimed=%lld "
        "report_damage=%lld credited=%lld hits=%zu rejected_hits=%d truncated=%d status=\"%s\"",
        (long long)x.s.player.id, (unsigned long long)x.s.gen, (long long)current.id,
        (long long)raid_id, audit_before ? (long long)audit_before->rounds_used + 1 : 0,
        (unsigned long long)elapsed, audit_before ? (long long)audit_before->last_round_ms : 0,
        (long long)x.wall_ms, (long long)report.claimed, (long long)report.charge_claimed,
        (long long)verdict.damage, (long long)applied, report.hits.size(), verdict.hits_dropped,
        int(report.truncated), RaidEngine::status_text(st));
    for (const RoundHit &hit : report.hits)
        log::audit("round_hit",
                   "player=%lld battle=%lld round=%lld seq=%lld move=%lld damage=%.6g landed=%d "
                   "crit=%d finisher=%d",
                   (long long)x.s.player.id, (long long)current.id,
                   audit_before ? (long long)audit_before->rounds_used + 1 : 0, (long long)hit.seq,
                   (long long)hit.move_id, hit.damage, int(hit.landed), int(hit.crit),
                   int(hit.finisher));

    if (st != RaidEngine::Status::Ok) {
        log::throttled(log::Level::Warn, "raid-finish", 2000,
                       "[%s] raid_battle_finish_round refused for %lld "
                       "(claimed %lld): %s",
                       x.s.remote().c_str(), (long long)x.s.player.id, (long long)claimed,
                       RaidEngine::status_text(st));
        Buffer &pr = proto_buf();
        if (b.id) {
            pb::Sub s(pr, 1);
            proto::build_raid_info(pr, b);
        }
        pb::v(pr, 2, 0);
        {
            pb::Sub s(pr, 3);
            pb::v(pr, 1, uint64_t(next_attempt(x)));
        }
        reply_ext(x, "raid_battle_finish_round", pr.view(), true);
        return;
    }

    if (verdict.verified)
        LOG_INFO("raid %lld: player %lld round report — %d hits, %s", (long long)b.id,
                 (long long)x.s.player.id, verdict.hits_total, verdict.breakdown().c_str());

    if (applied != report.claimed)
        LOG_INFO("raid %lld (RaidID %lld): player %lld claimed %lld damage, "
                 "credited %lld",
                 (long long)b.id, (long long)b.raid_id(), (long long)x.s.player.id,
                 (long long)report.claimed, (long long)applied);

    persist(x.w, b, x.wall_ms);

    const RaidParticipant *me = b.find(x.s.player.id);
    if (me) {
        push_party(x.w, b, raid_player_event(*me));
        Buffer pr;
        {
            pb::Sub s(pr, 1);
            proto::build_raid_player(pr, *me);
        }
        {
            pb::Sub s(pr, 2);
            proto::build_raid_boss(pr, b.boss);
        }
        pb::v(pr, 3, uint64_t(applied));
        push_party(x.w, b, make_event("raid_battle_finish_round_event", pr.view()));
    }

    credit_rating(x, x.cfg.raid_round_rating, applied, false, false);
    DailyEvent daily;
    daily.battle = b.id;
    daily.raid = b.raid_id();
    daily.round = me ? me->rounds_used : 0;
    daily.damage = applied;
    daily.crits = verdict.crits;
    int64_t accepted_hits = 0;
    for (const auto &type : verdict.by_type) {
        daily.hits.emplace_back(type.first, type.second.first);
        accepted_hits += type.second.first;
    }

    int64_t used_charges = 0;
    for (const auto &charge : charges.spent)
        used_charges += charge.count;
    daily.hits.erase(
        std::remove_if(daily.hits.begin(), daily.hits.end(),
                       [](const auto &hit) { return hit.first == "RaidChargeDamage"; }),
        daily.hits.end());
    if (used_charges > 0)
        daily.hits.emplace_back("RaidChargeDamage", used_charges);
    daily.survived = pb::get_varint(x.req.body, 6, 0) == 1;
    daily.style = accepted_hits > 0
                      ? std::clamp<int64_t>(int64_t(pb::get_varint(x.req.body, 7, 0)), 0, 5)
                      : 0;
    pb::scan(x.req.body, [&](const pb::Field &field) {
        if (field.number == 5 && field.wire == pb::Wire::Bytes && daily.combos.size() < 64) {
            const int64_t length = int64_t(pb::get_varint(field.data, 1, 0));
            const int64_t count = int64_t(pb::get_varint(field.data, 2, 0));
            if (length > 0 && count > 0 && length <= accepted_hits)
                daily.combos.emplace_back(length, std::min(count, accepted_hits / length));
        }
        return true;
    });
    const auto old_rating = x.s.player.currency2;
    x.w.db().record_daily(x.s.player.id, daily, x.wall_ms);
    if (auto fresh = x.w.db().by_id(x.s.player.id))
        x.s.player = std::move(*fresh);
    if (x.s.player.currency2 != old_rating)
        push_season_state(x.w, x.s);
    spend_charges(x, charges.spent);

    Buffer &pr = proto_buf();
    {
        pb::Sub s(pr, 1);
        proto::build_raid_info(pr, b);
    }
    pb::v(pr, 2, 1);
    {
        pb::Sub s(pr, 3);
        pb::v(pr, 1, uint64_t(next_attempt(x)));
    }
    {
        pb::Sub s(pr, 4);
        proto::build_player_info(pr, x.s.player);
    }
    reply_ext(x, "raid_battle_finish_round", pr.view(), true);
}

void on_raid_player_finish(Ctx &x) {

    const bool won = pb::get_varint(x.req.body, 1, 0) == 1;

    RaidBattle before;
    RaidEngine::instance().snapshot_of_player(x.s.player.id, before);
    const RaidParticipant *prior = before.find(x.s.player.id);
    RaidReward reward;
    RaidBattle b;
    RaidEngine::Status st = RaidEngine::instance().player_finish(x.s.player.id, won, reward, b);
    const uint64_t duration = prior && prior->round_opened_ms && x.now_ms >= prior->round_opened_ms
                                  ? x.now_ms - prior->round_opened_ms
                                  : 0;
    log::audit("final_finish",
               "player=%lld session=%llu battle=%lld raid=%lld duration_ms=%llu start_utc_ms=%lld "
               "end_utc_ms=%lld won=%d damage_total=%lld status=\"%s\"",
               (long long)x.s.player.id, (unsigned long long)x.s.gen, (long long)before.id,
               (long long)before.raid_id(), (unsigned long long)duration,
               prior ? (long long)prior->last_round_ms : 0, (long long)x.wall_ms, int(won),
               prior ? (long long)prior->damage : 0, RaidEngine::status_text(st));

    if (st == RaidEngine::Status::NotInBattle || st == RaidEngine::Status::NotEntered) {
        log::throttled(log::Level::Debug, "raid-finish-no", 5000,
                       "[%s] raid_battle_player_finish refused: %s", x.s.remote().c_str(),
                       RaidEngine::status_text(st));
        reply_ext(x, "raid_battle_player_finish");
        return;
    }

    if (st == RaidEngine::Status::BossAlive) {
        LOG_WARN("[%s] player %lld claimed the final battle of raid %lld with "
                 "%lld/%lld boss HP left — refused",
                 x.s.remote().c_str(), (long long)x.s.player.id, (long long)b.id,
                 (long long)b.boss.hp_left, (long long)b.boss.hp_max);
        Buffer &pr = proto_buf();
        {
            pb::Sub sub(pr, 1);
            proto::build_raid_info(pr, b);
        }
        {
            pb::Sub sub(pr, 2);
            proto::build_player_info(pr, x.s.player);
        }
        reply_ext(x, "raid_battle_player_finish", pr.view(), true);
        return;
    }

    if (st == RaidEngine::Status::FinishFailed) {
        const RaidParticipant *p = b.find(x.s.player.id);
        LOG_INFO("[%s] player %lld lost the final battle in raid %lld "
                 "(attempt %lld) — no reward",
                 x.s.remote().c_str(), (long long)x.s.player.id, (long long)b.id,
                 p ? (long long)p->finish_tries : 0);
        persist(x.w, b, x.wall_ms);
        Buffer &pr = proto_buf();
        {
            pb::Sub sub(pr, 1);
            proto::build_raid_info(pr, b);
        }
        {
            pb::Sub sub(pr, 2);
            proto::build_player_info(pr, x.s.player);
        }
        reply_ext(x, "raid_battle_player_finish", pr.view(), true);
        return;
    }

    const RaidParticipant *me = b.find(x.s.player.id);
    const int64_t place = me ? me->place : 0;

    if (st == RaidEngine::Status::Ok) {
        persist(x.w, b, x.wall_ms);
        grant_reward(x, b.id, reward, place);
        DailyEvent daily;
        daily.battle = b.id;
        daily.raid = b.raid_id();
        daily.finished = true;
        daily.won = true;
        daily.place = place;
        const auto old_rating = x.s.player.currency2;
        x.w.db().record_daily(x.s.player_id, daily, x.wall_ms);
        if (auto fresh = x.w.db().by_id(x.s.player_id))
            x.s.player = std::move(*fresh);
        if (x.s.player.currency2 != old_rating)
            push_season_state(x.w, x.s);
        if (b.closed) {
            Buffer info;
            proto::build_raid_info(info, b);
            auto finish = make_event("raid_battle_finish_event", info.view(), true);
            for (const auto &member : b.players)
                if (!member.left)
                    push(x.w, member.pid, finish);
        }
    }

    if (me)
        push_party(x.w, b, raid_player_event(*me), x.s.player.id);

    Buffer &pr = proto_buf();
    {
        pb::Sub s(pr, 1);
        proto::build_raid_info(pr, b);
    }
    {
        pb::Sub s(pr, 2);
        proto::build_player_info(pr, x.s.player);
    }
    reply_ext(x, "raid_battle_player_finish", pr.view(), true);

    announce_presence(x.w, x.s);
}

void on_raid_leave(Ctx &x) {
    const int64_t want = int64_t(pb::get_varint(x.req.body, 1, 0));

    RaidBattle b;
    const bool had = RaidEngine::instance().snapshot_of_player(x.s.player.id, b);
    const RaidParticipant *prior = b.find(x.s.player.id);
    log::audit(
        "raid_leave",
        "player=%lld session=%llu battle=%lld raid=%lld requested_battle=%lld in_raid=%d "
        "damage_total=%lld round_open=%d duration_ms=%llu",
        (long long)x.s.player.id, (unsigned long long)x.s.gen, (long long)b.id,
        (long long)b.raid_id(), (long long)want, int(had), prior ? (long long)prior->damage : 0,
        int(prior && prior->round_opened_ms),
        (unsigned long long)(prior && prior->round_opened_ms && x.now_ms >= prior->round_opened_ms
                                 ? x.now_ms - prior->round_opened_ms
                                 : 0));
    RaidEngine::instance().leave(x.s.player.id, want);
    RaidEngine::instance().leave_matcher(x.s.player.id);

    reply_ext(x, "raid_battle_leave");

    if (had) {
        if (const RaidParticipant *me = b.find(x.s.player.id)) {
            RaidParticipant gone = *me;
            gone.state = kRaidIdle;
            push_party(x.w, b, raid_player_event(gone), x.s.player.id);
        }
    }
    announce_presence(x.w, x.s);
}

void on_get_short_players(Ctx &x) {
    std::vector<int64_t> ids;
    pb::get_repeated_varints(x.req.body, 1, ids, x.cfg.raid_max_short_players);

    std::vector<ShortPlayer> rows = x.w.db().short_players(ids);
    for (ShortPlayer &r : rows)
        r.presence = presence_of(x.w, r.id);

    Buffer &pr = proto_buf();
    proto::build_short_players(pr, rows);
    reply_ext(x, "get_short_players", pr.view(), true);
}

std::mutex g_clan_ops;

constexpr int64_t kRoleRankLeader = 40;
constexpr int64_t kRoleRankOfficer = 30;
constexpr int64_t kRoleRankAdept = 20;
constexpr int64_t kRoleRankNovice = 10;

int64_t clan_role_rank(int64_t role) {
    static const char *names[] = {"", "ClanLeader", "ClanOfficer", "ClanAdept", "ClanNovice"};
    if (role > 0 && role < 5)
        for (const auto &entry : GameData::instance().clan().roles)
            if (entry.first == names[role])
                return entry.second;
    switch (role) {
    case kClanLeader:
        return kRoleRankLeader;
    case kClanOfficer:
        return kRoleRankOfficer;
    case kClanAdept:
        return kRoleRankAdept;
    case kClanNovice:
        return kRoleRankNovice;
    default:
        return 0;
    }
}

bool clan_permitted(int64_t role, std::string_view permission, int64_t fallback) {
    int64_t need = fallback;
    for (const auto &entry : GameData::instance().clan().permissions)
        if (entry.first == permission) {
            need = entry.second;
            break;
        }
    return clan_role_rank(role) > 0 && clan_role_rank(role) >= need;
}

void reply_clan_error(Ctx &x, std::string_view cmd, int32_t code = 10108) {
    LOG_DEBUG("clan refused player=%lld cmd=%.*s error=%d", (long long)x.s.player_id,
              int(cmd.size()), cmd.data(), code);
    Buffer &b = out_buf();
    resp::ext_error(b, cmd, x.req.req_id, code);
    reply(x, b);
}

const ClanMember *clan_member(const Clan &c, int64_t pid) {
    for (const ClanMember &m : c.members)
        if (m.pid == pid)
            return &m;
    return nullptr;
}

struct ClanProfileInput {
    std::string description;
    std::string emblem = "0,0,0";
    int64_t join_type = 1;
    int64_t min_rank = 0;
};

std::string clan_clean_text(std::string_view raw, int64_t max_symbols) {
    if (!validate::utf8_ok(raw))
        return {};
    return validate::sanitize_text(raw, size_t(std::max<int64_t>(0, max_symbols)));
}

bool clan_emblem_ok(std::string_view e) {

    if (e.empty() || e.size() > 40)
        return false;
    const auto &limits = GameData::instance().clan();

    const int64_t sizes[] = {limits.back_count, limits.icon_count, limits.color_count};
    size_t pos = 0;
    for (int part = 0; part < 3; ++part) {
        size_t end = e.find(',', pos);
        if (part == 2) {
            if (end != e.npos)
                return false;
            end = e.size();
        } else if (end == e.npos)
            return false;
        if (end == pos || end - pos > 6)
            return false;
        int64_t value = 0;
        for (size_t i = pos; i < end; ++i) {
            if (e[i] < '0' || e[i] > '9')
                return false;
            value = value * 10 + (e[i] - '0');
        }
        if (value >= sizes[part])
            return false;
        pos = end + 1;
    }
    return true;
}

bool parse_clan_profile(ByteView body, ClanProfileInput &out) {
    const ClanLimits &lim = GameData::instance().clan();
    std::string_view desc = pb::get_string(body, 1);
    std::string_view emb = pb::get_string(body, 2);
    LOG_DEBUG("clan profile desc_bytes=%zu emblem=%.*s join=%lld requirement=%lld", desc.size(),
              int(emb.size()), emb.data(), (long long)pb::get_varint(body, 3, 0),
              (long long)pb::get_varint(body, 4, 0));
    if (!validate::utf8_ok(desc) || !validate::utf8_ok(emb))
        return false;
    if (emb.empty())
        emb = "0,0,0";
    if (!clan_emblem_ok(emb))
        return false;

    out.description = clan_clean_text(desc, lim.desc_max);
    if (clan_clean_text(desc, lim.desc_max + 1) != out.description)
        return false;
    out.emblem.assign(emb);
    out.join_type = int64_t(pb::get_varint(body, 3, 1));
    if (out.join_type != 1 && out.join_type != 2)
        return false;
    out.min_rank = int64_t(pb::get_varint(body, 4, 0));
    if (out.min_rank < 0 || out.min_rank > INT32_MAX)
        return false;
    return true;
}

struct ClanLobby {
    int64_t id = 0;
    int64_t clan_id = 0;
    int64_t creator = 0;
    int64_t raid_id = 0;
    std::vector<int64_t> players;
    bool started = false;
};

std::mutex g_lobby_mu;
std::unordered_map<int64_t, ClanLobby> g_lobbies;
int64_t g_lobby_next = 1;

ClanRaidInfo lobby_info(const ClanLobby &l) {
    ClanRaidInfo r;
    r.id = l.id;
    r.raid_id = l.raid_id;
    r.players = l.players;
    r.started = l.started;
    return r;
}

std::optional<ClanLobby> lobby_copy(int64_t id) {
    std::lock_guard<std::mutex> lk(g_lobby_mu);
    auto it = g_lobbies.find(id);
    if (it == g_lobbies.end())
        return std::nullopt;
    return it->second;
}

std::optional<ClanLobby> lobby_of_player(int64_t pid) {
    std::lock_guard<std::mutex> lk(g_lobby_mu);
    for (const auto &kv : g_lobbies)
        for (int64_t q : kv.second.players)
            if (q == pid)
                return kv.second;
    return std::nullopt;
}

std::vector<ClanRaidInfo> clan_raid_snapshot(int64_t clan_id) {
    std::vector<ClanRaidInfo> out;
    std::lock_guard<std::mutex> lk(g_lobby_mu);
    for (const auto &kv : g_lobbies)
        if (kv.second.clan_id == clan_id)
            out.push_back(lobby_info(kv.second));
    std::sort(out.begin(), out.end(),
              [](const ClanRaidInfo &a, const ClanRaidInfo &b) { return a.id < b.id; });
    return out;
}

int64_t lobby_create(int64_t clan_id, int64_t creator, int64_t raid_id) {
    std::lock_guard<std::mutex> lk(g_lobby_mu);
    ClanLobby l;
    l.id = g_lobby_next++;
    l.clan_id = clan_id;
    l.creator = creator;
    l.raid_id = raid_id;
    l.players.push_back(creator);
    g_lobbies[l.id] = l;
    return l.id;
}

bool lobby_add(int64_t id, int64_t pid, size_t max_players, ClanLobby &out) {
    std::lock_guard<std::mutex> lk(g_lobby_mu);
    auto it = g_lobbies.find(id);
    if (it == g_lobbies.end() || it->second.started)
        return false;
    ClanLobby &l = it->second;
    for (const auto &kv : g_lobbies)
        if (std::find(kv.second.players.begin(), kv.second.players.end(), pid) !=
            kv.second.players.end())
            return false;
    if (l.players.size() >= max_players)
        return false;
    for (int64_t q : l.players)
        if (q == pid)
            return false;
    l.players.push_back(pid);
    out = l;
    return true;
}

bool lobby_remove(int64_t id, int64_t pid, ClanLobby &out, bool &empty) {
    std::lock_guard<std::mutex> lk(g_lobby_mu);
    auto it = g_lobbies.find(id);
    if (it == g_lobbies.end())
        return false;
    ClanLobby &l = it->second;
    auto p = std::find(l.players.begin(), l.players.end(), pid);
    if (p == l.players.end())
        return false;
    l.players.erase(p);

    if (l.creator == pid)
        l.players.clear();
    out = l;
    empty = l.players.empty();
    if (empty)
        g_lobbies.erase(it);
    return true;
}

bool lobby_erase(int64_t id, ClanLobby &out) {
    std::lock_guard<std::mutex> lk(g_lobby_mu);
    auto it = g_lobbies.find(id);
    if (it == g_lobbies.end())
        return false;
    out = it->second;
    g_lobbies.erase(it);
    return true;
}

std::optional<Clan> load_clan(Worker &w, int64_t clan_id) {
    auto c = w.db().clan_by_id(clan_id, int(w.cfg().clan_max_members));
    if (c)
        c->raids = clan_raid_snapshot(clan_id);
    return c;
}

void reply_clan(Ctx &x, const char *cmd, const Clan &c) {
    Buffer &pr = proto_buf();
    proto::build_clan(pr, c);
    reply_ext(x, cmd, pr.view(), true);
}

void reply_clan_join(Ctx &x, const char *cmd, const Clan &c) {
    Buffer &pr = proto_buf();
    {
        pb::Sub s(pr, 1);
        proto::build_clan(pr, c);
    }
    {
        pb::Sub s(pr, 2);
        proto::build_player_info(pr, x.s.player);
    }
    reply_ext(x, cmd, pr.view(), true);
}

void reply_player_data(Ctx &x, const char *cmd) {
    Buffer &pr = proto_buf();
    proto::build_player_info(pr, x.s.player);
    reply_ext(x, cmd, pr.view(), true);
}

void reply_clan_raid(Ctx &x, const char *cmd, const ClanRaidInfo &r) {
    Buffer &pr = proto_buf();
    proto::build_clan_raid(pr, r);
    reply_ext(x, cmd, pr.view(), true);
}

Packet clan_member_event(std::string_view cmd, const ClanMember &m) {
    Buffer pr;
    proto::build_clan_member(pr, m);
    return make_event(cmd, pr.view());
}

Packet clan_kick_event(int64_t actor, int64_t target) {
    Buffer pr;
    pb::v(pr, 1, uint64_t(actor));
    pb::v(pr, 2, uint64_t(target));
    return make_event("clan_member_kick_event", pr.view());
}

Packet clan_role_event(int64_t actor, int64_t target, int64_t role) {
    Buffer pr;
    pb::v(pr, 1, uint64_t(actor));
    pb::v(pr, 2, uint64_t(target));
    pb::v(pr, 3, uint64_t(role));
    return make_event("clan_change_role_event", pr.view());
}

Packet clan_leader_event(int64_t new_leader, int64_t old_leader) {
    Buffer pr;
    pb::v(pr, 1, uint64_t(new_leader));
    pb::v(pr, 2, uint64_t(old_leader));
    return make_event("clan_change_leader_event", pr.view());
}

Packet clan_profile_event(const Clan &c) {
    Buffer pr;
    proto::build_clan_profile(pr, c);
    return make_event("clan_update_profile_event", pr.view());
}

Packet clan_raid_event(std::string_view cmd, const ClanRaidInfo &r) {
    Buffer pr;
    proto::build_clan_raid(pr, r);
    return make_event(cmd, pr.view());
}

Packet clan_raid_destroy_event(int64_t id) {
    Buffer pr;
    pb::v(pr, 1, uint64_t(id));
    return make_event("clan_raid_destroy_event", pr.view());
}

void push_clan_event(Worker &w, int64_t clan_id, const Packet &p, int64_t exclude = 0) {
    if (!p || clan_id <= 0)
        return;
    for (int64_t pid : w.db().clan_member_ids(clan_id, int(w.cfg().clan_max_members)))
        if (pid != exclude)
            w.hub().send_to_player(pid, p);
}

void remove_from_clan_lobby(Worker &w, int64_t player_id) {
    auto mine = lobby_of_player(player_id);
    if (!mine)
        return;
    ClanLobby updated;
    bool empty = false;
    if (!lobby_remove(mine->id, player_id, updated, empty))
        return;
    Packet ev = empty ? clan_raid_destroy_event(mine->id)
                      : clan_raid_event("clan_raid_update_event", lobby_info(updated));
    push_clan_event(w, mine->clan_id, ev);
    w.hub().send_to_player(player_id, ev);
}

bool in_active_raid(int64_t player_id) {
    RaidBattle b;
    if (!RaidEngine::instance().snapshot_of_player(player_id, b) || b.closed)
        return false;
    const auto *member = b.find(player_id);
    return member && !member->finished && !member->left;
}

void announce_raid_start(Worker &w, const RaidBattle &b) {
    Packet start = raid_start_event(b);
    for (const RaidParticipant &p : b.players) {
        w.hub().send_to_player(p.pid, start);
        ShortPlayer sp;
        if (!short_of(w, p.pid, sp))
            continue;
        Buffer pr;
        proto::build_short_player(pr, sp);
        Packet ev = make_event("refresh_player_event", pr.view());
        for (const RaidParticipant &q : b.players)
            if (q.pid != p.pid)
                w.hub().send_to_player(q.pid, ev);
    }
}

void on_clan_get_by_id(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t id = int64_t(pb::get_varint(x.req.body, 1, 0));
    if (id <= 0) {
        reply_clan_error(x, "clan_get_by_id");
        return;
    }

    auto clan = load_clan(x.w, id);
    if (!clan) {
        reply_clan_error(x, "clan_get_by_id");
        return;
    }
    reply_clan(x, "clan_get_by_id", *clan);
}

void on_clan_find(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);

    std::string query =
        clan_clean_text(pb::get_string(x.req.body, 1), GameData::instance().clan().name_max);
    const bool recommended = pb::get_varint(x.req.body, 2, 0) != 0;
    std::vector<int64_t> ids = x.w.db().clan_search(query, recommended, int(x.cfg.clan_find_limit));

    Buffer &pr = proto_buf();
    pb::tag(pr, 1, 2);
    Buffer inner;
    for (int64_t id : ids)
        pb::varint(inner, uint64_t(id));
    pb::varint(pr, inner.size());
    pr.raw(inner.view());
    reply_ext(x, "clan_find", pr.view(), true);
}

void on_clan_get_short_info(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    std::vector<int64_t> ids;
    pb::get_repeated_varints(x.req.body, 1, ids, x.cfg.clan_find_limit);
    std::vector<Clan> clans = x.w.db().clans_short(ids);

    Buffer &pr = proto_buf();
    proto::build_clan_short_list(pr, clans);
    reply_ext(x, "clan_get_short_info", pr.view(), true);
}

void on_clan_create(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const ClanLimits &lim = GameData::instance().clan();
    if (x.w.db().clan_of(x.s.player_id) != 0) {
        reply_clan_error(x, "clan_create");
        return;
    }

    std::string name = clan_clean_text(pb::get_string(x.req.body, 1), lim.name_max);
    ClanProfileInput profile;
    if (name.empty() || clan_clean_text(pb::get_string(x.req.body, 1), lim.name_max + 1) != name ||
        !parse_clan_profile(pb::get_bytes(x.req.body, 2), profile)) {
        reply_clan_error(x, "clan_create");
        return;
    }
    if (x.w.db().clan_name_taken(name)) {
        reply_clan_error(x, "clan_create", 10101);
        return;
    }
    const int64_t price = std::max<int64_t>(0, lim.create_price_gems);

    auto payer = x.w.db().by_id(x.s.player_id);
    if (!payer || payer->gems < price) {
        reply_clan_error(x, "clan_create", 10004);
        return;
    }
    int64_t now_gems = 0;
    const int64_t clan_id =
        x.w.db().clan_create(name, profile.emblem, profile.description, profile.join_type,
                             profile.min_rank, x.s.player_id, price, &now_gems);
    if (!clan_id) {
        reply_clan_error(x, "clan_create");
        return;
    }

    x.s.player.gems = now_gems;
    x.s.player.clan_id = clan_id;

    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_create");
        return;
    }
    reply_clan_join(x, "clan_create", *clan);
}

void on_clan_free_join(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t clan_id = int64_t(pb::get_varint(x.req.body, 1, 0));
    if (clan_id <= 0 || x.w.db().clan_of(x.s.player_id) != 0) {
        reply_clan_error(x, "clan_free_join");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_free_join", 10102);
        return;
    }
    if (clan->join_type != 1) {
        reply_clan_error(x, "clan_free_join", 10109);
        return;
    }
    const ClanLimits &lim = GameData::instance().clan();
    const size_t max_members = size_t(std::max<int64_t>(1, lim.max_size));
    if (clan->member_count >= int64_t(max_members)) {
        reply_clan_error(x, "clan_free_join", 10104);
        return;
    }

    if (x.w.db().best_raid_rating(x.s.player_id) < clan->min_rank) {
        reply_clan_error(x, "clan_free_join", 10105);
        return;
    }
    int64_t until = 0;
    if (x.w.db().clan_kick_until(clan_id, x.s.player_id, 1, until) && until > x.wall_ms) {
        reply_clan_error(x, "clan_free_join", 10111);
        return;
    }
    if (!x.w.db().clan_join(clan_id, x.s.player_id, kClanAdept, int64_t(max_members))) {
        reply_clan_error(x, "clan_free_join");
        return;
    }
    x.s.player.clan_id = clan_id;

    clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_free_join");
        return;
    }
    reply_clan_join(x, "clan_free_join", *clan);

    if (const ClanMember *me = clan_member(*clan, x.s.player_id))
        push_clan_event(x.w, clan_id, clan_member_event("clan_member_join_event", *me),
                        x.s.player_id);
}

void on_clan_leave(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (clan_id <= 0) {
        reply_clan_error(x, "clan_leave");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_leave");
        return;
    }
    const ClanMember *me = clan_member(*clan, x.s.player_id);
    if (!me || me->role == kClanLeader) {
        reply_clan_error(x, "clan_leave");
        return;
    }
    ClanMember left = *me;
    if (!x.w.db().clan_remove_member(clan_id, x.s.player_id)) {
        reply_clan_error(x, "clan_leave");
        return;
    }
    remove_from_clan_lobby(x.w, x.s.player_id);
    x.s.player.clan_id = 0;
    reply_player_data(x, "clan_leave");
    push_clan_event(x.w, clan_id, clan_member_event("clan_member_leave_event", left));
}

void on_clan_destroy(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (clan_id <= 0) {
        reply_clan_error(x, "clan_destroy");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_destroy");
        return;
    }
    const ClanMember *me = clan_member(*clan, x.s.player_id);
    if (!me || !clan_permitted(me->role, "DisbandClan", kRoleRankLeader)) {
        reply_clan_error(x, "clan_destroy");
        return;
    }
    if (clan->member_count != 1) {
        reply_clan_error(x, "clan_destroy", 10107);
        return;
    }
    if (!x.w.db().clan_delete(clan_id)) {
        reply_clan_error(x, "clan_destroy");
        return;
    }
    for (const auto &raid : clan->raids) {
        ClanLobby removed;
        lobby_erase(raid.id, removed);
    }
    x.s.player.clan_id = 0;
    reply_player_data(x, "clan_destroy");
}

void on_clan_update_profile(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (clan_id <= 0) {
        reply_clan_error(x, "clan_update_profile");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_update_profile");
        return;
    }
    const ClanMember *me = clan_member(*clan, x.s.player_id);
    if (!me || !clan_permitted(me->role, "EditDescription", kRoleRankLeader) ||
        !clan_permitted(me->role, "EditIcon", kRoleRankLeader) ||
        !clan_permitted(me->role, "EditRequirements", kRoleRankLeader) ||
        !clan_permitted(me->role, "EditJoinType", kRoleRankLeader)) {
        reply_clan_error(x, "clan_update_profile");
        return;
    }
    ClanProfileInput profile;
    if (!parse_clan_profile(x.req.body, profile)) {
        reply_clan_error(x, "clan_update_profile");
        return;
    }
    if (!x.w.db().clan_update_profile(clan_id, profile.description, profile.emblem,
                                      profile.join_type, profile.min_rank)) {
        reply_clan_error(x, "clan_update_profile");
        return;
    }
    auto updated = load_clan(x.w, clan_id);
    if (!updated) {
        reply_clan_error(x, "clan_update_profile");
        return;
    }
    reply_clan(x, "clan_update_profile", *updated);
    push_clan_event(x.w, clan_id, clan_profile_event(*updated), x.s.player_id);
}

void on_clan_change_role(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t target = int64_t(pb::get_varint(x.req.body, 1, 0));
    const int64_t role = int64_t(pb::get_varint(x.req.body, 2, 0));
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (clan_id <= 0 || target <= 0 || target == x.s.player_id) {
        reply_clan_error(x, "clan_change_role");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_change_role");
        return;
    }
    const ClanMember *actor = clan_member(*clan, x.s.player_id);
    const ClanMember *dest = clan_member(*clan, target);
    if (!actor || !dest || dest->role == kClanLeader || role < kClanOfficer || role > kClanNovice ||
        !clan_permitted(actor->role, "ChangeRole", kRoleRankOfficer) ||
        clan_role_rank(dest->role) >= clan_role_rank(actor->role) ||
        clan_role_rank(role) >= clan_role_rank(actor->role)) {
        reply_clan_error(x, "clan_change_role");
        return;
    }
    if (!x.w.db().clan_set_role(clan_id, target, role)) {
        reply_clan_error(x, "clan_change_role");
        return;
    }
    reply_ext(x, "clan_change_role");
    push_clan_event(x.w, clan_id, clan_role_event(x.s.player_id, target, role));
}

void on_clan_change_leader(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t target = int64_t(pb::get_varint(x.req.body, 1, 0));
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (clan_id <= 0 || target <= 0 || target == x.s.player_id) {
        reply_clan_error(x, "clan_change_leader");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_change_leader");
        return;
    }
    const ClanMember *actor = clan_member(*clan, x.s.player_id);
    const ClanMember *dest = clan_member(*clan, target);
    if (!actor || !dest || actor->role != kClanLeader ||
        !clan_permitted(actor->role, "SetLeader", kRoleRankLeader)) {
        reply_clan_error(x, "clan_change_leader");
        return;
    }
    if (!x.w.db().clan_set_leader(clan_id, target, x.s.player_id)) {
        reply_clan_error(x, "clan_change_leader");
        return;
    }
    reply_ext(x, "clan_change_leader");
    push_clan_event(x.w, clan_id, clan_leader_event(target, x.s.player_id));
}

void on_clan_kick(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t target = int64_t(pb::get_varint(x.req.body, 1, 0));
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (clan_id <= 0 || target <= 0 || target == x.s.player_id) {
        reply_clan_error(x, "clan_kick");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_kick");
        return;
    }
    const ClanMember *actor = clan_member(*clan, x.s.player_id);
    const ClanMember *dest = clan_member(*clan, target);
    if (!actor || !dest || dest->role == kClanLeader ||
        !clan_permitted(actor->role, "KickPlayers", kRoleRankOfficer) ||
        clan_role_rank(dest->role) >= clan_role_rank(actor->role)) {
        reply_clan_error(x, "clan_kick");
        return;
    }
    if (!x.w.db().clan_remove_member(clan_id, target)) {
        reply_clan_error(x, "clan_kick");
        return;
    }
    remove_from_clan_lobby(x.w, target);
    const int64_t cooldown = GameData::instance().clan().kick_cooldown_ms;
    if (cooldown > 0)
        x.w.db().clan_kick_set(clan_id, target, 1, x.wall_ms + cooldown);

    reply_ext(x, "clan_kick");
    Packet ev = clan_kick_event(x.s.player_id, target);
    push_clan_event(x.w, clan_id, ev);
    x.w.hub().send_to_player(target, ev);
}

void on_clan_raid_create(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t raid_id = int64_t(pb::get_varint(x.req.body, 1, 0));
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (clan_id <= 0 || raid_id <= 0) {
        reply_clan_error(x, "clan_raid_create");
        return;
    }
    const RaidDef *def = GameData::instance().raid(raid_id);
    if (!def) {
        reply_clan_error(x, "clan_raid_create");
        return;
    }
    auto clan = load_clan(x.w, clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_raid_create");
        return;
    }
    const ClanMember *me = clan_member(*clan, x.s.player_id);
    if (!me || !clan_permitted(me->role, "CreateClanRaid", kRoleRankNovice)) {
        reply_clan_error(x, "clan_raid_create");
        return;
    }
    if (in_active_raid(x.s.player_id) || x.s.player.rank < def->dan_restriction ||
        lobby_of_player(x.s.player_id)) {
        reply_clan_error(x, "clan_raid_create");
        return;
    }
    for (const auto &raid : clan->raids)
        if (raid.raid_id == raid_id) {
            reply_clan_error(x, "clan_raid_create", 10114);
            return;
        }
    const int64_t lid = lobby_create(clan_id, x.s.player_id, raid_id);
    auto l = lobby_copy(lid);
    if (!l) {
        reply_clan_error(x, "clan_raid_create");
        return;
    }
    ClanRaidInfo info = lobby_info(*l);
    reply_clan_raid(x, "clan_raid_create", info);
    push_clan_event(x.w, clan_id, clan_raid_event("clan_raid_create_event", info), x.s.player_id);
}

void on_clan_raid_join(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t lid = int64_t(pb::get_varint(x.req.body, 1, 0));
    const int64_t clan_id = x.w.db().clan_of(x.s.player_id);
    if (lid <= 0 || clan_id <= 0) {
        reply_clan_error(x, "clan_raid_join");
        return;
    }
    auto l = lobby_copy(lid);
    if (!l || l->clan_id != clan_id || in_active_raid(x.s.player_id)) {
        reply_clan_error(x, "clan_raid_join");
        return;
    }
    const RaidDef *def = GameData::instance().raid(l->raid_id);
    if (!def || x.s.player.rank < def->dan_restriction) {
        reply_clan_error(x, "clan_raid_join", 10007);
        return;
    }
    const size_t cap = size_t(std::max<int64_t>(1, def->party_size));
    int64_t until = 0;
    if (x.w.db().clan_kick_until(clan_id, x.s.player_id, 2, until) && until > x.wall_ms) {
        reply_clan_error(x, "clan_raid_join", 10110);
        return;
    }
    ClanLobby updated;
    if (!lobby_add(lid, x.s.player_id, cap, updated)) {
        reply_clan_error(x, "clan_raid_join");
        return;
    }
    ClanRaidInfo info = lobby_info(updated);
    reply_clan_raid(x, "clan_raid_join", info);
    push_clan_event(x.w, clan_id, clan_raid_event("clan_raid_update_event", info), x.s.player_id);
}

void on_clan_raid_leave(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    auto mine = lobby_of_player(x.s.player_id);
    if (!mine) {
        reply_clan_error(x, "clan_raid_leave");
        return;
    }
    ClanLobby updated;
    bool empty = false;
    if (!lobby_remove(mine->id, x.s.player_id, updated, empty)) {
        reply_clan_error(x, "clan_raid_leave");
        return;
    }
    ClanRaidInfo info = lobby_info(updated);
    reply_clan_raid(x, "clan_raid_leave", info);
    if (empty) {
        push_clan_event(x.w, mine->clan_id, clan_raid_destroy_event(mine->id));
    } else {
        push_clan_event(x.w, mine->clan_id, clan_raid_event("clan_raid_update_event", info),
                        x.s.player_id);
    }
}

void on_clan_raid_kick(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    const int64_t target = int64_t(pb::get_varint(x.req.body, 1, 0));
    auto mine = lobby_of_player(x.s.player_id);
    if (!mine || target <= 0 || target == x.s.player_id) {
        reply_clan_error(x, "clan_raid_kick");
        return;
    }
    auto clan = load_clan(x.w, mine->clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_raid_kick");
        return;
    }
    const ClanMember *actor = clan_member(*clan, x.s.player_id);
    if (!actor ||
        (mine->creator != x.s.player_id && clan_role_rank(actor->role) < kRoleRankOfficer)) {
        reply_clan_error(x, "clan_raid_kick");
        return;
    }
    if (std::find(mine->players.begin(), mine->players.end(), target) == mine->players.end()) {
        reply_clan_error(x, "clan_raid_kick");
        return;
    }
    ClanLobby updated;
    bool empty = false;
    if (!lobby_remove(mine->id, target, updated, empty)) {
        reply_clan_error(x, "clan_raid_kick");
        return;
    }
    const int64_t cooldown = GameData::instance().clan().raid_kick_cooldown_ms;
    if (cooldown > 0)
        x.w.db().clan_kick_set(mine->clan_id, target, 2, x.wall_ms + cooldown);

    ClanRaidInfo info = lobby_info(updated);
    reply_clan_raid(x, "clan_raid_kick", info);
    Packet ev = clan_raid_event("clan_raid_update_event", info);
    push_clan_event(x.w, mine->clan_id, ev);
    x.w.hub().send_to_player(target, ev);
    if (empty)
        push_clan_event(x.w, mine->clan_id, clan_raid_destroy_event(mine->id));
}

void on_clan_raid_start(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    auto mine = lobby_of_player(x.s.player_id);
    if (!mine) {
        reply_clan_error(x, "clan_raid_start");
        return;
    }
    auto clan = load_clan(x.w, mine->clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_raid_start");
        return;
    }
    const ClanMember *actor = clan_member(*clan, x.s.player_id);
    if (!actor ||
        (mine->creator != x.s.player_id && clan_role_rank(actor->role) < kRoleRankOfficer)) {
        reply_clan_error(x, "clan_raid_start");
        return;
    }
    const RaidDef *def = GameData::instance().raid(mine->raid_id);
    const size_t need = size_t(std::max<int64_t>(1, def ? def->party_size : 6));
    if (mine->players.size() != need) {
        reply_clan_error(x, "clan_raid_start");
        return;
    }
    for (int64_t pid : mine->players) {
        auto player = x.w.db().by_id(pid);
        if (!player || x.w.db().clan_of(pid) != mine->clan_id || !x.w.hub().is_online(pid) ||
            player->rank < (def ? def->dan_restriction : 0)) {
            reply_clan_error(x, "clan_raid_start", 10113);
            return;
        }
    }
    RaidBattle battle;
    if (!RaidEngine::instance().start_clan_raid(mine->raid_id, mine->players, x.now_ms, x.wall_ms,
                                                battle)) {
        reply_clan_error(x, "clan_raid_start");
        return;
    }
    ClanLobby started;
    lobby_erase(mine->id, started);
    started.started = true;
    reply_ext(x, "clan_raid_start");
    ClanRaidInfo info = lobby_info(started);
    push_clan_event(x.w, mine->clan_id, clan_raid_event("clan_raid_update_event", info));

    push_clan_event(x.w, mine->clan_id, clan_raid_destroy_event(started.id));

    x.w.db().save_raid(battle, x.wall_ms);
    announce_raid_start(x.w, battle);
}

void on_clan_raid_destroy(Ctx &x) {
    std::lock_guard<std::mutex> operation(g_clan_ops);
    auto mine = lobby_of_player(x.s.player_id);
    if (!mine) {
        reply_clan_error(x, "clan_raid_destroy");
        return;
    }
    auto clan = load_clan(x.w, mine->clan_id);
    if (!clan) {
        reply_clan_error(x, "clan_raid_destroy");
        return;
    }
    const ClanMember *actor = clan_member(*clan, x.s.player_id);
    if (!actor ||
        (mine->creator != x.s.player_id && clan_role_rank(actor->role) < kRoleRankOfficer)) {
        reply_clan_error(x, "clan_raid_destroy");
        return;
    }
    ClanLobby removed;
    if (!lobby_erase(mine->id, removed)) {
        reply_clan_error(x, "clan_raid_destroy");
        return;
    }
    Buffer pr;
    pb::v(pr, 1, uint64_t(removed.id));
    reply_ext(x, "clan_raid_destroy", pr.view());
    push_clan_event(x.w, mine->clan_id, clan_raid_destroy_event(removed.id));
}

const HandlerDef kRaid[] = {
    {1, 13, "get_league", Need::LoggedIn, Cost::Lookup, on_get_league},

    {1, 13, "join_raid_matcher", Need::Named, Cost::Lookup, on_join_raid_matcher},
    {1, 13, "get_raid_matcher", Need::Named, Cost::Cheap, on_get_raid_matcher},
    {1, 13, "leave_raid_matcher", Need::Named, Cost::Cheap, on_leave_raid_matcher},
    {1, 13, "raid_battle_get_info", Need::Named, Cost::Lookup, on_raid_get_info},
    {1, 13, "raid_battle_player_start", Need::Named, Cost::Lookup, on_raid_player_start},
    {1, 13, "raid_battle_start_round", Need::Named, Cost::Lookup, on_raid_start_round},
    {1, 13, "raid_battle_finish_round", Need::Named, Cost::Heavy, on_raid_finish_round},
    {1, 13, "raid_battle_player_finish", Need::Named, Cost::Heavy, on_raid_player_finish},
    {1, 13, "raid_battle_leave", Need::Named, Cost::Lookup, on_raid_leave},
    {1, 13, "get_short_players", Need::LoggedIn, Cost::Lookup, on_get_short_players},
};

const HandlerDef kClan[] = {
    {1, 13, "clan_get_by_id", Need::LoggedIn, Cost::Lookup, on_clan_get_by_id},
    {1, 13, "clan_find", Need::LoggedIn, Cost::Lookup, on_clan_find},
    {1, 13, "clan_get_short_info", Need::LoggedIn, Cost::Lookup, on_clan_get_short_info},
    {1, 13, "clan_create", Need::Named, Cost::Heavy, on_clan_create},
    {1, 13, "clan_free_join", Need::Named, Cost::Lookup, on_clan_free_join},
    {1, 13, "clan_leave", Need::Named, Cost::Heavy, on_clan_leave},
    {1, 13, "clan_destroy", Need::Named, Cost::Heavy, on_clan_destroy},
    {1, 13, "clan_update_profile", Need::Named, Cost::Heavy, on_clan_update_profile},
    {1, 13, "clan_change_role", Need::Named, Cost::Heavy, on_clan_change_role},
    {1, 13, "clan_change_leader", Need::Named, Cost::Heavy, on_clan_change_leader},
    {1, 13, "clan_kick", Need::Named, Cost::Heavy, on_clan_kick},
    {1, 13, "clan_raid_create", Need::Named, Cost::Lookup, on_clan_raid_create},
    {1, 13, "clan_raid_join", Need::Named, Cost::Lookup, on_clan_raid_join},
    {1, 13, "clan_raid_leave", Need::Named, Cost::Cheap, on_clan_raid_leave},
    {1, 13, "clan_raid_kick", Need::Named, Cost::Lookup, on_clan_raid_kick},
    {1, 13, "clan_raid_start", Need::Named, Cost::Heavy, on_clan_raid_start},
    {1, 13, "clan_raid_destroy", Need::Named, Cost::Lookup, on_clan_raid_destroy},
};

}

void register_raid_handlers(const Config &cfg) {

    GameData::Report gd = GameData::instance().load(cfg.game_data_dir, cfg.config_path);
    if (gd.ok) {
        LOG_INFO("game data: %s (from %s)", gd.summary().c_str(), gd.resolved.c_str());
    } else {
        LOG_ERROR("GAME DATA NOT FOUND — set game_data_dir to the directory "
                  "holding config.xml. Looked for '%s' in:",
                  cfg.game_data_dir.c_str());
        for (const std::string &root : gd.searched)
            LOG_ERROR("    %s", root.c_str());
        LOG_ERROR("Without it every raid uses fallback numbers instead of its "
                  "real party size, duration and boss.");
    }
    for (const std::string &problem : gd.problems)
        LOG_WARN("  %s", problem.c_str());

    for (const RaidDef *d : GameData::instance().raids()) {
        RaidDef *m = GameData::instance().raid_mut(d->id);
        if (!m)
            continue;
        if (m->boss_hp <= 0)
            m->boss_hp = cfg.raid_default_boss_hp;
        const int64_t real_hp = m->boss_hp;
        auto ov = cfg.raid_overrides.find(d->id);
        if (ov == cfg.raid_overrides.end())
            continue;
        const Config::RaidOverride &o = ov->second;

        if (o.hp >= 0) {
            if (real_hp > 0 && o.hp < real_hp && !cfg.raid_allow_hp_override) {
                LOG_WARN("raid %lld: ignoring raid.%lld.hp = %lld — it is below "
                         "the boss's real ShieldTotal %lld, and the final "
                         "battle's boss takes its health from it. Serving %lld. "
                         "Use raid.%lld.damage_scale to shorten the raid, or "
                         "raid_allow_hp_override = true if you really mean it.",
                         (long long)m->id, (long long)m->id, (long long)o.hp, (long long)real_hp,
                         (long long)real_hp, (long long)m->id);
            } else {
                m->boss_hp = o.hp;
            }
        }
        if (o.party_size > 0)
            m->party_size = o.party_size;
        if (o.duration_ms > 0)
            m->duration_ms = o.duration_ms;
        if (o.cooldown_ms >= 0)
            m->round_cooldown_ms = o.cooldown_ms;
        if (o.dan >= 0)
            m->dan_restriction = o.dan;
        if (o.damage_scale > 0)
            m->damage_scale = o.damage_scale;
        LOG_INFO("raid %lld (%s%s) overridden: party=%lld hp=%lld duration=%llds"
                 " damage_scale=%.3g",
                 (long long)m->id, m->alias.c_str(), m->hard ? ", hard" : "",
                 (long long)m->party_size, (long long)m->boss_hp,
                 (long long)(m->duration_ms / 1000), m->damage_scale);

        if (real_hp > 0 && m->boss_hp > 0 && m->boss_hp < real_hp)
            LOG_WARN("raid %lld: serving hp %lld, below the boss's real "
                     "ShieldTotal %lld. The round's shield is built client-side "
                     "(ShieldTotal/ShieldStack = %lld/%lld) and will look "
                     "normal, but the final battle's boss gets its health from "
                     "the number sent here — so it will be over in one hit. "
                     "Use raid.%lld.damage_scale to shorten the raid and leave "
                     "the boss its real size.",
                     (long long)m->id, (long long)m->boss_hp, (long long)real_hp,
                     (long long)real_hp, (long long)(m->shield_stack > 0 ? m->shield_stack : 1),
                     (long long)m->id);
    }

    RaidRules r = default_raid_rules();
    r.bots_enabled = cfg.raid_bots_enabled;
    r.bot_fill_wait_ms = cfg.raid_bot_fill_wait_ms;
    r.bot_min_damage = cfg.raid_bot_min_damage;
    r.bot_max_damage = cfg.raid_bot_max_damage;
    r.match_wait_ms = cfg.raid_match_wait_ms;
    r.max_rounds = cfg.raid_max_rounds;
    r.min_round_ms = cfg.raid_min_round_ms;
    r.round_grace_ms = cfg.raid_round_grace_ms;
    r.max_round_dmg = cfg.raid_max_round_damage;
    r.round_rating = cfg.raid_round_rating;
    r.keep_finished_ms = int64_t(cfg.raid_keep_finished_s) * 1000;
    r.enforce_round_cooldown = cfg.raid_enforce_round_cooldown;
    r.require_boss_dead = cfg.raid_require_boss_dead;
    r.enforce_damage_threshold = cfg.raid_enforce_damage_threshold;
    r.enforce_round_expiry = cfg.raid_enforce_round_expiry;
    r.fallback_party_size = cfg.raid_fallback_party_size;
    r.fallback_duration_ms = cfg.raid_fallback_duration_ms;
    r.fallback_cooldown_ms = cfg.raid_fallback_cooldown_ms;
    r.default_boss_hp = cfg.raid_default_boss_hp;

    struct {
        const std::string &spec;
        RaidRewardTier *tier;
        const char *what;
    } tiers[] = {
        {cfg.raid_reward_place1, &r.place1, "raid_reward_place1"},
        {cfg.raid_reward_place2, &r.place2, "raid_reward_place2"},
        {cfg.raid_reward_place3, &r.place3, "raid_reward_place3"},
        {cfg.raid_reward_other, &r.other, "raid_reward_other"},
    };

    for (const auto &t : tiers) {
        if (t.spec.empty())
            continue;
        std::string err;
        if (!parse_reward_tier(t.spec, *t.tier, err))
            LOG_WARN("%s ignored: %s", t.what, err.c_str());
    }
    RaidEngine::instance().configure(r);
    g_reward_policy.enforce_threshold = cfg.raid_enforce_damage_threshold;
    RaidEngine::instance().set_reward_builder(&build_reward);

    Registry &reg = Registry::instance();
    if (cfg.enable_raids)
        reg.add_all(kRaid, sizeof kRaid / sizeof kRaid[0]);
    if (cfg.enable_clans)
        reg.add_all(kClan, sizeof kClan / sizeof kClan[0]);
}

bool raid_catalogue_loaded() {
    return GameData::instance().have_raids();
}

void on_player_online(Worker &w, Session &s) {
    if (!w.cfg().enable_clans && !w.cfg().enable_raids)
        return;
    s.player.clan_id = w.db().clan_of(s.player.id);

    RaidBattle b;
    if (w.cfg().enable_raids && RaidEngine::instance().snapshot_of_player(s.player_id, b) &&
        !b.closed) {
        const RaidParticipant *me = b.find(s.player_id);
        if (me && !me->finished && !me->left) {
            LOG_INFO("[%s] player %lld reconnected into raid %lld (RaidID %lld)",
                     s.remote().c_str(), (long long)s.player_id, (long long)b.id,
                     (long long)b.raid_id());
            w.hub().send_to_player(s.player_id, raid_start_event(b));

            for (const RaidParticipant &q : b.players) {
                if (q.pid == s.player_id)
                    continue;
                if (Packet p = player_event(w, q.pid))
                    w.hub().send_to_player(s.player_id, p);
            }
        }
    }

    push_season_state(w, s);

    Packet p = player_event(w, s.player_id);
    if (!p)
        return;
    for (int64_t pid : presence_audience(w, s))
        push(w, pid, p);
}

void sync_season_clock(Worker &w) {
    std::lock_guard<std::recursive_mutex> operation(g_season_ops);
    const int64_t now = w.wall_ms();
    auto current = w.db().current_season();
    if (current && current->ends_ms > now)
        return;

    do {
        const int64_t boundary = current ? current->ends_ms : now;
        const int64_t id = current ? current->id + 1 : int64_t(w.cfg().season_id);
        const int64_t end = league_ends_ms(w.cfg(), boundary);
        if (!w.db().advance_season(int64_t(w.cfg().season_id), boundary, end,
                                   league_version(w.cfg(), id))) {
            LOG_ERROR("could not advance league season");
            return;
        }
        current = w.db().current_season();
    } while (current && current->ends_ms <= now);
}

void push_season_notices(Worker &w, Session &s) {
    if (!s.logged_in() || !s.season_state_pushed || !s.named())
        return;
    std::lock_guard<std::recursive_mutex> operation(g_season_ops);
    const auto notices = w.db().pending_season_notices(s.player_id);
    if (notices.empty())
        return;
    if (auto p = w.db().by_id(s.player_id))
        s.player = std::move(*p);
    for (const auto &n : notices) {
        Buffer finished, started;
        if (n.league) {
            Player profile = s.player;
            profile.ts2 = n.league;
            {
                pb::Sub p(finished, 1);
                proto::build_player_info(finished, profile);
            }
            pb::bin(finished, 2, ByteView(n.reward));
            if (auto old = w.db().season_by_version(n.league)) {
                LeagueBoard board;
                board.league_id = old->id;
                board.ends_ms = old->ends_ms;
                board.content_version = n.league;
                board.entries = w.db().league_board(n.league, 150);
                pb::Sub table(finished, 3);
                proto::build_league(finished, board);
            }
        }
        {
            pb::Sub p(started, 1);
            proto::build_player_info(started, s.player);
        }
        started.raw(ByteView(n.restored));
        if (!w.db().mark_season_notice(s.player_id, n.season, w.wall_ms()))
            continue;
        if (n.league)
            w.hub().send_to_player(s.player_id,
                                   make_event("season_finished_event", finished.view(), true));
        w.hub().send_to_player(s.player_id,
                               make_event("season_started_event", started.view(), true));
        LOG_INFO("season %lld notice player=%lld league=%lld", (long long)n.season,
                 (long long)s.player_id, (long long)n.league);
    }
}

void sync_session_season(Worker &w, Session &s) {
    if (!s.logged_in() || s.player_id <= 0)
        return;
    auto current = w.db().current_season();
    if (!current || (s.player.season_id == current->id && s.player.ts1 > 0))
        return;
    w.db().ensure_season_row(s.player_id, current->id, w.wall_ms());
    if (auto p = w.db().by_id(s.player_id))
        s.player = std::move(*p);
    if (w.cfg().push_profile_updates) {
        Buffer profile;
        proto::build_player_info(profile, s.player);
        w.hub().send_to_player(s.player_id,
                               make_event("ledger_update_event", profile.view(), true));
    }
    push_season_state(w, s);
}

void push_season_state(Worker &w, Session &s) {
    if (s.player_id <= 0)
        return;
    const int64_t league = s.player.season_id;
    if (league <= 0)
        return;

    if (w.cfg().league_seed_on_login)
        w.db().ensure_season_row(s.player_id, league, w.wall_ms());

    LeagueBoard board;
    board.league_id = league;
    const auto current = w.db().current_season();
    if (!current)
        return;
    board.ends_ms = current->ends_ms;
    board.content_version = s.player.ts1;
    board.entries = w.db().league_board(s.player.ts1, 150);

    Buffer pr;
    proto::build_league(pr, board);
    w.hub().send_to_player(s.player_id, make_event("refresh_league_event", pr.view(), true));

    const Db::RaidStats rs = w.db().raid_stats(s.player_id, league);
    int64_t member_rating = rs.score ? rs.score : s.player.currency2;
    if (auto clan = w.db().clan_by_id(w.db().clan_of(s.player_id), int(w.cfg().clan_max_members)))
        for (const ClanMember &m : clan->members)
            if (m.pid == s.player_id) {
                member_rating = m.contribution;
                break;
            }
    if (Packet r = clan_rating_event(s.player_id, member_rating))
        w.hub().send_to_player(s.player_id, r);
}

void raid_tick(Worker &w, uint64_t mono_ms, int64_t wall_ms) {
    if (!w.cfg().enable_raids)
        return;

    sync_season_clock(w);

    static uint64_t next_snapshot = 0;
    if (!next_snapshot || mono_ms >= next_snapshot) {
        auto current = w.db().current_season();
        if (next_snapshot && current)
            w.db().snapshot_league_places(current->id, int(w.cfg().league_size));
        const int64_t interval = GameData::instance().league().update_interval_ms;
        next_snapshot = mono_ms + uint64_t(interval > 0 ? interval : 600000);
    }

    RaidEngine::Tick t = RaidEngine::instance().tick(mono_ms, wall_ms);
    if (t.formed.empty() && t.expired.empty() && t.bots.empty())
        return;

    for (const RaidBattle &b : t.formed) {
        w.db().save_raid(b, wall_ms);
        announce_raid_start(w, b);
    }

    for (const auto &update : t.bots) {
        const RaidBattle &b = update.battle;
        persist(w, b, wall_ms);
        for (const auto &p : b.players)
            if (is_raid_bot(p.pid))
                push_party(w, b, raid_player_event(p));
        for (const auto &damage : update.damage) {
            const auto *p = b.find(damage.first);
            if (!p)
                continue;
            Buffer pr;
            {
                pb::Sub sub(pr, 1);
                proto::build_raid_player(pr, *p);
            }
            {
                pb::Sub sub(pr, 2);
                proto::build_raid_boss(pr, b.boss);
            }
            pb::v(pr, 3, uint64_t(damage.second));
            push_party(w, b, make_event("raid_battle_finish_round_event", pr.view()));
            log::audit("raid_bot_round", "battle=%lld bot=%lld damage=%lld hp_left=%lld",
                       (long long)b.id, (long long)p->pid, (long long)damage.second,
                       (long long)b.boss.hp_left);
        }
    }

    for (const RaidBattle &b : t.expired) {
        w.db().save_raid(b, wall_ms);
        Buffer pr;
        {
            pb::Sub s(pr, 1);
            proto::build_raid_info(pr, b);
        }
        {
            pb::Sub s(pr, 2);
            pb::v(pr, 1, uint64_t(wall_ms));
        }
        Packet ev = make_event("raid_battle_get_info", pr.view(), true);
        const auto expired = make_event("raid_battle_expire_event", {});
        for (const auto &p : b.players)
            if (!p.finished && !p.left)
                w.hub().send_to_player(p.pid, expired);
        for (const RaidParticipant &p : b.players)
            if (!p.left)
                w.hub().send_to_player(p.pid, ev);
        LOG_INFO("raid %lld closed, %zu participants", (long long)b.id, b.players.size());
    }
}

void on_player_gone(Worker &w, int64_t player_id) {
    if (player_id <= 0)
        return;
    if (w.cfg().enable_clans) {
        std::lock_guard<std::mutex> operation(g_clan_ops);
        remove_from_clan_lobby(w, player_id);
    }
    if (!w.cfg().enable_raids)
        return;

    RaidBattle b;
    const bool in_raid = RaidEngine::instance().snapshot_of_player(player_id, b);
    if (in_raid) {
        const RaidParticipant *p = b.find(player_id);
        log::audit("raid_disconnect",
                   "player=%lld battle=%lld raid=%lld round_open=%d round_start_utc_ms=%lld "
                   "damage_total=%lld",
                   (long long)player_id, (long long)b.id, (long long)b.raid_id(),
                   int(p && p->round_opened_ms), p ? (long long)p->last_round_ms : 0,
                   p ? (long long)p->damage : 0);
    }

    RaidEngine::instance().drop(player_id);

    if (!in_raid)
        return;
    if (const RaidParticipant *me = b.find(player_id)) {
        RaidParticipant gone = *me;
        gone.state = kRaidIdle;
        Buffer pr;
        proto::build_raid_player(pr, gone);
        Packet ev = make_event("refresh_raid_player_event", pr.view());
        for (const RaidParticipant &q : b.players)
            if (q.pid != player_id)
                w.hub().send_to_player(q.pid, ev);
    }
}

}
