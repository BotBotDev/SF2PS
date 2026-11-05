

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sf2/bytes.hpp"

namespace sf2 {

struct Progress {
    int64_t level = 1;
    int64_t xp = 0;
    int64_t world = 1;
    int64_t energy = 10;
    int64_t rating = 0;
    int64_t coins = 500;
};

struct NamedCount {
    std::string name;
    int64_t count = 0;
};

struct Quest {
    int64_t qid = 0;
    int64_t ts_start = 0;
    int64_t ts_end = 0;
    int64_t cur = 0;
    int64_t max = 0;
    std::string name;
    NamedCount req;
    NamedCount reward;
    int64_t amount = 0;
};

struct Opponent {
    int64_t pid = 0;
    int64_t rank = 0;
    bool has_rating = false;
    int64_t rating = 0;
    int64_t our_rank = 0;
};

struct MatchEntry {
    int64_t score_delta = 0;
    int64_t ts_ms = 0;
    int64_t opp_rank = 0;
};

struct Medal {
    int64_t season = 0, earned_ms = 0, place = 0;
};

struct Player {
    int64_t id = 0;
    std::string name;
    std::string tag;
    std::string login_uuid;
    std::string email;

    int64_t rank = 2;
    int64_t season_rank = 0;
    int64_t season_id = 0;
    int64_t season_score = 0;
    std::vector<uint8_t> daily_payload;
    std::string bp_name = "PlaceHolder";
    uint64_t bp_claimed = 0;
    std::vector<Medal> medals;
    std::vector<std::pair<int64_t, int64_t>> mutes;
    int64_t gems = 0;
    int64_t currency2 = 0;

    int64_t kills = 0, wins = 0, losses = 0, draws = 0, streak = 0;

    int64_t clan_id = 0;
    int64_t raid_score = 0;

    Progress progress;
    std::string inventory_json = R"({"v":2,"lot":0,"eq":[]})";

    mutable std::vector<uint8_t> inventory_z;

    void set_inventory(std::string json) {
        inventory_json = std::move(json);
        inventory_z.clear();
    }

    std::vector<NamedCount> items;
    std::vector<NamedCount> perks;
    std::vector<std::string> gifts;
    std::vector<Quest> quests;
    std::vector<Opponent> opponents;
    std::vector<MatchEntry> match_history;

    int64_t ts1 = 0, ts2 = 0;
    int64_t created_at = 0, updated_at = 0;

    std::vector<uint8_t> pw_salt;
    std::vector<uint8_t> pw_hash;
    uint32_t pw_iters = 0;
    bool has_secret = false;
    int64_t banned_until = 0;
};

namespace proto {

ByteView inventory_deflated(const Player &p);

void build_player(Buffer &out, const Player &p, int64_t now_ms);

void build_player_info(Buffer &out, const Player &p);

void build_public_player(Buffer &out, const Player &p, int64_t presence = 1);

}
}
