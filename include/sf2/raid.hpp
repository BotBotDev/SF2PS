

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sf2/config.hpp"
#include "sf2/gamedata.hpp"
#include "sf2/player.hpp"

namespace sf2 {

constexpr int64_t kRaidBotIdBase = 4000000000000000LL;

inline bool is_raid_bot(int64_t pid) noexcept {
    return pid >= kRaidBotIdBase && pid < kRaidBotIdBase + 1000000000000000LL;
}

struct RaidBoss {
    int64_t raid_id = 1;
    int64_t hp_left = 0;
    int64_t hp_max = 0;
};

enum : int64_t {
    kRaidIdle = 1,
    kRaidInRound = 2,
};

enum : int64_t {
    kPresenceOffline = 1,
    kPresenceOnline = 2,
    kPresenceInRaid = 3,
};

struct RaidChest {
    std::string name;
    std::vector<NamedCount> items;
    std::vector<NamedCount> spheres;
    int64_t gems = 0, rating = 0;
};

struct RaidReward {
    bool present = false;
    RaidChest chest;
    int64_t rating_base = 0;
    int64_t rating_bonus = 0;
    int64_t damage = 0;
    int64_t gems = 0;

    int64_t total_rating() const noexcept {
        return rating_base + rating_bonus + damage;
    }
};

struct RaidParticipant {
    int64_t pid = 0;
    int64_t damage = 0;
    int64_t rounds_used = 0;
    bool joined = false;
    int64_t state = kRaidIdle;
    bool finished = false;
    bool left = false;
    int64_t place = 0;
    int64_t finish_tries = 0;
    RaidReward reward;
    uint64_t bot_action_ms = 0;
    int64_t bot_round_damage = 0;

    uint64_t round_opened_ms = 0;
    bool final_round = false;

    int64_t last_round_ms = 0;
};

struct RaidBattle {
    int64_t id = 0;
    RaidBoss boss;
    int64_t started_ms = 0;
    int64_t duration_ms = 0;
    int64_t season_id = 0;
    bool closed = false;

    int64_t round_cooldown_ms = 0;
    int64_t round_time_ms = 45000;
    double damage_scale = 1.0;

    uint64_t started_mono = 0;
    std::vector<RaidParticipant> players;

    int64_t raid_id() const noexcept {
        return boss.raid_id;
    }

    int64_t ends_ms() const noexcept {
        return started_ms + duration_ms;
    }

    const RaidParticipant *find(int64_t pid) const noexcept;
    RaidParticipant *find(int64_t pid) noexcept;
};

struct RaidRewardTier {
    RaidChest chest;
    int64_t rating_base = 0;
    int64_t rating_bonus = 0;
};

struct RaidRules {
    bool bots_enabled = false;
    uint64_t bot_fill_wait_ms = 10000;
    int64_t bot_min_damage = 100;
    int64_t bot_max_damage = 1000;
    uint64_t match_wait_ms = 3000;
    int64_t max_rounds = 20;

    uint64_t min_round_ms = 1500;
    uint64_t round_grace_ms = 60000;
    int64_t max_round_dmg = 200000;
    int64_t max_finish_tries = 3;
    int64_t round_rating = 0;
    int64_t keep_finished_ms = 300000;

    bool enforce_round_cooldown = false;
    bool enforce_damage_threshold = true;
    bool enforce_round_expiry = true;

    bool require_boss_dead = true;

    int64_t fallback_party_size = 6;
    int64_t fallback_duration_ms = 763000;
    int64_t fallback_cooldown_ms = 100000;
    int64_t fallback_round_time_ms = 45000;
    int64_t default_boss_hp = 10095;

    RaidRewardTier place1, place2, place3, other;
};

bool parse_reward_tier(const std::string &spec, RaidRewardTier &out, std::string &err);
RaidRules default_raid_rules();

class RaidEngine {
  public:
    static RaidEngine &instance();

    void configure(const RaidRules &rules);

    const RaidRules &rules() const noexcept {
        return rules_;
    }

