
#include "sf2/raid_report.hpp"

#include <algorithm>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

#include "sf2/protobuf.hpp"

namespace sf2 {
namespace {

float f32_of(uint64_t bits) noexcept {
    const uint32_t u = uint32_t(bits);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

double f64_of(uint64_t bits) noexcept {
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

}

void parse_round_report(ByteView body, RoundReport &out, size_t max_hits) {
    out.claimed = 0;
    out.charge_claimed = 0;
    out.hits.clear();
    out.extra.clear();
    out.truncated = false;

    pb::scan(body, [&](const pb::Field &f) {
        if (f.number == 1 && f.wire == pb::Wire::Varint) {
            out.claimed = int64_t(f.num);
            return true;
        }

        if (f.number == 3 && f.wire == pb::Wire::Varint) {
            out.charge_claimed = int64_t(f.num);
            return true;
        }
        if (f.number == 2 && f.wire == pb::Wire::Bytes) {
            if (out.charges.size() < 16) {
                NamedCount c;
                c.name.assign(pb::get_string(f.data, 1));
                c.count = int64_t(pb::get_varint(f.data, 2, 0));
                if (!c.name.empty() && c.name.size() <= 64 && c.count > 0)
                    out.charges.push_back(std::move(c));
            }
            return true;
        }
        if (f.wire == pb::Wire::Varint && f.number != 6 && f.number != 7) {
            if (out.extra.size() < 16)
                out.extra.emplace_back(f.number, int64_t(f.num));
            return true;
        }
        if (f.number != 4 || f.wire != pb::Wire::Bytes)
            return true;
        if (out.hits.size() >= max_hits) {
            out.truncated = true;
            return false;
        }

        RoundHit h;
        pb::scan(f.data, [&](const pb::Field &g) {
            switch (g.number) {
            case 1:
                if (g.wire == pb::Wire::Varint)
                    h.seq = int64_t(g.num);
                break;
            case 2:
                if (g.wire == pb::Wire::Varint)
                    h.move_id = int64_t(g.num);
                break;
            case 3:
                if (g.wire == pb::Wire::Varint)
                    h.landed = g.num != 0;
                break;
            case 5:
                if (g.wire == pb::Wire::Varint)
                    h.crit = g.num != 0;
                break;
            case 6:
                if (g.wire == pb::Wire::Varint)
                    h.finisher = g.num != 0;
                break;

            case 7:
                if (g.wire == pb::Wire::Fixed32) {
                    h.damage = double(f32_of(g.num));
                    h.has_damage = true;
                } else if (g.wire == pb::Wire::Fixed64) {
                    h.damage = f64_of(g.num);
                    h.has_damage = true;
                } else if (g.wire == pb::Wire::Varint) {
                    h.damage = double(g.num);
                    h.has_damage = true;
                }
                break;
            case 8:
                if (g.wire == pb::Wire::Bytes && g.data.size() <= 32)
                    h.tag.assign(g.data.sv());
                break;
            default:
                break;
            }
            return true;
        });
        out.hits.push_back(std::move(h));
        return true;
    });
}

std::string RoundVerdict::reasons() const {
    std::string out;
    auto add = [&](const char *what, int n) {
        if (!n)
            return;
        if (!out.empty())
            out += ", ";
        out += std::to_string(n);
        out += ' ';
        out += what;
    };
    add("no damage", no_damage);
    add("unknown move", unknown_move);
    add("wrong boss", wrong_boss);
    add("over per-hit cap", over_cap);
    add("marked as a miss", missed);
    add("past hit budget", hit_budget);
    add("past crit budget", crit_budget);
    return out.empty() ? std::string("none") : out;
}

std::string RoundReport::extras() const {
    if (extra.empty())
        return "none";
    std::string out;
    for (const auto &e : extra) {
        if (!out.empty())
            out += ", ";
        out += "f" + std::to_string(e.first) + "=" + std::to_string(e.second);
    }
    return out;
}

ChargeResolution resolve_charges(int64_t claimed, const std::vector<NamedCount> &used,
                                 const std::vector<NamedCount> &owned, const GameData &gd) {
    ChargeResolution r;
    if (claimed <= 0 || used.empty())
        return r;

    int64_t budget = 0;
    for (const NamedCount &u : used) {
        if (u.count <= 0)
            continue;
        const SphereDef *def = gd.sphere(u.name);
        if (!def) {
            r.unowned = true;
            continue;
        }

        int64_t have = 0;
        for (const NamedCount &o : owned)
            if (o.name == u.name) {
                have = o.count;
                break;
            }
        const int64_t take = std::min(u.count, have);
        if (take < u.count)
            r.unowned = true;
        if (take <= 0)
            continue;

        budget += take * def->max_damage();
        r.spent.push_back(NamedCount{u.name, take});
    }
    r.accepted = std::min(claimed, budget);
    return r;
}

std::string RoundVerdict::breakdown() const {
    if (by_type.empty())
        return "no typed hits";
    std::string out;
    for (const auto &e : by_type) {
        if (!out.empty())
            out += ", ";
        out += e.first;
        out += " x" + std::to_string(e.second.first);
        out += " (" + std::to_string(int64_t(e.second.second)) + ")";
    }
    return out;
}

RoundVerdict verify_round(const RoundReport &report, int64_t raid_id, const GameData &gd,
                          int64_t hard_cap) {
    RoundVerdict v;
    v.hits_total = int(report.hits.size());

    int64_t claimed = report.claimed;
    if (claimed < 0)
        claimed = 0;

    if (!report.has_hits()) {
        v.damage = std::min<int64_t>(claimed, hard_cap);
        v.note = "no per-hit report";
        return v;
    }

    const bool have_moves = gd.have_moves();
    const FightRules &rules = gd.fight();

    const int64_t crit_budget =
        int64_t(std::floor(rules.crit_probability * double(report.hits.size()))) +
        rules.crit_additional;

    std::unordered_map<std::string, int64_t> used;
    double total = 0;
    int64_t crits = 0;

    for (const RoundHit &h : report.hits) {
        auto drop = [&](int &why) {
            ++v.hits_dropped;
            ++why;
        };

        if (!h.landed) {
            drop(v.missed);
            continue;
        }
        if (!(h.damage > 0) || !std::isfinite(h.damage)) {
            drop(v.no_damage);
            continue;
        }
        if (h.damage > rules.damage_factor_max) {
            drop(v.over_cap);
            continue;
        }

        if (have_moves) {
            const MoveDef *m = gd.move(h.move_id);
            if (!m || !m->enabled) {
                drop(v.unknown_move);
                continue;
            }

            if (!m->raids.empty() && !m->usable_in(raid_id)) {
                drop(v.wrong_boss);
                continue;
            }
            if (!m->damage_type.empty()) {
                const int64_t budget = rules.max_uses(m->damage_type);
                int64_t &n = used[m->damage_type];
                if (budget >= 0 && n >= budget) {
                    drop(v.hit_budget);
                    continue;
                }
                ++n;
            }
        }

        if (h.crit) {
            if (crits >= crit_budget) {
                drop(v.crit_budget);
                continue;
            }
            ++crits;
        }
        total += h.damage;

        std::string type = "unknown";
        if (have_moves)
            if (const MoveDef *m = gd.move(h.move_id))
                if (!m->damage_type.empty())
                    type = m->damage_type;
        bool seen = false;
        for (auto &e : v.by_type)
            if (e.first == type) {
                ++e.second.first;
                e.second.second += h.damage;
                seen = true;
                break;
            }
        if (!seen)
            v.by_type.emplace_back(type, std::make_pair(1, h.damage));
    }

    v.crits = int(crits);
    v.recomputed = total;
    v.verified = true;

    int64_t accepted = int64_t(std::llround(total));
    if (accepted < 0)
        accepted = 0;
    if (claimed < accepted) {
        accepted = claimed;
        v.note = "claim below report";
    } else if (claimed > accepted && v.hits_dropped)
        v.note = "hits rejected";
    else if (claimed > accepted)
        v.note = "claim above report";

    if (accepted > hard_cap) {
        accepted = hard_cap;
        v.note = "per-round ceiling";
    }
    v.damage = accepted;
    return v;
}

}
