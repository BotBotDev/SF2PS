

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sf2/bytes.hpp"
#include "sf2/raid.hpp"

namespace sf2 {

struct ShortPlayer {
    int64_t id = 0;
    std::string name;
    std::string tag;
    int64_t rank = 1;
    int64_t presence = kPresenceOffline;
    int64_t clan_id = 0;
};

enum : int64_t { kClanLeader = 1, kClanOfficer = 2, kClanAdept = 3, kClanNovice = 4 };

struct ClanMember {
    int64_t pid = 0;
    std::string name;
    int64_t role = kClanAdept;
    int64_t contribution = 0;
};

struct ClanRaidInfo {
    int64_t id = 0;
    int64_t raid_id = 0;
    std::vector<int64_t> players;
    bool started = false;
};

struct Clan {
    int64_t id = 0;
    std::string name;
    int64_t rating = 0;
    int64_t level = 1;
    int64_t weekly_rating = 0;
    std::string description;
    std::string emblem = "0,0,0";
    int64_t join_type = 1;
    int64_t min_rank = 0;
    int64_t member_count = 0;
    std::vector<ClanMember> members;
    std::vector<ClanRaidInfo> raids;
};

struct LeagueEntry {
    int64_t pid = 0;
    int64_t place = 0;
    int64_t score = 0;
    int64_t prev_place = 0;
};

struct LeagueBoard {
    int64_t league_id = 0;
    int64_t ends_ms = 0;
    int64_t content_version = 0;
    std::vector<LeagueEntry> entries;
};

namespace proto {

void build_short_player(Buffer &out, const ShortPlayer &p);
void build_short_players(Buffer &out, const std::vector<ShortPlayer> &v);

void build_raid_boss(Buffer &out, const RaidBoss &b);

void build_loot(Buffer &out, const RaidChest &c);
void build_raid_player(Buffer &out, const RaidParticipant &p);
void build_raid_reward(Buffer &out, const RaidReward &r);

void build_raid_info(Buffer &out, const RaidBattle &b);

void build_clan_profile(Buffer &out, const Clan &c);
void build_clan_member(Buffer &out, const ClanMember &m);
void build_clan_raid(Buffer &out, const ClanRaidInfo &r);
void build_clan(Buffer &out, const Clan &c);
void build_clan_short_list(Buffer &out, const std::vector<Clan> &v);

void build_league(Buffer &out, const LeagueBoard &b);

}
}
