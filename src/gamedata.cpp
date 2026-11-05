
#include "sf2/gamedata.hpp"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

#include "sf2/crypto.hpp"
#include "sf2/log.hpp"
#include "sf2/xml.hpp"

namespace sf2 {
namespace {

double rand01() {
    uint64_t v = 0;
    crypto::random_bytes(reinterpret_cast<uint8_t *>(&v), sizeof v);
    return double(v >> 11) * (1.0 / 9007199254740992.0);
}

std::string join_path(const std::string &dir, const char *rel) {
    if (dir.empty())
        return rel;
    if (dir.back() == '/')
        return dir + rel;
    return dir + "/" + rel;
}

std::string parent_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string exe_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return {};
    buf[n] = 0;
    return parent_of(std::string(buf));
}

bool readable(const std::string &path) {
    return ::access(path.c_str(), R_OK) == 0;
}

constexpr int kMaxLootDepth = 24;

}

const RewardTier *RaidRewards::for_place(int64_t place) const noexcept {
    const RewardTier *best = nullptr;
    for (const RewardTier &t : tiers)
        if (place <= t.pos_to && (!best || t.pos_to < best->pos_to))
            best = &t;
    return best;
}

int64_t FightRules::max_uses(std::string_view damage_type) const noexcept {
    for (const auto &kv : hit_budgets)
        if (kv.first == damage_type)
            return kv.second;
    return -1;
}

bool MoveDef::usable_in(int64_t raid_id) const noexcept {
    return std::binary_search(raids.begin(), raids.end(), raid_id);
}

const LeagueDan *LeagueSettings::for_rating(int64_t rating) const noexcept {
    const LeagueDan *best = nullptr;
    for (const LeagueDan &d : dans)
        if (rating >= d.min_reward_rating && (!best || d.dan > best->dan))
            best = &d;
    return best;
}

double ClanLimits::weight_for_position(int64_t pos) const noexcept {
    const std::pair<int64_t, double> *best = nullptr;
    for (const auto &w : rating_weights)
        if (pos <= w.first && (!best || w.first < best->first))
            best = &w;
    return best ? best->second : 0.0;
}

std::string GameData::Report::summary() const {
    std::ostringstream o;
    o << raids << " raids, " << moves << " moves, " << containers << " loot containers, "
      << league_dans << " league tiers, " << spheres << " spheres";
    if (!problems.empty())
        o << " (" << problems.size() << " file(s) unavailable)";
    return o.str();
}

GameData &GameData::instance() {
    static GameData g;
    return g;
}

const RaidDef *GameData::raid(int64_t id) const noexcept {
    auto it = raids_.find(id);
    return it == raids_.end() ? nullptr : &it->second;
}

RaidDef *GameData::raid_mut(int64_t id) noexcept {
    auto it = raids_.find(id);
    return it == raids_.end() ? nullptr : &it->second;
}

std::vector<const RaidDef *> GameData::raids() const {
    std::vector<const RaidDef *> out;
    out.reserve(raids_.size());
    for (const auto &kv : raids_)
        out.push_back(&kv.second);
    std::sort(out.begin(), out.end(),
              [](const RaidDef *a, const RaidDef *b) { return a->id < b->id; });
    return out;
}

const MoveDef *GameData::move(int64_t id) const noexcept {
    auto it = moves_.find(id);
    return it == moves_.end() ? nullptr : &it->second;
}

bool GameData::has_container(std::string_view name) const noexcept {
    return containers_.find(std::string(name)) != containers_.end();
}