    using RewardBuilder =
        std::function<bool(const RaidBattle &, const RaidParticipant &, RaidReward &)>;
    void set_reward_builder(RewardBuilder fn);

    enum class Status {
        Ok,
        NotInBattle,
        BattleOver,
        RoundAlreadyOpen,
        NoRoundOpen,
        RoundTooFast,
        OutOfRounds,
        AlreadyFinished,
        UnknownRaid,
        RoundExpired,
        OnCooldown,
        PaymentFailed,
        NotEntered,
        FinishFailed,
        OutOfFinishTries,
        BossAlive,
    };
    static const char *status_text(Status s) noexcept;

    struct MatchResult {
        bool formed = false;
        RaidBattle battle;
        std::vector<int64_t> waiting;
        bool rejected = false;

        bool already_in = false;
    };

    MatchResult join_matcher(int64_t pid, int64_t raid_id, uint64_t mono_ms, int64_t wall_ms);
    void leave_matcher(int64_t pid);

    bool start_clan_raid(int64_t raid_id, const std::vector<int64_t> &players, uint64_t mono_ms,
                         int64_t wall_ms, RaidBattle &out);

    struct BotUpdate {
        RaidBattle battle;
        std::vector<std::pair<int64_t, int64_t>> damage;
    };

    struct Tick {
        std::vector<RaidBattle> formed;
        std::vector<RaidBattle> expired;
        std::vector<BotUpdate> bots;
    };

    Tick tick(uint64_t mono_ms, int64_t wall_ms);

    bool snapshot(int64_t raid_id, RaidBattle &out) const;
    bool snapshot_of_player(int64_t pid, RaidBattle &out) const;
    int64_t battle_of(int64_t pid) const;

    Status player_start(int64_t pid, RaidBattle &out);
    Status start_round(int64_t pid, uint64_t mono_ms, int64_t wall_ms, RaidBattle &out,
                       int64_t offered_gems = 0, const std::function<bool(int64_t)> &pay = {});

    Status finish_round(int64_t pid, int64_t claimed, uint64_t mono_ms, int64_t &applied,
                        RaidBattle &out);

    int64_t next_attempt_ms(int64_t pid, int64_t wall_ms) const;

    Status player_finish(int64_t pid, bool won, RaidReward &reward, RaidBattle &out);
    void leave(int64_t pid, int64_t raid_id);
    void drop(int64_t pid);

    RaidReward reward_for(int64_t place, int64_t damage) const;

    struct MatcherStatus {
        int64_t raid_id = 0, capacity = 0;
        std::vector<int64_t> players;
    };

    MatcherStatus matcher_status(int64_t pid) const;
    bool is_queued(int64_t pid) const;
    size_t active_battles() const;
    size_t queued_players() const;

  private:
    RaidEngine() : rules_(default_raid_rules()) {}

    struct Waiter {
        int64_t pid;
        uint64_t since;
    };

    struct Queue {
        std::vector<Waiter> waiters;
    };

    RaidBattle form_locked(int64_t raid_id, std::vector<int64_t> pids, uint64_t mono_ms,
                           int64_t wall_ms);

    struct Shape {
        int64_t party_size, duration_ms, cooldown_ms, round_time_ms, hp;
        double damage_scale;
        bool known;
    };

    Shape shape_of(int64_t raid_id) const;
    void bots_locked(uint64_t mono_ms, int64_t wall_ms, Tick &tick);
    void rank_locked(RaidBattle &b) const;
    RaidBattle *battle_locked(int64_t pid);
    void retire_locked(uint64_t mono_ms, std::vector<RaidBattle> &out);

    mutable std::mutex mu_;
    RaidRules rules_;
    RewardBuilder reward_builder_;
    std::unordered_map<int64_t, Queue> queues_;
    std::unordered_map<int64_t, RaidBattle> battles_;
    std::unordered_map<int64_t, int64_t> where_;
    int64_t next_bot_id_ = kRaidBotIdBase;
};

}
