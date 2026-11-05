
#include "sf2/raid.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "sf2/crypto.hpp"
#include "sf2/log.hpp"

namespace sf2 {
namespace {

std::string trim(std::string_view s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos)
        return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string_view::npos) {
            out.push_back(trim(s.substr(start)));
            break;
        }
        out.push_back(trim(s.substr(start, p - start)));
        start = p + 1;
    }
    return out;
}

bool parse_named_list(const std::string &in, std::vector<NamedCount> &out, std::string &err) {
    out.clear();
    if (in.empty())
        return true;
    for (const std::string &piece : split(in, ',')) {
        if (piece.empty())
            continue;
        size_t colon = piece.rfind(':');
        if (colon == std::string::npos) {
            err = "expected name:count in '" + piece + "'";
            return false;
        }
        std::string name = trim(std::string_view(piece).substr(0, colon));
        std::string num = trim(std::string_view(piece).substr(colon + 1));
        if (name.empty() || name.size() > 64) {
            err = "bad item name in '" + piece + "'";
            return false;
        }
        char *end = nullptr;
        long long n = std::strtoll(num.c_str(), &end, 10);
        if (!end || *end || n < 0 || n > 1000000) {
            err = "bad count in '" + piece + "'";
            return false;
        }
        out.push_back(NamedCount{std::move(name), int64_t(n)});
        if (out.size() > 32) {
            err = "too many entries";
            return false;
        }
    }
    return true;
}

uint64_t random_below(uint64_t limit) {
    uint64_t raw = 0;
    crypto::random_bytes(reinterpret_cast<uint8_t *>(&raw), sizeof raw);
    return limit ? raw % limit : 0;
}

void stop_bot(RaidParticipant &p) {
    p.left = true;
    p.state = kRaidIdle;
    p.round_opened_ms = 0;
    p.bot_action_ms = 0;
    p.final_round = false;
}

int64_t random_id() {
    uint8_t raw[8];
    crypto::random_bytes(raw, sizeof raw);
    uint64_t v = 0;
    for (uint8_t byte : raw)
        v = (v << 8) | byte;

    return int64_t(100000000ULL + v % 800000000ULL);
}

}

bool parse_reward_tier(const std::string &spec, RaidRewardTier &out, std::string &err) {
    std::vector<std::string> parts = split(spec, '|');
    if (parts.size() != 5) {
        err = "expected 'chest | items | spheres | base | bonus'";
        return false;
    }
    if (parts[0].empty() || parts[0].size() > 64) {
        err = "bad chest name";
        return false;
    }
    RaidRewardTier t;
    t.chest.name = parts[0];
    if (!parse_named_list(parts[1], t.chest.items, err))
        return false;
    if (!parse_named_list(parts[2], t.chest.spheres, err))
        return false;
    char *end = nullptr;
    long long base = std::strtoll(parts[3].c_str(), &end, 10);
    if (!end || *end || base < 0) {
        err = "bad rating_base";
        return false;
    }
    long long bonus = std::strtoll(parts[4].c_str(), &end, 10);
    if (!end || *end || bonus < 0) {
        err = "bad rating_bonus";
        return false;
    }
    t.rating_base = int64_t(base);
    t.rating_bonus = int64_t(bonus);
    out = std::move(t);
    return true;
}

RaidRules default_raid_rules() {
    RaidRules r;

    r.place1 = {{"RaidChestKeyFungusFirstPlace",
                 {{"MonkShard", 40}, {"Key3", 1}},
                 {{"Sphere1_Name", 1}, {"Sphere2_Name", 2}}},
                2000,
                2500};
    r.place2 = {
        {"RaidChestKeyFungusSecondPlace", {{"MonkShard", 25}, {"Key3", 1}}, {{"Sphere1_Name", 1}}},
        1400,
        1600};
    r.place3 = {
        {"RaidChestKeyFungusThirdPlace", {{"MonkShard", 15}}, {{"Sphere1_Name", 1}}}, 1000, 1100};
    r.other = {{"RaidChestKeyFungusParticipant", {{"MonkShard", 8}}, {}}, 600, 700};
    return r;
}

const RaidParticipant *RaidBattle::find(int64_t pid) const noexcept {
    for (const RaidParticipant &p : players)
        if (p.pid == pid)
            return &p;
    return nullptr;
}