bool GameData::load_raids(const std::string &dir, Report &rep) {
    xml::Doc doc;
    std::string err;
    if (!doc.load(join_path(dir, "config.xml"), err, xml::Limits{})) {
        rep.problems.push_back(err);
        return false;
    }
    const xml::Node *root = doc.root();
    if (!root)
        return false;

    if (const xml::Node *t = doc.path(*root, "SFSSettings/Timings")) {
        auto ms = [&](const char *tag, const char *attr, int64_t &into) {
            if (const xml::Node *n = doc.child(*t, tag))
                into = xml::Doc::attr_i(*n, attr, into);
        };
        ms("FinishRaidDelta", "Value", timings_.finish_raid_delta_ms);
        ms("Matching", "Timeout", timings_.matching_timeout_ms);
        ms("JoinZone", "Timeout", timings_.join_zone_timeout_ms);
        ms("Ping", "Delay", timings_.ping_delay_ms);
        ms("UpdateClientProfile", "Value", timings_.update_client_profile_ms);
    }

    if (const xml::Node *rs = doc.child(*root, "RaidSettings")) {
        if (const xml::Node *f = doc.child(*rs, "Fight")) {
            fight_.loaded = true;
            if (const xml::Node *c = doc.child(*f, "CritVerification")) {
                fight_.crit_probability = xml::Doc::attr_d(*c, "Probability", 0.2);
                fight_.crit_additional = xml::Doc::attr_i(*c, "Additional", 5);
            }
            if (const xml::Node *d = doc.child(*f, "DamageFactor"))
                fight_.damage_factor_max = xml::Doc::attr_d(*d, "MaxValue", 20000);
            for (const xml::Node *h : doc.children(*f, "HitVerification")) {
                std::string type(xml::Doc::attr(*h, "DamageType"));
                if (type.empty())
                    continue;
                fight_.hit_budgets.emplace_back(std::move(type),
                                                xml::Doc::attr_i(*h, "MaxUses", -1));
            }
        }
    }

    const xml::Node *zones = doc.path(*root, "map/Stages/Zones");
    if (!zones) {
        rep.problems.push_back("config.xml: no map/Stages/Zones");
        return false;
    }

    for (const xml::Node *zone : doc.children(*zones, "Zone")) {
        for (const xml::Node *b : doc.children(*zone, "Battle")) {
            if (xml::Doc::attr(*b, "Type") != "RAID")
                continue;
            const xml::Node *rd = doc.child(*b, "RaidData");
            if (!rd)
                continue;

            RaidDef d;
            const auto zone_name = xml::Doc::attr(*zone, "Name");
            if (zone_name == "ZONE_RAID1")
                d.tier = 2;
            else if (zone_name == "ZONE_RAID2")
                d.tier = 3;
            else if (zone_name == "ZONE_RAID3")
                d.tier = 4;
            d.id = xml::Doc::attr_i(*rd, "RaidID", -1);
            if (d.id < 0)
                continue;

            d.name = std::string(xml::Doc::attr(*b, "Name"));
            d.alias = std::string(xml::Doc::attr(*b, "Alias"));
            d.title = std::string(xml::Doc::attr(*b, "Title"));
            d.location = std::string(xml::Doc::attr(*b, "Location"));
            d.hard = xml::Doc::attr_b(*b, "RaidMode", false);
            d.hidden = xml::Doc::attr_b(*b, "Hide", false);

            d.boss_mmr = xml::Doc::attr_i(*rd, "BossMMR", 0);
            d.party_size = xml::Doc::attr_i(*rd, "RaidSize", 4);
            d.dan_restriction = xml::Doc::attr_i(*rd, "DanRestriction", 0);
            d.final_battle_power = xml::Doc::attr_i(*rd, "FinalBattlePower", 0);
            d.raid_time_ms = xml::Doc::attr_i(*rd, "RaidTime", 300) * 1000;
            d.duration_ms = d.raid_time_ms + timings_.finish_raid_delta_ms;
            d.round_cooldown_ms = xml::Doc::attr_i(*rd, "RoundCoolDown", 100) * 1000;
            d.cd_min_time_s = xml::Doc::attr_i(*rd, "CdMinTime", 0);
            d.cd_cost = xml::Doc::attr_i(*rd, "CdCost", 0);

            for (const xml::Node *c : doc.children(*rd, "RaidCurrencyCost")) {
                RaidCost cost;
                cost.currency = std::string(xml::Doc::attr(*c, "Name"));
                cost.value = xml::Doc::attr_i(*c, "Value", 0);
                if (!cost.currency.empty() && cost.value > 0)
                    d.costs.push_back(std::move(cost));
            }

            if (const xml::Node *fight = doc.child(*b, "Fight")) {
                d.round_time_ms = xml::Doc::attr_i(*fight, "RoundTime", 45) * 1000;

                if (const xml::Node *ws = doc.child(*fight, "Warriors")) {
                    for (const xml::Node *w : doc.children(*ws, "Warrior")) {
                        d.shield_total = xml::Doc::attr_i(*w, "ShieldTotal", 0);
                        d.shield_stack = xml::Doc::attr_i(*w, "ShieldStack", 0);
                        break;
                    }
                }
                if (d.shield_total > 0)
                    d.boss_hp = d.shield_total;
                if (const xml::Node *rw = doc.child(*fight, "Rewards")) {
                    d.rewards.threshold_damage_fraction =
                        xml::Doc::attr_d(*rw, "TresholdDamageFraction", 0);
                    d.rewards.base_rating = xml::Doc::attr_i(*rw, "BaseRating", 0);
                    d.rewards.pos_rating = xml::Doc::attr_i(*rw, "PosRating", 0);
                    d.rewards.damage_rating_scaler =
                        xml::Doc::attr_d(*rw, "DamageRatingScaler", 1.0);
                    for (const xml::Node *r : doc.children(*rw, "Reward")) {
                        RewardTier t;
                        t.pos_to = xml::Doc::attr_i(*r, "PosTo", 0);
                        t.pos_rating_scaler = xml::Doc::attr_d(*r, "PosRatingScaler", 0);
                        t.container = std::string(xml::Doc::attr(*r, "LootContainerName"));
                        if (t.pos_to > 0)
                            d.rewards.tiers.push_back(std::move(t));
                    }
                    std::sort(d.rewards.tiers.begin(), d.rewards.tiers.end(),
                              [](const RewardTier &x, const RewardTier &y) {
                                  return x.pos_to < y.pos_to;
                              });
                }
            }
            raids_[d.id] = std::move(d);
        }
    }
    rep.raids = int(raids_.size());
    return true;
}

