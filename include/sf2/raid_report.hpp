

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sf2/bytes.hpp"
#include "sf2/gamedata.hpp"
#include "sf2/player.hpp"

namespace sf2 {

struct RoundHit {
    int64_t seq = 0;
    int64_t move_id = 0;
    bool landed = true;
    bool crit = false;
    bool finisher = false;
    double damage = 0;
    std::string tag;
    bool has_damage = false;
};

struct RoundReport {

    int64_t claimed = 0;
    int64_t charge_claimed = 0;

    std::vector<NamedCount> charges;
    std::vector<RoundHit> hits;

    std::vector<std::pair<uint32_t, int64_t>> extra;
    bool truncated = false;

    bool has_hits() const noexcept {
        return !hits.empty();
    }

    std::string extras() const;
};

struct RoundVerdict {
    int64_t damage = 0;
    double recomputed = 0;
    int hits_total = 0;
    int hits_dropped = 0;
    int crits = 0;
    bool verified = false;
    const char *note = nullptr;

    int no_damage = 0, unknown_move = 0, wrong_boss = 0, over_cap = 0;
    int missed = 0, hit_budget = 0, crit_budget = 0;
    std::string reasons() const;

    std::vector<std::pair<std::string, std::pair<int, double>>> by_type;
    std::string breakdown() const;

    int64_t charge_damage = 0;
};

void parse_round_report(ByteView body, RoundReport &out, size_t max_hits);

RoundVerdict verify_round(const RoundReport &report, int64_t raid_id, const GameData &gd,
                          int64_t hard_cap);

struct ChargeResolution {
    int64_t accepted = 0;
    std::vector<NamedCount> spent;
    bool unowned = false;
};

ChargeResolution resolve_charges(int64_t claimed, const std::vector<NamedCount> &used,
                                 const std::vector<NamedCount> &owned, const GameData &gd);

}
