

#include "sf2/player.hpp"

#include "sf2/protobuf.hpp"
#include "sf2/protocol_data.hpp"
#include "sf2/zlibx.hpp"

namespace sf2::proto {

ByteView inventory_deflated(const Player &p);

namespace {

inline uint64_t vi(int64_t v) noexcept {
    return uint64_t(v);
}

void id_block(Buffer &b, const Player &p, int64_t rank_a, int64_t rank_b) {
    pb::v(b, 1, vi(p.id));
    {

        pb::tag(b, 2, 2);
        const size_t n = p.name.size() + 1 + p.tag.size();
        pb::varint(b, n);
        b.str(p.name);
        b.u8('#');
        b.str(p.tag);
    }
    pb::str(b, 3, p.name);
    pb::v(b, 4, vi(rank_a));
    pb::v(b, 5, vi(rank_b));

    if (p.clan_id > 0) {
        pb::Sub clan(b, 6);
        pb::v(b, 1, vi(p.clan_id));
    }
}

void medals_block(Buffer &b, const Player &p) {
    for (const Medal &m : p.medals) {
        pb::Sub medal(b, 7);
        pb::v(b, 1, vi(m.season));
        {
            pb::Sub ts(b, 2);
            pb::v(b, 1, vi(m.earned_ms));
        }
        pb::v(b, 3, vi(m.place));
    }
}

void progress_block(Buffer &b, const Progress &g) {
    pb::v(b, 1, vi(g.level));
    pb::v(b, 2, vi(g.xp));
    pb::v(b, 3, vi(g.world));
    pb::v(b, 4, vi(g.energy));
    pb::v(b, 5, vi(g.rating));
    pb::v(b, 6, vi(g.coins));
}

}

ByteView inventory_deflated(const Player &p) {
    if (p.inventory_z.empty()) {
        static thread_local Buffer plain;
        plain.clear();
        plain.reserve(p.inventory_json.size() + 3);
        plain.u8(0xEF);
        plain.u8(0xBB);
        plain.u8(0xBF);
        plain.str(p.inventory_json);

        static thread_local Buffer packed;
        packed.clear();
        zx::deflate_to(packed, plain.view());
        p.inventory_z.assign(packed.data(), packed.data() + packed.size());
    }
    return ByteView(p.inventory_z);
}

namespace {

void quest_block(Buffer &b, const Quest &q) {
    pb::v(b, 1, vi(q.qid));
    {
        pb::Sub s(b, 2);
        pb::v(b, 1, vi(q.ts_start));
    }
    if (q.ts_end) {
        pb::Sub s(b, 3);
        pb::v(b, 1, vi(q.ts_end));
    }
    pb::v(b, 4, vi(q.cur));
    pb::v(b, 5, vi(q.max));
    {
        pb::Sub desc(b, 6);
        pb::str(b, 1, q.name);
        {
            pb::Sub r(b, 2);
            pb::str(b, 1, q.req.name);
            pb::v(b, 2, vi(q.req.count));
        }
        {
            pb::Sub r(b, 3);
            pb::str(b, 1, q.reward.name);
            pb::v(b, 2, vi(q.reward.count));
        }
        pb::v(b, 5, vi(q.amount));
    }
}

void player_info(Buffer &b, const Player &p) {
    {
        pb::Sub basic(b, 1);
        {
            pb::Sub id(b, 1);
            id_block(b, p, p.rank, p.rank);
        }
        {
            pb::Sub pg(b, 2);
            progress_block(b, p.progress);
            medals_block(b, p);
        }
        pb::bin(b, 3, inventory_deflated(p));
        pb::v(b, 4, vi(p.currency2));
    }
    pb::str(b, 2, "");
    pb::v(b, 3, vi(p.gems));

    for (const NamedCount &it : p.items) {
        pb::Sub s(b, 4);
        pb::str(b, 1, it.name);
        pb::v(b, 2, vi(it.count));
    }

    pb::v(b, 6, vi(p.ts1));
    pb::v(b, 7, vi(p.ts2));

    for (const NamedCount &pk : p.perks) {
        pb::Sub s(b, 8);
        pb::str(b, 1, pk.name);
        pb::v(b, 2, vi(pk.count));
    }

    pb::v(b, 9, 50);

    if (!p.daily_payload.empty())
        pb::bin(b, 10, ByteView(p.daily_payload));
    else {
        pb::Sub quests(b, 10);
        pb::v(b, 1, vi(p.season_id));
        for (const Quest &q : p.quests) {
            pb::Sub s(b, 2);
            quest_block(b, q);
        }
    }

    for (const std::string &g : p.gifts)
        pb::str(b, 11, g);

    for (const auto &mute : p.mutes) {
        pb::Sub s(b, 12);
        pb::v(b, 1, uint64_t(mute.first));
        pb::v(b, 2, uint64_t(mute.second));
    }
    {
        pb::Sub s(b, 13);
        pb::str(b, 1, p.bp_name);
        if (p.bp_claimed)
            pb::v(b, 2, p.bp_claimed);
    }
}

}

void build_player_info(Buffer &out, const Player &p) {
    player_info(out, p);
}

namespace {

void match_history(Buffer &b, const Player &p) {
    {
        pb::Sub head(b, 1);
        pb::v(b, 1, vi(p.season_id));
        {
            pb::Sub ts(b, 2);
            pb::v(b, 1, 0);
        }
    }
    pb::v(b, 2, vi(p.ts1));
    for (const Opponent &o : p.opponents) {
        pb::Sub s(b, 3);
        pb::v(b, 1, vi(o.pid));
        pb::v(b, 2, vi(o.rank));
        if (o.has_rating)
            pb::v(b, 3, vi(o.rating));
        if (o.our_rank)
            pb::v(b, 4, vi(o.our_rank));
    }
}

}

void build_player(Buffer &out, const Player &p, int64_t now_ms) {
    {
        pb::Sub f1(out, 1);
        player_info(out, p);
    }
    {
        pb::Sub f3(out, 3);
        pb::str(out, 1, p.login_uuid);
        pb::v(out, 2, 1);
    }
    {
        pb::Sub f4(out, 4);
        pb::str(out, 1, p.email);
        pb::v(out, 2, 3);
    }
    {
        pb::Sub f5(out, 5);
        match_history(out, p);
    }
    {
        pb::Sub f6(out, 6);
        pb::v(out, 1, vi(now_ms));
    }
    pb::bin(out, 7, protocol_data::anticheat_blob());
    pb::v(out, 8, vi(p.season_rank));
}

void build_public_player(Buffer &out, const Player &p, int64_t presence) {
    {
        pb::Sub f1(out, 1);
        id_block(out, p, p.rank, presence);
    }
    {
        pb::Sub f2(out, 2);
        pb::v(out, 1, vi(p.kills));
        pb::v(out, 2, vi(p.wins));
        pb::v(out, 3, vi(p.losses));
        pb::v(out, 4, vi(p.draws));
        pb::v(out, 5, vi(p.streak));
        pb::v(out, 6, vi(p.season_score));
        medals_block(out, p);
    }
    pb::bin(out, 3, inventory_deflated(p));
    pb::v(out, 4, vi(p.season_score));
    pb::v(out, 5, vi(p.progress.world));
    pb::v(out, 6, vi(p.rank));
}

}