bool GameData::load_moves(const std::string &dir, Report &rep) {
    xml::Doc doc;
    std::string err;
    xml::Limits lim;
    lim.max_nodes = 500'000;
    if (!doc.load(join_path(dir, "server/ServerMoves.xml"), err, lim)) {
        rep.problems.push_back(err);
        return false;
    }
    const xml::Node *root = doc.root();
    if (!root)
        return false;

    for (const xml::Node *iv : doc.children(*root, "Interval")) {
        MoveDef m;
        m.id = xml::Doc::attr_i(*iv, "ID", -1);
        if (m.id < 0)
            continue;
        m.name = std::string(xml::Doc::attr(*iv, "MoveName"));
        m.damage_type = std::string(xml::Doc::attr(*iv, "DamageType"));
        m.enabled = xml::Doc::attr_b(*iv, "Enabled", true);
        for (const xml::Node *b : doc.children(*iv, "Boss")) {
            int64_t rid = xml::Doc::attr_i(*b, "RaidID", -1);
            if (rid >= 0)
                m.raids.push_back(rid);
        }
        std::sort(m.raids.begin(), m.raids.end());
        m.raids.erase(std::unique(m.raids.begin(), m.raids.end()), m.raids.end());
        moves_[m.id] = std::move(m);
    }
    rep.moves = int(moves_.size());
    return true;
}

bool GameData::load_loot(const std::string &dir, Report &rep) {
    xml::Doc doc;
    std::string err;
    xml::Limits lim;
    lim.max_nodes = 200'000;
    if (!doc.load(join_path(dir, "LootContainers.xml"), err, lim)) {
        rep.problems.push_back(err);
        return false;
    }
    const xml::Node *root = doc.root();
    if (!root)
        return false;

    std::function<uint32_t(const xml::Node &, int)> build = [&](const xml::Node &n,
                                                                int depth) -> uint32_t {
        const uint32_t idx = uint32_t(loot_.size());
        loot_.emplace_back();
        {
            LootNode node;
            if (n.name == "AndContainer")
                node.kind = LootKind::And;
            else if (n.name == "OrContainer")
                node.kind = LootKind::Or;
            else if (n.name == "WRContainer")
                node.kind = LootKind::WeightedRand;
            else if (n.name == "Currency")
                node.kind = LootKind::Currency;
            else if (n.name == "Consumable")
                node.kind = LootKind::Consumable;
            else if (n.name == "Gems")
                node.kind = LootKind::Gems;
            else if (n.name == "Rating")
                node.kind = LootKind::Rating;
            else
                node.kind = LootKind::And;

            node.name = std::string(xml::Doc::attr(n, "Name"));
            node.value = xml::Doc::attr_i(n, "Value", 0);
            node.weight = xml::Doc::attr_d(n, "Weight", 1.0);
            node.probability = xml::Doc::attr_d(n, "Probability", 1.0);
            node.gift = xml::Doc::attr(n, "Gift") == "true";
            if (node.weight < 0)
                node.weight = 0;
            loot_[idx] = std::move(node);
        }
        if (depth < kMaxLootDepth) {
            for (const xml::Node *k : doc.children(n)) {
                const uint32_t kid = build(*k, depth + 1);
                loot_[idx].kids.push_back(kid);
            }
        }
        return idx;
    };

    for (const xml::Node *c : doc.children(*root)) {
        std::string_view cname = xml::Doc::attr(*c, "CName");
        if (cname.empty())
            continue;
        const uint32_t idx = build(*c, 0);
        containers_[std::string(cname)] = idx;
    }
    rep.containers = int(containers_.size());
    return true;
}

