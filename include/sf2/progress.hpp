#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace sf2 {
struct DailyRule {
    int64_t id = 0, target = 0, raid = -1, length = 0, style = 0;
    std::string group, trigger, reward, damage_type;
    std::vector<std::pair<std::string, int64_t>> dan_conditions;
    bool eligible(int64_t dan) const;
};

struct DailySettings {
    int64_t renew_ms = 0, replace_ms = 0, cd_unit_s = 0, cd_cost = 0;
    std::vector<DailyRule> quests;
    const DailyRule *quest(int64_t id) const;
};

struct BpLevel {
    int64_t number = 0, points = 0;
    bool premium = false;
    std::string loot, replacement;
};

struct BpPass {
    std::string name;
    int64_t start_ms = 0, end_ms = 0;
    std::vector<BpLevel> levels;
};

struct BpSettings {
    std::string points, premium, premium_pack;
    std::vector<BpPass> passes;
    const BpPass *active(int64_t now) const;
};

struct DailyEvent {
    int64_t battle = 0, raid = 0, round = 0, damage = 0, crits = 0, style = 0, place = 0;
    bool survived = false, finished = false, won = false;
    std::vector<std::pair<std::string, int64_t>> hits;
    std::vector<std::pair<int64_t, int64_t>> combos;
};

int64_t parse_utc_ms(const std::string &text);
}