RaidParticipant *RaidBattle::find(int64_t pid) noexcept {
    for (RaidParticipant &p : players)
        if (p.pid == pid)
            return &p;
    return nullptr;
}

RaidEngine &RaidEngine::instance() {
    static RaidEngine e;
    return e;
}

void RaidEngine::configure(const RaidRules &rules) {
    std::lock_guard<std::mutex> lk(mu_);
    rules_ = rules;
    rules_.bot_min_damage = std::clamp<int64_t>(rules_.bot_min_damage, 1, 2000000000);
    rules_.bot_max_damage =
        std::clamp<int64_t>(rules_.bot_max_damage, rules_.bot_min_damage, 2000000000);
    rules_.bot_fill_wait_ms = std::max<uint64_t>(10000, rules_.bot_fill_wait_ms);
    if (rules_.max_rounds < 1)
        rules_.max_rounds = 1;
    if (rules_.default_boss_hp < 1)
        rules_.default_boss_hp = 1;
    if (rules_.fallback_party_size < 1)
        rules_.fallback_party_size = 1;
    if (rules_.fallback_duration_ms < 1000)
        rules_.fallback_duration_ms = 1000;
}

void RaidEngine::set_reward_builder(RewardBuilder fn) {
    std::lock_guard<std::mutex> lk(mu_);
    reward_builder_ = std::move(fn);
}

RaidEngine::Shape RaidEngine::shape_of(int64_t raid_id) const {
    const GameData &gd = GameData::instance();
    if (const RaidDef *d = gd.raid(raid_id)) {
        return Shape{d->party_size,
                     d->duration_ms,
                     d->round_cooldown_ms,
                     d->round_time_ms,
                     d->boss_hp > 0 ? d->boss_hp : rules_.default_boss_hp,
                     d->damage_scale > 0 ? d->damage_scale : 1.0,
                     true};
    }

    return Shape{rules_.fallback_party_size,
                 rules_.fallback_duration_ms,
                 rules_.fallback_cooldown_ms,
                 rules_.fallback_round_time_ms,
                 rules_.default_boss_hp,
                 1.0,
                 false};
}

const char *RaidEngine::status_text(Status s) noexcept {
    switch (s) {
    case Status::Ok:
        return "ok";
    case Status::NotInBattle:
        return "not in a raid";
    case Status::BattleOver:
        return "raid already finished";
    case Status::RoundAlreadyOpen:
        return "a round is already open";
    case Status::NoRoundOpen:
        return "no round was started";
    case Status::RoundTooFast:
        return "round finished implausibly fast";
    case Status::OutOfRounds:
        return "no rounds left";
    case Status::AlreadyFinished:
        return "player already cashed out";
    case Status::UnknownRaid:
        return "no such raid";
    case Status::RoundExpired:
        return "round outlived RoundTime";
    case Status::PaymentFailed:
        return "cooldown payment refused";
    case Status::OnCooldown:
        return "round cooldown has not elapsed";
    case Status::NotEntered:
        return "player never entered the raid";
    case Status::FinishFailed:
        return "lost the final battle";
    case Status::OutOfFinishTries:
        return "no final-battle attempts left";
    case Status::BossAlive:
        return "the boss is still standing";
    }
    return "?";
}

RaidBattle *RaidEngine::battle_locked(int64_t pid) {
    auto w = where_.find(pid);
    if (w == where_.end())
        return nullptr;
    auto b = battles_.find(w->second);
    if (b == battles_.end()) {
        where_.erase(w);
        return nullptr;
    }
    return &b->second;
}

RaidBattle RaidEngine::form_locked(int64_t raid_id, std::vector<int64_t> pids, uint64_t mono_ms,
                                   int64_t wall_ms) {
    const Shape sh = shape_of(raid_id);

    RaidBattle b;
    do {
        b.id = random_id();
    } while (battles_.count(b.id));

    b.boss.raid_id = raid_id;
    b.boss.hp_max = sh.hp > 0 ? sh.hp : 1;
    b.boss.hp_left = b.boss.hp_max;
    b.started_ms = wall_ms;
    b.started_mono = mono_ms;
    b.duration_ms = sh.duration_ms;
    b.round_cooldown_ms = sh.cooldown_ms;
    b.round_time_ms = sh.round_time_ms;
    b.damage_scale = sh.damage_scale > 0 ? sh.damage_scale : 1.0;

    b.players.reserve(pids.size());
    for (int64_t pid : pids) {
        RaidParticipant p;
        p.pid = pid;
        p.state = kRaidIdle;
        if (is_raid_bot(pid))
            p.bot_action_ms = mono_ms + 2000;
        b.players.push_back(p);
        where_[pid] = b.id;
    }
    battles_[b.id] = b;
    LOG_INFO("raid %lld formed: RaidID=%lld hp=%lld players=%zu duration=%llds", (long long)b.id,
             (long long)raid_id, (long long)b.boss.hp_max, b.players.size(),
             (long long)(b.duration_ms / 1000));
    return b;
}