void GameData::roll_node(uint32_t idx, LootRoll &out, int depth) const {
    if (idx >= loot_.size() || depth > kMaxLootDepth)
        return;
    const LootNode &n = loot_[idx];

    auto add = [](std::vector<NamedCount> &into, const std::string &name, int64_t v) {
        if (name.empty() || v == 0)
            return;
        for (NamedCount &e : into)
            if (e.name == name) {
                e.count += v;
                return;
            }
        if (into.size() < 64)
            into.push_back(NamedCount{name, v});
    };

    switch (n.kind) {
    case LootKind::Currency:
        if (n.probability >= 1.0 || rand01() < n.probability)
            add(out.currency, n.name, n.value);
        return;
    case LootKind::Consumable:
        if (n.probability >= 1.0 || rand01() < n.probability)
            add(out.consumables, n.name, n.value);
        return;
    case LootKind::Gems:
        if (n.probability >= 1.0 || rand01() < n.probability)
            out.gems += n.value;
        return;
    case LootKind::Rating:
        if (n.probability >= 1.0 || rand01() < n.probability)
            out.rating += n.value;
        return;

    case LootKind::And:

        if (n.probability < 1.0 && rand01() >= n.probability)
            return;
        for (uint32_t k : n.kids)
            roll_node(k, out, depth + 1);
        return;

    case LootKind::Or: {
        if (n.probability < 1.0 && rand01() >= n.probability)
            return;
        double total = 0;
        for (uint32_t k : n.kids)
            total += loot_[k].weight;
        if (total <= 0)
            return;
        double pick = rand01() * total;
        for (uint32_t k : n.kids) {
            pick -= loot_[k].weight;
            if (pick < 0) {
                roll_node(k, out, depth + 1);
                return;
            }
        }
        roll_node(n.kids.back(), out, depth + 1);
        return;
    }

    case LootKind::WeightedRand: {
        if (n.kids.empty())
            return;
        const size_t pick = size_t(rand01() * double(n.kids.size()));
        roll_node(n.kids[std::min(pick, n.kids.size() - 1)], out, depth + 1);
        return;
    }
    }
}

bool GameData::gift_container(std::string_view name) const {
    auto it = containers_.find(std::string(name));
    if (it == containers_.end() || it->second >= loot_.size())
        return false;
    return loot_[it->second].gift;
}

bool GameData::roll(std::string_view name, LootRoll &out) const {
    auto it = containers_.find(std::string(name));
    if (it == containers_.end())
        return false;
    roll_node(it->second, out, 0);
    return true;
}

bool GameData::load_spheres(const std::string &dir, Report &rep) {
    xml::Doc doc;
    std::string err;
    xml::Limits lim;
    lim.max_nodes = 100'000;
    if (!doc.load(join_path(dir, "RaidItems.xml"), err, lim)) {
        rep.problems.push_back(err);
        return false;
    }
    const xml::Node *root = doc.root();
    if (!root)
        return false;
    if (const auto *packs = doc.child(*root, "Packs")) {
        for (const auto *n : doc.children(*packs, "Pack")) {
            ShopPack p;
            p.name = std::string(xml::Doc::attr(*n, "Name"));
            p.container = std::string(xml::Doc::attr(*n, "LootContainerName"));
            p.price = xml::Doc::attr_i(*n, "GemsPrice", -1);
            p.target_dan = xml::Doc::attr_i(*n, "TargetDan", 0);
            p.min_level = xml::Doc::attr_i(*n, "LevelRestrictionMin", 0);
            p.max_level = xml::Doc::attr_i(*n, "LevelRestrictionMax", 0);
            p.purchasable = xml::Doc::attr(*n, "Purchasable") == "true";
            p.start = std::string(xml::Doc::attr(*n, "StartTime"));
            p.end = std::string(xml::Doc::attr(*n, "EndTime"));
            if (!p.name.empty())
                packs_[p.name] = std::move(p);
        }
    }
    const xml::Node *items = doc.child(*root, "Items");
    if (!items)
        return false;

    for (const xml::Node *c : doc.children(*items, "Consumable")) {
        if (xml::Doc::attr(*c, "Type") == "POTION")
            potions_.push_back(std::string(xml::Doc::attr(*c, "Name")));
        if (!xml::Doc::has_attr(*c, "RaidChargeDamage"))
            continue;
        SphereDef s;
        s.name = std::string(xml::Doc::attr(*c, "Name"));
        s.charge_damage = xml::Doc::attr_i(*c, "RaidChargeDamage", 0);
        s.dispersion = xml::Doc::attr_d(*c, "DamageDispersionPct", 0);
        if (s.name.empty() || s.charge_damage <= 0)
            continue;
        spheres_.push_back(std::move(s));
    }
    std::sort(spheres_.begin(), spheres_.end(), [](const SphereDef &a, const SphereDef &b) {
        return a.charge_damage > b.charge_damage;
    });
    rep.spheres = int(spheres_.size());
    return true;
}

