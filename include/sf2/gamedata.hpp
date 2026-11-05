

#pragma once
#include "sf2/progress.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sf2/player.hpp"

namespace sf2 {

struct RewardTier {
    int64_t pos_to = 0;
    double pos_rating_scaler = 0;
    std::string container;
};

struct RaidRewards {

    double threshold_damage_fraction = 0;
    int64_t base_rating = 0;
    int64_t pos_rating = 0;
    double damage_rating_scaler = 1.0;
    std::vector<RewardTier> tiers;

    const RewardTier *for_place(int64_t place) const noexcept;
};

struct RaidCost {
    std::string currency;
    int64_t value = 0;
};

struct RaidDef {
    int64_t id = 0;
    int tier = 1;
    std::string name;
    std::string alias;
    std::string title;
    std::string location;
    bool hard = false;
    bool hidden = false;

    int64_t boss_mmr = 0;
    int64_t party_size = 4;
    int64_t dan_restriction = 0;
    int64_t final_battle_power = 0;
    int64_t raid_time_ms = 0;
    int64_t duration_ms = 0;
    int64_t round_cooldown_ms = 0;
    int64_t round_time_ms = 45000;
    int64_t cd_min_time_s = 0;
    int64_t cd_cost = 0;

    std::vector<RaidCost> costs;
    RaidRewards rewards;

    int64_t shield_total = 0;
    int64_t shield_stack = 0;

    int64_t boss_hp = 0;

    double damage_scale = 1.0;

    bool solo() const noexcept {
        return party_size <= 1;
    }
};

struct ShopPack {
    std::string name, container, start, end;
    int64_t price = -1, min_level = 0, max_level = 0;
    int64_t target_dan = 0;
    bool purchasable = false;
};

struct SphereDef {
    std::string name;
    int64_t charge_damage = 0;
    double dispersion = 0;

    int64_t min_damage() const noexcept {
        return int64_t(double(charge_damage) * (1.0 - dispersion / 100.0));
    }

    int64_t max_damage() const noexcept {
        return int64_t(double(charge_damage) * (1.0 + dispersion / 100.0)) + 1;
    }
};

struct FightRules {
    bool loaded = false;
    double crit_probability = 0.2;
    int64_t crit_additional = 5;
    double damage_factor_max = 20000;
    std::vector<std::pair<std::string, int64_t>> hit_budgets;

    int64_t max_uses(std::string_view damage_type) const noexcept;
};

struct MoveDef {
    int64_t id = 0;
    std::string name;
    std::string damage_type;
    bool enabled = true;
    std::vector<int64_t> raids;

    bool usable_in(int64_t raid_id) const noexcept;
};

enum class LootKind : uint8_t {
    And,
    Or,
    WeightedRand,
    Currency,
    Consumable,
    Gems,
    Rating,
};

struct LootNode {
    LootKind kind = LootKind::And;
    std::string name;
    int64_t value = 0;
    double weight = 1.0;
    double probability = 1.0;
    bool gift = false;
    std::vector<uint32_t> kids;
};

struct LootRoll {
    std::vector<NamedCount> currency;
    std::vector<NamedCount> consumables;
    int64_t gems = 0;
    int64_t rating = 0;

    bool empty() const noexcept {
        return currency.empty() && consumables.empty() && !gems && !rating;
    }
};

struct LeagueReward {
    int64_t pos_to = 0;
    std::string container;
    bool dan_up = false;
};

struct LeagueDan {
    int64_t dan = 0;
    int64_t min_size = 0, max_size = 0;
    int64_t min_reward_rating = 0;
    std::vector<LeagueReward> rewards;
};

struct LeagueSettings {
    bool loaded = false;
    int64_t update_interval_ms = 600000;
    int64_t max_rows = 100;
    std::string restore_currency;
    int64_t restore_value = 0;

    std::string schedule_cron;
    std::string day_schedule_cron;
    std::vector<LeagueDan> dans;
    std::vector<int64_t> medal_positions;