RaidEngine::MatchResult RaidEngine::join_matcher(int64_t pid, int64_t raid_id, uint64_t mono_ms,
                                                 int64_t wall_ms) {
    MatchResult res;
    std::lock_guard<std::mutex> lk(mu_);

    const Shape sh = shape_of(raid_id);

    if (pid <= 0 || is_raid_bot(pid) || raid_id < 0 ||
        (GameData::instance().have_raids() && !sh.known)) {
        res.rejected = true;
        return res;
    }

    if (RaidBattle *cur = battle_locked(pid)) {
        const RaidParticipant *me = cur->find(pid);
        if (!cur->closed && me && !me->finished && !me->left) {
            res.already_in = true;
            res.battle = *cur;
            return res;
        }

        where_.erase(pid);
    }

    for (auto &kv : queues_) {
        auto &v = kv.second.waiters;
        v.erase(std::remove_if(v.begin(), v.end(), [pid](const Waiter &w) { return w.pid == pid; }),
                v.end());
    }

    Queue &q = queues_[raid_id];
    q.waiters.push_back(Waiter{pid, mono_ms});

    std::vector<int64_t> pids;
    std::vector<Waiter> keep;
    keep.reserve(q.waiters.size());
    for (const Waiter &w : q.waiters) {
        const bool eligible = mono_ms - w.since >= rules_.match_wait_ms;
        if (eligible && int64_t(pids.size()) < sh.party_size)
            pids.push_back(w.pid);
        else
            keep.push_back(w);
    }
    if (int64_t(pids.size()) >= sh.party_size) {
        q.waiters = std::move(keep);
        res.formed = true;
        res.battle = form_locked(raid_id, std::move(pids), mono_ms, wall_ms);
        return res;
    }

    for (const Waiter &w : q.waiters)
        if (w.pid != pid)
            res.waiting.push_back(w.pid);
    return res;
}

void RaidEngine::leave_matcher(int64_t pid) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto &kv : queues_) {
        auto &v = kv.second.waiters;
        v.erase(std::remove_if(v.begin(), v.end(), [pid](const Waiter &w) { return w.pid == pid; }),
                v.end());
    }
}

bool RaidEngine::start_clan_raid(int64_t raid_id, const std::vector<int64_t> &players,
                                 uint64_t mono_ms, int64_t wall_ms, RaidBattle &out) {
    if (players.empty())
        return false;
    const Shape sh = shape_of(raid_id);
    if (GameData::instance().have_raids() && !sh.known)
        return false;

    if (players.size() != size_t(std::max<int64_t>(1, sh.party_size)))
        return false;
    std::vector<int64_t> unique = players;
    std::sort(unique.begin(), unique.end());
    if (std::adjacent_find(unique.begin(), unique.end()) != unique.end())
        return false;
    std::lock_guard<std::mutex> lk(mu_);

    for (int64_t pid : players) {
        if (pid <= 0 || is_raid_bot(pid))
            return false;
        if (RaidBattle *cur = battle_locked(pid)) {
            const RaidParticipant *me = cur->find(pid);
            if (!cur->closed && me && !me->finished && !me->left)
                return false;
        }
    }
    for (int64_t pid : players) {
        where_.erase(pid);
        for (auto &kv : queues_) {
            auto &v = kv.second.waiters;
            v.erase(
                std::remove_if(v.begin(), v.end(), [pid](const Waiter &w) { return w.pid == pid; }),
                v.end());
        }
    }
    out = form_locked(raid_id, players, mono_ms, wall_ms);
    return true;
}