bool GameData::potion(std::string_view name) const {
    return std::find(potions_.begin(), potions_.end(), name) != potions_.end();
}

const ShopPack *GameData::pack(std::string_view name) const {
    auto it = packs_.find(std::string(name));
    return it == packs_.end() ? nullptr : &it->second;
}

const SphereDef *GameData::sphere(std::string_view name) const noexcept {
    for (const SphereDef &s : spheres_)
        if (s.name == name)
            return &s;
    return nullptr;
}

bool GameData::load_league(const std::string &dir, Report &rep) {
    xml::Doc doc;
    std::string err;
    if (!doc.load(join_path(dir, "Leagues.xml"), err, xml::Limits{})) {
        rep.problems.push_back(err);
        return false;
    }
    const xml::Node *root = doc.root();
    if (!root)
        return false;

    league_.loaded = true;
    league_.update_interval_ms = xml::Doc::attr_i(*root, "LeaderboardUpdateInterval", 600000);
    league_.max_rows = xml::Doc::attr_i(*root, "LeaderboardMaxRows", 100);
    league_.restore_currency =
        std::string(xml::Doc::attr(*root, "RestoreCurrencyNameAtSeasonStart"));
    league_.restore_value = xml::Doc::attr_i(*root, "RestoreCurrencyValueAtSeasonStart", 0);
    if (const xml::Node *sc = doc.child(*root, "Schedule"))
        league_.schedule_cron = std::string(xml::Doc::attr(*sc, "CronSchedule"));
    if (const xml::Node *dc = doc.child(*root, "DaySchedule"))
        league_.day_schedule_cron = std::string(xml::Doc::attr(*dc, "CronSchedule"));

    if (const xml::Node *board = doc.child(*root, "Leaderboard"))
        for (const xml::Node *rating : doc.children(*board, "TopRating"))
            league_.medal_positions.push_back(xml::Doc::attr_i(*rating, "PosTo", 0));

    for (const xml::Node *l : doc.children(*root, "League")) {
        LeagueDan d;
        d.dan = xml::Doc::attr_i(*l, "Dan", 0);
        d.min_size = xml::Doc::attr_i(*l, "MinSize", 0);
        d.max_size = xml::Doc::attr_i(*l, "MaxSize", 0);
        d.min_reward_rating = xml::Doc::attr_i(*l, "MinRewardRating", 0);
        if (const xml::Node *rws = doc.child(*l, "Rewards")) {
            for (const xml::Node *r : doc.children(*rws, "Reward")) {
                LeagueReward lr;
                lr.pos_to = xml::Doc::attr_i(*r, "PosTo", 0);
                lr.container = std::string(xml::Doc::attr(*r, "LootContainerName"));
                lr.dan_up = xml::Doc::attr_b(*r, "DanUp", false);
                d.rewards.push_back(std::move(lr));
            }
        }
        league_.dans.push_back(std::move(d));
    }
    rep.league_dans = int(league_.dans.size());
    return true;
}