    const LeagueDan *for_rating(int64_t rating) const noexcept;
};

struct ClanLimits {
    bool loaded = false;
    int64_t icon_count = 36, back_count = 10, color_count = 45;
    int64_t name_max = 16;
    int64_t desc_max = 200;
    int64_t chat_max = 256;
    int64_t max_size = 40;
    int64_t create_price_gems = 349;
    int64_t kick_cooldown_ms = 86400000;
    int64_t raid_kick_cooldown_ms = 600000;
    std::vector<std::pair<std::string, int64_t>> permissions;
    std::vector<std::pair<std::string, int64_t>> roles;
    std::vector<std::pair<int64_t, double>> rating_weights;

    double weight_for_position(int64_t pos) const noexcept;
};

struct Timings {
    int64_t finish_raid_delta_ms = 3000;
    int64_t matching_timeout_ms = 120000;
    int64_t join_zone_timeout_ms = 5000;
    int64_t ping_delay_ms = 5000;
    int64_t update_client_profile_ms = 1000;
};

class GameData {
  public:
    static GameData &instance();

    struct Report {
        bool ok = false;
        int raids = 0, moves = 0, containers = 0, league_dans = 0, spheres = 0;
        std::string resolved;
        std::vector<std::string> searched;
        std::vector<std::string> problems;
        std::string summary() const;
    };

    static std::vector<std::string> search_roots(const std::string &dir,
                                                 const std::string &config_path);

    Report load(const std::string &dir, const std::string &config_path = {});

    bool have_raids() const noexcept {
        return !raids_.empty();
    }

    bool have_moves() const noexcept {
        return !moves_.empty();
    }

    bool have_loot() const noexcept {
        return !containers_.empty();
    }

    const RaidDef *raid(int64_t id) const noexcept;
    RaidDef *raid_mut(int64_t id) noexcept;
    std::vector<const RaidDef *> raids() const;

    const FightRules &fight() const noexcept {
        return fight_;
    }

    const LeagueSettings &league() const noexcept {
        return league_;
    }

    const ClanLimits &clan() const noexcept {
        return clan_;
    }

    const Timings &timings() const noexcept {
        return timings_;
    }

    const MoveDef *move(int64_t id) const noexcept;

    size_t move_count() const noexcept {
        return moves_.size();
    }

    const std::vector<SphereDef> &spheres() const noexcept {
        return spheres_;
    }

    const SphereDef *sphere(std::string_view name) const noexcept;

    bool has_container(std::string_view name) const noexcept;

    bool roll(std::string_view name, LootRoll &out) const;

    bool gift_container(std::string_view name) const;

    const DailySettings &daily() const {
        return daily_;
    }

    const BpSettings &battle_pass() const {
        return bp_;
    }

    const ShopPack *pack(std::string_view name) const;
    bool potion(std::string_view name) const;

  private:
    GameData() = default;

    bool load_raids(const std::string &dir, Report &rep);
    bool load_moves(const std::string &dir, Report &rep);
    bool load_loot(const std::string &dir, Report &rep);
    bool load_league(const std::string &dir, Report &rep);
    bool load_spheres(const std::string &dir, Report &rep);
    bool load_progress(const std::string &dir, Report &rep);
    bool load_clan(const std::string &dir, Report &rep);

    void roll_node(uint32_t idx, LootRoll &out, int depth) const;

    std::unordered_map<int64_t, RaidDef> raids_;
    std::unordered_map<int64_t, MoveDef> moves_;
    std::vector<LootNode> loot_;
    std::unordered_map<std::string, uint32_t> containers_;
    std::vector<SphereDef> spheres_;
    std::unordered_map<std::string, ShopPack> packs_;
    std::vector<std::string> potions_;
    FightRules fight_;
    LeagueSettings league_;
    ClanLimits clan_;
    Timings timings_;
    DailySettings daily_;
    BpSettings bp_;
};

}