void RaidEngine::retire_locked(uint64_t mono_ms, std::vector<RaidBattle> &out) {
    for (auto it = battles_.begin(); it != battles_.end();) {
        RaidBattle &b = it->second;
        const uint64_t age = mono_ms - b.started_mono;
        if (!b.closed && age >= uint64_t(b.duration_ms)) {
            b.closed = true;
            for (auto &p : b.players)
                if (is_raid_bot(p.pid))
                    stop_bot(p);
            rank_locked(b);
            out.push_back(b);
            LOG_INFO("raid %lld expired", (long long)b.id);
        }
        if (b.closed && age >= uint64_t(b.duration_ms) + rules_.keep_finished_ms) {
            for (const RaidParticipant &p : b.players) {
                auto w = where_.find(p.pid);
                if (w != where_.end() && w->second == b.id)
                    where_.erase(w);
            }
            it = battles_.erase(it);
            continue;
        }
        ++it;
    }
}

RaidEngine::Tick RaidEngine::tick(uint64_t mono_ms, int64_t wall_ms) {
    Tick t;
    std::lock_guard<std::mutex> lk(mu_);

    for (auto &kv : queues_) {
        Queue &q = kv.second;
        if (q.waiters.empty())
            continue;

        const Shape sh = shape_of(kv.first);
        const size_t needed = size_t(sh.party_size);

        std::vector<int64_t> pids;
        std::vector<Waiter> keep;
        keep.reserve(q.waiters.size());
        for (const Waiter &w : q.waiters) {
            const bool eligible = mono_ms - w.since >= rules_.match_wait_ms;
            if (eligible && pids.size() < needed)
                pids.push_back(w.pid);
            else
                keep.push_back(w);
        }
        if (pids.size() < needed) {
            const bool waited =
                !pids.empty() &&
                std::any_of(q.waiters.begin(), q.waiters.end(), [&](const Waiter &w) {
                    return mono_ms - w.since >= rules_.bot_fill_wait_ms;
                });
            if (!rules_.bots_enabled || !waited)
                continue;
            while (pids.size() < needed)
                pids.push_back(next_bot_id_++);
        }

        q.waiters = std::move(keep);
        t.formed.push_back(form_locked(kv.first, std::move(pids), mono_ms, wall_ms));
    }

    bots_locked(mono_ms, wall_ms, t);
    retire_locked(mono_ms, t.expired);
    return t;
}

void RaidEngine::bots_locked(uint64_t mono_ms, int64_t wall_ms, Tick &tick) {
    for (auto &entry : battles_) {
        RaidBattle &b = entry.second;
        if (b.closed || mono_ms - b.started_mono >= uint64_t(b.duration_ms))
            continue;
        const bool has_bot =
            std::any_of(b.players.begin(), b.players.end(),
                        [](const RaidParticipant &p) { return is_raid_bot(p.pid); });
        if (!has_bot)
            continue;
        const bool human_active =
            std::any_of(b.players.begin(), b.players.end(), [](const RaidParticipant &p) {
                return !is_raid_bot(p.pid) && !p.left && !p.finished;
            });
        const bool human_finished =
            std::any_of(b.players.begin(), b.players.end(),
                        [](const RaidParticipant &p) { return !is_raid_bot(p.pid) && p.finished; });
        bool changed = false;
        BotUpdate update;
        for (auto &p : b.players) {
            if (!is_raid_bot(p.pid) || p.left)
                continue;
            if (!human_active || human_finished || b.boss.hp_left <= 0 ||
                p.rounds_used >= rules_.max_rounds) {
                stop_bot(p);
                changed = true;
                continue;
            }
            if (mono_ms < p.bot_action_ms)
                continue;
            changed = true;
            if (p.state != kRaidInRound) {
                p.joined = true;
                p.state = kRaidInRound;
                p.round_opened_ms = mono_ms;
                p.last_round_ms = wall_ms;
                constexpr uint64_t min_fight_ms = 30000, max_fight_ms = 45000;
                const uint64_t duration =
                    min_fight_ms + random_below(max_fight_ms - min_fight_ms + 1);
                int64_t min_damage = rules_.bot_min_damage, max_damage = rules_.bot_max_damage;
                if (const auto *def = GameData::instance().raid(b.raid_id())) {
                    if (def->tier == 2) {
                        min_damage = 500;
                        max_damage = 1500;
                    } else if (def->tier == 3 || def->tier == 4) {
                        min_damage = 750;
                        max_damage = 2000;
                    }
                }
                const int64_t span = max_damage - min_damage;

                p.bot_round_damage = min_damage + (int64_t(random_below(uint64_t(span + 1))) * 2 +
                                                   span * int64_t(duration - min_fight_ms) /
                                                       int64_t(max_fight_ms - min_fight_ms)) /
                                                      3;
                p.bot_action_ms = mono_ms + duration;
            } else {
                const int64_t applied = std::min(p.bot_round_damage, b.boss.hp_left);
                b.boss.hp_left -= applied;
                p.damage += applied;
                ++p.rounds_used;
                p.state = kRaidIdle;
                p.bot_action_ms = std::max(mono_ms + 2000,
                                           p.round_opened_ms +
                                               uint64_t(std::max<int64_t>(0, b.round_cooldown_ms)));
                p.round_opened_ms = 0;
                update.damage.emplace_back(p.pid, applied);
            }
        }

        if (b.boss.hp_left <= 0 || !human_active || human_finished) {
            for (auto &p : b.players) {
                if (is_raid_bot(p.pid) && !p.left) {
                    stop_bot(p);
                    changed = true;
                }
            }
        }
        if (!human_active && !b.closed) {
            b.closed = true;
            changed = true;
        }
        if (changed) {
            rank_locked(b);
            update.battle = b;
            tick.bots.push_back(std::move(update));
        }
    }
}

