

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unordered_map>

#include "sf2/config.hpp"
#include "sf2/player.hpp"
#include "sf2/progress.hpp"
#include "sf2/social.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace sf2 {

struct ModerationState {
    int64_t banned_until = 0, kick_seq = 0;
    int kick_kind = 3;
    std::string reason;
};

struct LeaderEntry {
    int64_t player_id = 0;
    int64_t score = 0;
};

struct RaidRecord {
    int64_t raid_id = 0;
    int64_t player_id = 0;
    int64_t damage = 0;
    int64_t rounds = 0;
    int64_t place = 0;
    int64_t rating = 0;
    std::string chest;
    int64_t claimed_at = 0;
};

bool db_bootstrap(const Config &cfg, std::string &err);

class Db {
  public:
    explicit Db(const Config &cfg);
    ~Db();
    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;

    bool open(std::string &err);
    std::optional<int64_t> gem_balance(int64_t player_id);

    std::optional<Player> by_uuid(std::string_view login_uuid);
    std::optional<Player> by_id(int64_t id);

    std::optional<Player> create(std::string_view login_uuid, std::string_view email,
                                 std::string_view password, const Config &cfg);

    int pay_raid_round(int64_t id, int64_t price, const std::vector<std::string> &potions,
                       Player &out);
    void load_player_progress(Player &p);
    bool refresh_daily(int64_t id, int64_t now, int64_t renew_id = 0, bool force = false,
                       int64_t offered = 0, int *error = nullptr);
    bool record_daily(int64_t id, const DailyEvent &event, int64_t now);
    int claim_battle_pass(int64_t id, std::string_view name, int64_t index, int64_t now,
                          std::vector<struct RaidChest> &loot);
    bool apply_progress_loot(Player &p, const struct LootRoll &loot);
    std::optional<ModerationState> moderation(int64_t id);
    std::vector<std::pair<int64_t, int64_t>> mutes(int64_t id);
    std::optional<std::pair<int64_t, int64_t>> reboot_notice();
    bool set_identity(int64_t id, std::string_view name, std::string_view tag);
    bool set_inventory(int64_t id, std::string_view inventory_json);
    bool set_progress(int64_t id, const Progress &p);
    bool set_currencies(int64_t id, int64_t gems, int64_t currency2);
    std::optional<Player> buy_pack(int64_t id, std::string_view name, int64_t wall_ms,
                                   struct LootRoll &loot);
    bool set_secret(int64_t id, ByteView salt, ByteView hash, uint32_t iters);
    bool touch_login(int64_t id, int64_t now_ms);

    bool name_taken(std::string_view name, std::string_view tag);
    std::vector<LeaderEntry> season_leaderboard(int64_t season_id, int limit);

    bool set_items(int64_t id, const std::vector<NamedCount> &v);
    bool set_perks(int64_t id, const std::vector<NamedCount> &v);
    bool set_gifts(int64_t id, const std::vector<std::string> &v);

    bool add_currency2(int64_t id, int64_t delta, int64_t &now_value);
    bool add_season_score(int64_t id, int64_t delta, int64_t &now_value);
    bool set_rank(int64_t id, int64_t rank);

    std::vector<ShortPlayer> short_players(const std::vector<int64_t> &ids);
    std::optional<ShortPlayer> short_player(int64_t id);

    std::optional<std::string> get_var(int64_t id, std::string_view key);
    bool set_var(int64_t id, std::string_view key, std::string_view value, int64_t now_ms);
    int64_t var_count(int64_t id);

    bool save_raid(const RaidBattle &b, int64_t now_ms);
    bool save_raid_participant(int64_t raid_id, const RaidParticipant &p, int64_t rating,
                               int64_t now_ms);

    bool claim_raid_reward(int64_t raid_id, int64_t player_id, int64_t now_ms);

    bool add_raid_score(int64_t player_id, int64_t season_id, int64_t score, int64_t damage,
                        bool entered, bool won);
    bool add_first_place(int64_t player_id, int64_t season_id);

    bool ensure_season_row(int64_t player_id, int64_t season_id, int64_t now_ms);
    std::vector<LeaderEntry> raid_leaderboard(int64_t season_id, int limit);
    std::vector<RaidRecord> raid_history(int64_t player_id, int limit);

