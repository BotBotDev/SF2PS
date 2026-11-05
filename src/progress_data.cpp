#include "sf2/gamedata.hpp"
#include "sf2/xml.hpp"
#include <algorithm>
#include <ctime>

namespace sf2 {
int64_t parse_utc_ms(const std::string &value) {
    tm t{};
    char *end = strptime(value.c_str(), "%Y-%m-%dT%H:%M:%SZ", &t);
    return end && !*end ? int64_t(timegm(&t)) * 1000 : 0;
}

bool DailyRule::eligible(int64_t dan) const {
    for (const auto &c : dan_conditions) {
        const auto &op = c.first;
        const auto v = c.second;
        if (op == "EQ"                             ? dan != v
            : op == "NE"                           ? dan == v
            : op == "LT"                           ? dan >= v
            : op == "LE"                           ? dan > v
            : op == "GT"                           ? dan <= v
            : (op == "GE" || op == "EqualGreater") ? dan < v
                                                   : true)
            return false;
    }
    return true;
}

const DailyRule *DailySettings::quest(int64_t id) const {
    for (const auto &q : quests)
        if (q.id == id)
            return &q;
    return nullptr;
}

const BpPass *BpSettings::active(int64_t now) const {
    const BpPass *out = nullptr;
    for (const auto &p : passes)
        if (p.start_ms <= now && now < p.end_ms && (!out || p.start_ms > out->start_ms))
            out = &p;
    return out;
}

bool GameData::load_progress(const std::string &dir, Report &rep) {
    daily_ = {};
    bp_ = {};
    bool ok = true;
    xml::Doc doc;
    std::string err;
    if (doc.load(dir + "/DailyQuests.xml", err, xml::Limits{})) {
        if (const auto *root = doc.root()) {
            daily_.renew_ms = xml::Doc::attr_i(*root, "RenewPeriod", 0) * 1000;
            daily_.replace_ms = xml::Doc::attr_i(*root, "NewQuestAfterFinishedPeriod", 0) * 1000;
            daily_.cd_unit_s = xml::Doc::attr_i(*root, "CdMinTime", 0);
            daily_.cd_cost = xml::Doc::attr_i(*root, "CdCost", 0);
            for (const auto *n : doc.children(*root, "Quest")) {
                DailyRule q;
                q.id = xml::Doc::attr_i(*n, "Id", 0);
                q.group = std::string(xml::Doc::attr(*n, "Group"));
                const auto *t = doc.child(*n, "Trigger");
                const auto *reward = doc.child(*n, "Reward");
                if (!t || !reward)
                    continue;
                q.trigger = std::string(xml::Doc::attr(*t, "Type"));
                q.target = xml::Doc::attr_i(*t, "MaxProgress", 0);
                q.raid = xml::Doc::attr_i(*t, "RaidID", -1);
                q.length =
                    xml::Doc::attr_i(*t, q.trigger == "RaidPlacePerSeason" ? "Place" : "Length",
                                     q.trigger == "RaidPlacePerSeason" ? 1 : 0);
                q.style = xml::Doc::attr_i(*t, "Style", 0);
                q.damage_type = std::string(xml::Doc::attr(*t, "DamageType"));
                q.reward = std::string(xml::Doc::attr(*reward, "LootContainerName"));
                if (const auto *conditions = doc.child(*n, "Conditions"))
                    for (const auto *c : doc.children(*conditions, "Condition")) {
                        if (xml::Doc::attr(*c, "Name") != "Dan")
                            q.dan_conditions.push_back({"unsupported", 0});
                        else
                            q.dan_conditions.emplace_back(std::string(xml::Doc::attr(*c, "Type")),
                                                          xml::Doc::attr_i(*c, "Value", 0));
                    }
                if (q.id > 0 && q.target > 0 && has_container(q.reward))
                    daily_.quests.push_back(std::move(q));
            }
        }
    } else {
        rep.problems.push_back(err);
        ok = false;
    }
    if (doc.load(dir + "/BattlePass.xml", err, xml::Limits{})) {
        if (const auto *root = doc.root()) {
            bp_.points = std::string(xml::Doc::attr(*root, "BattlePassPointsCurrency"));
            bp_.premium = std::string(xml::Doc::attr(*root, "PremiumFlagCurrency"));
            bp_.premium_pack = std::string(xml::Doc::attr(*root, "PremiumPackName"));
            for (const auto *n : doc.children(*root, "BattlePass")) {
                BpPass p;
                p.name = std::string(xml::Doc::attr(*n, "Name"));
                p.start_ms = parse_utc_ms(std::string(xml::Doc::attr(*n, "StartTime")));
                p.end_ms = parse_utc_ms(std::string(xml::Doc::attr(*n, "EndTime")));
                for (const auto *level : doc.children(*n, "Level")) {
                    BpLevel l;
                    l.number = xml::Doc::attr_i(*level, "Number", 0);
                    l.points = xml::Doc::attr_i(*level, "Points", 0);
                    l.premium = xml::Doc::attr(*level, "Premium") == "true";
                    l.loot = std::string(xml::Doc::attr(*level, "Loot"));
                    l.replacement = std::string(xml::Doc::attr(*level, "Replacement"));
                    p.levels.push_back(std::move(l));
                }
                std::sort(p.levels.begin(), p.levels.end(),
                          [](const auto &a, const auto &b) { return a.number < b.number; });
                if (!p.name.empty() && p.end_ms > p.start_ms && p.levels.size() <= 64)
                    bp_.passes.push_back(std::move(p));
            }
        }
    } else {
        rep.problems.push_back(err);
        ok = false;
    }
    return ok;
}
}