bool RaidEngine::snapshot(int64_t raid_id, RaidBattle &out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = battles_.find(raid_id);
    if (it == battles_.end())
        return false;
    out = it->second;
    return true;
}

bool RaidEngine::snapshot_of_player(int64_t pid, RaidBattle &out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto w = where_.find(pid);
    if (w == where_.end())
        return false;
    auto it = battles_.find(w->second);
    if (it == battles_.end())
        return false;
    out = it->second;
    return true;
}

int64_t RaidEngine::battle_of(int64_t pid) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto w = where_.find(pid);
    return w == where_.end() ? 0 : w->second;
}

RaidEngine::Status RaidEngine::player_start(int64_t pid, RaidBattle &out) {
    std::lock_guard<std::mutex> lk(mu_);
    RaidBattle *b = battle_locked(pid);
    if (!b)
        return Status::NotInBattle;
    if (b->closed) {
        out = *b;
        return Status::BattleOver;
    }
    RaidParticipant *p = b->find(pid);
    if (!p)
        return Status::NotInBattle;
    out = *b;
    if (p->finished)
        return Status::AlreadyFinished;
    p->joined = true;
    p->left = false;
    out = *b;
    return Status::Ok;
}

RaidEngine::Status RaidEngine::start_round(int64_t pid, uint64_t mono_ms, int64_t wall_ms,
                                           RaidBattle &out, int64_t offered_gems,
                                           const std::function<bool(int64_t)> &pay) {
    std::lock_guard<std::mutex> lk(mu_);
    RaidBattle *b = battle_locked(pid);
    if (!b)
        return Status::NotInBattle;
    if (b->closed) {
        out = *b;
        return Status::BattleOver;
    }
    RaidParticipant *p = b->find(pid);
    if (!p)
        return Status::NotInBattle;

    out = *b;
    if (p->finished)
        return Status::AlreadyFinished;

    if (!p->joined)
        return Status::NotEntered;
    if (p->state == kRaidInRound)
        return Status::RoundAlreadyOpen;

    const bool final_battle = b->boss.hp_left <= 0;

    if (!final_battle && p->rounds_used >= rules_.max_rounds)
        return Status::OutOfRounds;

    int64_t debit = 0;
    if (offered_gems < 0)
        return Status::PaymentFailed;
    if (!final_battle && p->last_round_ms && b->round_cooldown_ms > 0) {
        const int64_t remaining = p->last_round_ms + b->round_cooldown_ms - wall_ms;
        if (remaining > 0) {
            if (offered_gems > 0) {
                const auto *def = GameData::instance().raid(b->raid_id());
                if (!def || def->cd_min_time_s <= 0 || def->cd_cost <= 0 || !pay)
                    return Status::PaymentFailed;
                const int64_t unit = def->cd_min_time_s * 1000;
                const int64_t cost = ((remaining + unit - 1) / unit) * def->cd_cost;

                if (cost > offered_gems)
                    return Status::PaymentFailed;
                debit = cost;
            } else if (rules_.enforce_round_cooldown)
                return Status::OnCooldown;
        }
    }

    if (pay && !pay(debit))
        return Status::PaymentFailed;
    p->state = kRaidInRound;
    p->final_round = final_battle;
    p->round_opened_ms = mono_ms;
    p->last_round_ms = wall_ms;
    out = *b;
    return Status::Ok;
}