    struct SeasonNotice {
        int64_t season = 0, league = 0;
        std::vector<uint8_t> reward, restored;
    };

    std::vector<Medal> player_medals(int64_t player_id);
    std::vector<SeasonNotice> pending_season_notices(int64_t player_id);
    bool mark_season_notice(int64_t player_id, int64_t season, int64_t now_ms);

    struct Season {
        int64_t id = 0, ends_ms = 0, content_version = 0;
        bool closed = false;
    };

    std::optional<Season> current_season();
    std::optional<Season> season_by_version(int64_t version);

    bool advance_season(int64_t seed, int64_t now, int64_t next_end, int64_t version);
    std::vector<LeaderEntry> clan_leaderboard(int64_t season, int limit);

    std::vector<LeagueEntry> league_board(int64_t group_id, int limit);
    std::vector<int64_t> raid_seasons();

    int64_t newest_raid_season();

    struct RaidStats {
        int64_t score = 0, damage = 0, raids = 0, wins = 0;
        int64_t best_damage = 0, rounds = 0, first_places = 0;
    };

    RaidStats raid_stats(int64_t player_id, int64_t season_id);
    RaidStats raid_profile_stats(int64_t player_id);
    bool set_season(int64_t player_id, int64_t season_id);
    bool touch_seen(int64_t player_id, int64_t now_ms);
    bool snapshot_league_places(int64_t season_id, int limit);

    struct ChatMessage {
        int64_t id = 0, player_id = 0, sent_at = 0;
        std::string name, text;
    };

    bool append_chat(int64_t channel, int64_t scope, int64_t player, std::string_view name,
                     std::string_view text, int64_t sent_at);
    std::vector<ChatMessage> chat_history(int64_t channel, int64_t scope);

    std::optional<Clan> clan_by_id(int64_t clan_id, int member_limit);
    std::vector<Clan> clans_short(const std::vector<int64_t> &ids);
    std::vector<int64_t> clan_find(int limit);

    std::vector<int64_t> clan_search(std::string_view query, bool recommended, int limit);
    bool clan_name_taken(std::string_view name);
    int64_t clan_of(int64_t player_id);
    int64_t best_raid_rating(int64_t player_id);
    std::optional<int64_t> clan_member_role(int64_t clan_id, int64_t player_id);
    bool clan_recalculate_rating(int64_t clan_id, int64_t *delta = nullptr);
    bool clan_add_contribution(int64_t player_id, int64_t delta, int64_t &clan_id,
                               int64_t &member_total);
    std::vector<int64_t> clan_member_ids(int64_t clan_id, int limit);

    int64_t clan_create(std::string_view name, std::string_view emblem,
                        std::string_view description, int64_t join_type, int64_t min_rank,
                        int64_t leader, int64_t price = 0, int64_t *remaining_gems = nullptr);
    bool clan_join(int64_t clan_id, int64_t player_id, int64_t role, int64_t max_members = 40);
    bool clan_set_role(int64_t clan_id, int64_t player_id, int64_t role);
    bool clan_set_leader(int64_t clan_id, int64_t new_leader, int64_t old_leader);
    bool clan_update_profile(int64_t clan_id, std::string_view description, std::string_view emblem,
                             int64_t join_type, int64_t min_rank);
    bool clan_remove_member(int64_t clan_id, int64_t player_id);
    bool clan_delete(int64_t clan_id);
    bool spend_gems(int64_t player_id, int64_t cost, int64_t &now_gems);

    bool clan_kick_set(int64_t clan_id, int64_t player_id, int kind, int64_t until_ms);
    bool clan_kick_until(int64_t clan_id, int64_t player_id, int kind, int64_t &until_ms);

    const char *last_error() const noexcept {
        return err_.c_str();
    }

  private:
    sqlite3_stmt *prep(const char *sql, std::string &err);

    sqlite3_stmt *cached(const char *sql);
    std::optional<Player> read_row(sqlite3_stmt *st);
    bool exec_write(sqlite3_stmt *st);

    bool award_season(const Season &season);

    const Config &cfg_;
    sqlite3 *db_ = nullptr;
    std::string err_;

    struct Stmts;
    std::unique_ptr<Stmts> st_;
    std::unordered_map<std::string, sqlite3_stmt *> cache_;
};

}
