

#include "sf2/social.hpp"

#include "sf2/protobuf.hpp"

namespace sf2::proto {
namespace {
inline uint64_t vi(int64_t v) noexcept {
    return uint64_t(v);
}

void name_tag(Buffer &b, uint32_t field, const std::string &name, const std::string &tag) {
    pb::tag(b, field, 2);
    pb::varint(b, name.size() + 1 + tag.size());
    b.str(name);
    b.u8('#');
    b.str(tag);
}

void named_count(Buffer &b, uint32_t field, const NamedCount &nc) {
    pb::Sub s(b, field);
    pb::str(b, 1, nc.name);
    pb::v(b, 2, vi(nc.count));
}
}

void build_short_player(Buffer &out, const ShortPlayer &p) {
    pb::v(out, 1, vi(p.id));
    name_tag(out, 2, p.name, p.tag);
    pb::str(out, 3, p.name);
    pb::v(out, 4, vi(p.rank));
    pb::v(out, 5, vi(p.presence));
    if (p.clan_id) {
        pb::Sub s(out, 6);
        pb::v(out, 1, vi(p.clan_id));
    }
}

void build_short_players(Buffer &out, const std::vector<ShortPlayer> &v) {
    for (const ShortPlayer &p : v) {
        pb::Sub s(out, 1);
        build_short_player(out, p);
    }
}

void build_raid_boss(Buffer &out, const RaidBoss &b) {
    pb::v(out, 1, vi(b.raid_id));
    if (b.hp_left > 0)
        pb::v(out, 2, vi(b.hp_left));
    pb::v(out, 3, vi(b.hp_max));
}

void build_loot(Buffer &out, const RaidChest &c) {
    pb::str(out, 1, c.name);
    for (const NamedCount &it : c.items)
        named_count(out, 2, it);
    for (const NamedCount &sp : c.spheres)
        named_count(out, 3, sp);
    if (c.gems)
        pb::v(out, 4, vi(c.gems));
    if (c.rating)
        pb::v(out, 5, vi(c.rating));
}

void build_raid_reward(Buffer &out, const RaidReward &r) {
    {
        pb::Sub chest(out, 1);
        build_loot(out, r.chest);
    }
    pb::v(out, 2, vi(r.rating_base));
    pb::v(out, 3, vi(r.rating_bonus));
    pb::v(out, 4, vi(r.damage));
}

void build_raid_player(Buffer &out, const RaidParticipant &p) {
    pb::v(out, 1, vi(p.pid));
    if (p.damage)
        pb::v(out, 2, vi(p.damage));
    if (p.joined)
        pb::v(out, 3, 1);
    pb::v(out, 4, vi(p.state));
    if (p.reward.present) {
        pb::Sub s(out, 5);
        build_raid_reward(out, p.reward);
    }
}

void build_raid_info(Buffer &out, const RaidBattle &b) {
    pb::v(out, 1, vi(b.id));
    {
        pb::Sub s(out, 2);
        build_raid_boss(out, b.boss);
    }
    {
        pb::Sub s(out, 3);
        pb::v(out, 1, vi(b.started_ms));
    }
    pb::v(out, 4, vi(b.duration_ms));
    for (const RaidParticipant &p : b.players) {
        pb::Sub s(out, 6);
        build_raid_player(out, p);
    }
}

void build_clan_profile(Buffer &out, const Clan &c) {

    if (!c.description.empty())
        pb::str(out, 1, c.description);
    pb::str(out, 2, c.emblem);
    pb::v(out, 3, vi(c.join_type));
    pb::v(out, 4, vi(c.min_rank));
}

void build_clan_member(Buffer &out, const ClanMember &m) {
    pb::v(out, 1, vi(m.pid));
    pb::str(out, 2, m.name);
    pb::v(out, 3, vi(m.role));
    if (m.contribution)
        pb::v(out, 4, vi(m.contribution));
}

void build_clan_raid(Buffer &out, const ClanRaidInfo &r) {
    pb::v(out, 1, vi(r.id));
    pb::v(out, 2, vi(r.raid_id));
    if (!r.players.empty()) {

        pb::tag(out, 3, 2);
        Buffer inner;
        for (int64_t pid : r.players)
            pb::varint(inner, uint64_t(pid));
        pb::varint(out, inner.size());
        out.raw(inner.view());
    }
    if (r.started)
        pb::v(out, 4, 1);
}

void build_clan(Buffer &out, const Clan &c) {
    pb::v(out, 1, vi(c.id));
    pb::str(out, 2, c.name);
    pb::v(out, 3, vi(c.rating));
    pb::v(out, 4, vi(c.level));
    pb::v(out, 5, vi(c.weekly_rating));
    {
        pb::Sub s(out, 6);
        build_clan_profile(out, c);
    }
    for (const ClanMember &m : c.members) {
        pb::Sub s(out, 7);
        build_clan_member(out, m);
    }
    for (const ClanRaidInfo &r : c.raids) {
        pb::Sub s(out, 8);
        build_clan_raid(out, r);
    }
}

void build_clan_short_list(Buffer &out, const std::vector<Clan> &v) {
    for (const Clan &c : v) {
        pb::Sub s(out, 1);
        pb::v(out, 1, vi(c.id));
        pb::str(out, 2, c.name);
        pb::str(out, 3, c.emblem);
        pb::v(out, 4, vi(c.level));
        pb::v(out, 5, vi(c.member_count));
        pb::v(out, 6, vi(c.rating));
    }
}

void build_league(Buffer &out, const LeagueBoard &b) {
    {
        pb::Sub head(out, 1);
        pb::v(out, 1, vi(b.league_id));
        {
            pb::Sub ts(out, 2);
            pb::v(out, 1, vi(b.ends_ms));
        }
    }
    pb::v(out, 2, vi(b.content_version));
    for (const LeagueEntry &e : b.entries) {
        pb::Sub s(out, 3);
        pb::v(out, 1, vi(e.pid));
        pb::v(out, 2, vi(e.place));
        if (e.score)
            pb::v(out, 3, vi(e.score));
        if (e.prev_place)
            pb::v(out, 4, vi(e.prev_place));
    }
}

}