int64_t RaidEngine::next_attempt_ms(int64_t pid, int64_t wall_ms) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto w = where_.find(pid);
    if (w == where_.end())
        return wall_ms;
    auto it = battles_.find(w->second);
    if (it == battles_.end())
        return wall_ms;
    const RaidBattle &b = it->second;
    const RaidParticipant *p = b.find(pid);

    const int64_t past = (b.started_ms > 0 ? b.started_ms : wall_ms) - 60000;
    if (!p || !p->last_round_ms)
        return past;
    if (b.boss.hp_left <= 0)
        return past;
    return p->last_round_ms + b.round_cooldown_ms;
}

RaidEngine::Status RaidEngine::finish_round(int64_t pid, int64_t claimed, uint64_t mono_ms,
                                            int64_t &applied, RaidBattle &out) {
    applied = 0;
    std::lock_guard<std::mutex> lk(mu_);
    RaidBattle *b = battle_locked(pid);
    if (!b)
        return Status::NotInBattle;
    if (b->closed) {
        out = *b;
        return Status::BattleOver;
    }
    RaidParticipant *p = b->find(pid);
    if (!p)
        return Status::NotInBattle;
    out = *b;
    if (p->finished)
        return Status::AlreadyFinished;
    if (!p->joined)
        return Status::NotEntered;

    if (std::any_of(b->players.begin(), b->players.end(),
                    [](const RaidParticipant &member) { return member.finished; })) {
        p->state = kRaidIdle;
        p->round_opened_ms = 0;
        p->final_round = false;
        out = *b;
        return Status::BattleOver;
    }
    if (p->final_round)
        return Status::NoRoundOpen;
    if (p->state != kRaidInRound || p->round_opened_ms == 0)
        return Status::NoRoundOpen;

    if (rules_.enforce_round_expiry && b->round_time_ms > 0 &&
        mono_ms - p->round_opened_ms > uint64_t(b->round_time_ms) + rules_.round_grace_ms) {
        p->state = kRaidIdle;
        p->round_opened_ms = 0;
        p->rounds_used += 1;
        out = *b;
        return Status::RoundExpired;
    }

    if (mono_ms - p->round_opened_ms < rules_.min_round_ms) {

        p->state = kRaidIdle;
        p->round_opened_ms = 0;
        p->rounds_used += 1;
        out = *b;
        return Status::RoundTooFast;
    }

    int64_t dmg = claimed;
    if (dmg < 0)
        dmg = 0;
    if (dmg > rules_.max_round_dmg)
        dmg = rules_.max_round_dmg;
    dmg = std::min(dmg, std::numeric_limits<int64_t>::max() - p->damage);

    int64_t to_boss = dmg;
    if (b->damage_scale > 0 && b->damage_scale != 1.0) {
        const double scaled = double(dmg) * b->damage_scale;
        to_boss = scaled >= 9.0e15 ? int64_t(9.0e15) : int64_t(scaled);
    }
    if (to_boss > b->boss.hp_left)
        to_boss = b->boss.hp_left;

    b->boss.hp_left -= to_boss;
    p->damage += dmg;
    p->rounds_used += 1;
    p->state = kRaidIdle;
    p->round_opened_ms = 0;
    applied = dmg;

    rank_locked(*b);
    out = *b;
    return Status::Ok;
}

void RaidEngine::rank_locked(RaidBattle &b) const {
    std::vector<const RaidParticipant *> order;
    order.reserve(b.players.size());
    for (const RaidParticipant &p : b.players)
        order.push_back(&p);
    std::stable_sort(order.begin(), order.end(),
                     [](const RaidParticipant *a, const RaidParticipant *c) {
                         if (a->damage != c->damage)
                             return a->damage > c->damage;
                         return a->pid < c->pid;
                     });

    int64_t place = 0, seen = 0, last = -1;
    for (const RaidParticipant *p : order) {
        ++seen;
        if (p->damage != last) {
            place = seen;
            last = p->damage;
        }
        b.find(p->pid)->place = place;
    }
}