bool GameData::load_clan(const std::string &dir, Report &rep) {
    xml::Doc doc;
    std::string err;
    if (!doc.load(join_path(dir, "ClanSettings.xml"), err, xml::Limits{})) {
        rep.problems.push_back(err);
        return false;
    }
    const xml::Node *root = doc.root();
    if (!root)
        return false;
    clan_.loaded = true;

    auto val = [&](const char *tag, const char *attr, int64_t dflt) {
        const xml::Node *n = doc.child(*root, tag);
        return n ? xml::Doc::attr_i(*n, attr, dflt) : dflt;
    };
    clan_.create_price_gems = val("ClanCreatePrice", "GemsValue", 349);
    clan_.name_max = val("ClanName", "MaxSymbols", 16);
    clan_.desc_max = val("ClanDescription", "MaxSymbols", 200);
    clan_.chat_max = val("ClanChat", "MaxSymbols", 256);
    clan_.kick_cooldown_ms = val("ClanKickCoolDown", "Value", 86400000);
    clan_.raid_kick_cooldown_ms = val("RaidKickCoolDown", "Value", 600000);

    if (const xml::Node *icons = doc.child(*root, "ClanIcons")) {
        if (const xml::Node *n = doc.child(*icons, "Icons"))
            clan_.icon_count = int64_t(doc.children(*n, "image").size());
        if (const xml::Node *n = doc.child(*icons, "Backs"))
            clan_.back_count = int64_t(doc.children(*n, "image").size());
        if (const xml::Node *n = doc.child(*icons, "Colors"))
            clan_.color_count = int64_t(doc.children(*n, "Preset").size());
    }
    if (const xml::Node *permissions = doc.child(*root, "Permissions"))
        for (const xml::Node *p : doc.children(*permissions, "Permission"))
            clan_.permissions.emplace_back(std::string(xml::Doc::attr(*p, "Name")),
                                           xml::Doc::attr_i(*p, "Rank", 40));
    if (const xml::Node *levels = doc.child(*root, "ClanLevels")) {
        clan_.max_size = 0;
        for (const xml::Node *l : doc.children(*levels, "Level"))
            clan_.max_size = std::max(clan_.max_size, xml::Doc::attr_i(*l, "ClanMaxSize", 40));
    }
    if (clan_.max_size <= 0)
        clan_.max_size = 40;
    if (const xml::Node *cr = doc.child(*root, "ClanRating")) {
        for (const xml::Node *w : doc.children(*cr, "RatingWeight"))
            clan_.rating_weights.emplace_back(xml::Doc::attr_i(*w, "PosTo", 0),
                                              xml::Doc::attr_d(*w, "Value", 0));
        std::sort(clan_.rating_weights.begin(), clan_.rating_weights.end());
    }
    if (const xml::Node *roles = doc.child(*root, "ClanRoles")) {
        for (const xml::Node *r : doc.children(*roles, "Role"))
            clan_.roles.emplace_back(std::string(xml::Doc::attr(*r, "Name")),
                                     xml::Doc::attr_i(*r, "Rank", 0));
        std::sort(clan_.roles.begin(), clan_.roles.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });
    }
    return true;
}

std::vector<std::string> GameData::search_roots(const std::string &dir,
                                                const std::string &config_path) {
    std::vector<std::string> out;
    if (dir.empty())
        return out;
    if (dir.front() == '/') {
        out.push_back(dir);
        return out;
    }

    out.push_back(dir);
    if (!config_path.empty())
        out.push_back(join_path(parent_of(config_path), dir.c_str()));

    const std::string exe = exe_dir();
    if (!exe.empty()) {
        out.push_back(join_path(exe, dir.c_str()));
        out.push_back(join_path(parent_of(exe), dir.c_str()));

        out.push_back(join_path(join_path(parent_of(exe), "src"), dir.c_str()));
        out.push_back(join_path(join_path(parent_of(exe), "share/sf2server"), dir.c_str()));
    }
    out.push_back(join_path("/usr/local/share/sf2server", dir.c_str()));
    out.push_back(join_path("/usr/share/sf2server", dir.c_str()));

    std::vector<std::string> uniq;
    for (const std::string &p : out)
        if (std::find(uniq.begin(), uniq.end(), p) == uniq.end())
            uniq.push_back(p);
    return uniq;
}

GameData::Report GameData::load(const std::string &dir, const std::string &config_path) {
    Report rep;
    raids_.clear();
    moves_.clear();
    loot_.clear();
    containers_.clear();
    spheres_.clear();
    packs_.clear();
    potions_.clear();

    std::string root;
    for (const std::string &candidate : search_roots(dir, config_path)) {
        rep.searched.push_back(candidate);
        if (readable(join_path(candidate, "config.xml"))) {
            root = candidate;
            break;
        }
    }
    if (root.empty())
        return rep;

    rep.resolved = root;
    rep.searched.clear();
    const bool raids = load_raids(root, rep);
    const bool loot = load_loot(root, rep);
    load_moves(root, rep);
    load_league(root, rep);
    load_clan(root, rep);
    load_spheres(root, rep);
    load_progress(root, rep);

    rep.ok = raids && loot;
    return rep;
}

}