RaidReward RaidEngine::reward_for(int64_t place, int64_t damage) const {
    const RaidRewardTier *tier = &rules_.other;
    if (place == 1)
        tier = &rules_.place1;
    else if (place == 2)
        tier = &rules_.place2;
    else if (place == 3)
        tier = &rules_.place3;

    RaidReward r;
    r.present = true;
    r.chest = tier->chest;
    r.rating_base = tier->rating_base;
    r.rating_bonus = tier->rating_bonus;
    r.damage = damage;
    return r;
}

RaidEngine::Status RaidEngine::player_finish(int64_t pid, bool won, RaidReward &reward,
                                             RaidBattle &out) {
    std::lock_guard<std::mutex> lk(mu_);
    RaidBattle *b = battle_locked(pid);
    if (!b)
        return Status::NotInBattle;
    RaidParticipant *p = b->find(pid);
    if (!p)
        return Status::NotInBattle;
    if (p->finished) {

        reward = p->reward;
        out = *b;
        return Status::AlreadyFinished;
    }
    if (!p->joined) {
        out = *b;
        return Status::NotEntered;
    }

    if (rules_.require_boss_dead && b->boss.hp_left > 0) {
        out = *b;
        return Status::BossAlive;
    }

    if (!won) {
        p->finish_tries += 1;
        p->state = kRaidIdle;
        p->round_opened_ms = 0;
        out = *b;
        return Status::FinishFailed;
    }
    p->finish_tries += 1;

    rank_locked(*b);
    p->finished = true;
    p->state = kRaidIdle;
    p->round_opened_ms = 0;
    RaidReward built;
    const bool priced = reward_builder_ && reward_builder_(*b, *p, built);
    p->reward = priced ? built : reward_for(p->place, p->damage);
    reward = p->reward;

    for (auto &q : b->players)
        if (is_raid_bot(q.pid))
            stop_bot(q);
    bool all_done = true;
    for (const RaidParticipant &q : b->players)
        if (!q.finished && !q.left) {
            all_done = false;
            break;
        }
    if (all_done)
        b->closed = true;

    out = *b;
    return Status::Ok;
}

void RaidEngine::leave(int64_t pid, int64_t raid_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto w = where_.find(pid);
    if (w == where_.end())
        return;
    if (raid_id && w->second != raid_id)
        return;
    auto it = battles_.find(w->second);
    if (it != battles_.end()) {
        if (RaidParticipant *p = it->second.find(pid)) {
            p->left = true;
            p->state = kRaidIdle;
            p->round_opened_ms = 0;
        }
    }
    where_.erase(w);
}

void RaidEngine::drop(int64_t pid) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto &kv : queues_) {
        auto &v = kv.second.waiters;
        v.erase(std::remove_if(v.begin(), v.end(), [pid](const Waiter &w) { return w.pid == pid; }),
                v.end());
    }
    auto w = where_.find(pid);
    if (w == where_.end())
        return;
    auto it = battles_.find(w->second);
    if (it == battles_.end())
        return;

    if (RaidParticipant *p = it->second.find(pid)) {
        p->state = kRaidIdle;
        p->round_opened_ms = 0;
    }
}

RaidEngine::MatcherStatus RaidEngine::matcher_status(int64_t pid) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto &kv : queues_) {
        const auto &waiters = kv.second.waiters;
        auto me = std::find_if(waiters.begin(), waiters.end(),
                               [pid](const Waiter &w) { return w.pid == pid; });
        if (me == waiters.end())
            continue;
        MatcherStatus status;
        status.raid_id = kv.first;
        status.capacity = shape_of(kv.first).party_size;
        const size_t capacity = size_t(std::max<int64_t>(1, status.capacity));
        const size_t begin = size_t(me - waiters.begin()) / capacity * capacity;
        for (size_t i = begin; i < std::min(begin + capacity, waiters.size()); ++i)
            status.players.push_back(waiters[i].pid);
        return status;
    }
    return {};
}

bool RaidEngine::is_queued(int64_t pid) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto &kv : queues_)
        for (const Waiter &w : kv.second.waiters)
            if (w.pid == pid)
                return true;
    return false;
}

size_t RaidEngine::active_battles() const {
    std::lock_guard<std::mutex> lk(mu_);
    return battles_.size();
}

size_t RaidEngine::queued_players() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto &kv : queues_)
        n += kv.second.waiters.size();
    return n;
}

}
