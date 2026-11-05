
#pragma once

namespace sf2 {

inline constexpr const char *kProfileStatsSchema = R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS raid_profile_stats (
 player_id INTEGER PRIMARY KEY,
 raids INTEGER NOT NULL DEFAULT 0, wins INTEGER NOT NULL DEFAULT 0,
 rounds INTEGER NOT NULL DEFAULT 0, first_places INTEGER NOT NULL DEFAULT 0,
 best_rating INTEGER NOT NULL DEFAULT 0
);
INSERT INTO raid_profile_stats(player_id,raids,wins,rounds,first_places,best_rating)
SELECT player_id,SUM(raids),SUM(wins),SUM(rounds),SUM(first_places),MAX(score)
FROM raid_scores GROUP BY player_id
ON CONFLICT(player_id) DO UPDATE SET
raids=excluded.raids,wins=excluded.wins,rounds=excluded.rounds,
first_places=excluded.first_places,best_rating=excluded.best_rating;
CREATE TRIGGER IF NOT EXISTS raid_profile_stats_insert AFTER INSERT ON raid_scores BEGIN
INSERT INTO raid_profile_stats(player_id,raids,wins,rounds,first_places,best_rating)
SELECT NEW.player_id,COALESCE(SUM(raids),0),COALESCE(SUM(wins),0),COALESCE(SUM(rounds),0),
COALESCE(SUM(first_places),0),COALESCE(MAX(score),0) FROM raid_scores WHERE player_id=NEW.player_id
ON CONFLICT(player_id) DO UPDATE SET
raids=excluded.raids,wins=excluded.wins,rounds=excluded.rounds,
first_places=excluded.first_places,best_rating=excluded.best_rating;
END;
CREATE TRIGGER IF NOT EXISTS raid_profile_stats_update AFTER UPDATE OF player_id,score,raids,wins,rounds,first_places ON raid_scores BEGIN
INSERT INTO raid_profile_stats(player_id,raids,wins,rounds,first_places,best_rating)
SELECT OLD.player_id,COALESCE(SUM(raids),0),COALESCE(SUM(wins),0),COALESCE(SUM(rounds),0),
COALESCE(SUM(first_places),0),COALESCE(MAX(score),0) FROM raid_scores WHERE player_id=OLD.player_id
ON CONFLICT(player_id) DO UPDATE SET
raids=excluded.raids,wins=excluded.wins,rounds=excluded.rounds,
first_places=excluded.first_places,best_rating=excluded.best_rating;
INSERT INTO raid_profile_stats(player_id,raids,wins,rounds,first_places,best_rating)
SELECT NEW.player_id,COALESCE(SUM(raids),0),COALESCE(SUM(wins),0),COALESCE(SUM(rounds),0),
COALESCE(SUM(first_places),0),COALESCE(MAX(score),0) FROM raid_scores WHERE player_id=NEW.player_id
ON CONFLICT(player_id) DO UPDATE SET
raids=excluded.raids,wins=excluded.wins,rounds=excluded.rounds,
first_places=excluded.first_places,best_rating=excluded.best_rating;
END;
CREATE TRIGGER IF NOT EXISTS raid_profile_stats_delete AFTER DELETE ON raid_scores BEGIN
INSERT INTO raid_profile_stats(player_id,raids,wins,rounds,first_places,best_rating)
SELECT OLD.player_id,COALESCE(SUM(raids),0),COALESCE(SUM(wins),0),COALESCE(SUM(rounds),0),
COALESCE(SUM(first_places),0),COALESCE(MAX(score),0) FROM raid_scores WHERE player_id=OLD.player_id
ON CONFLICT(player_id) DO UPDATE SET
raids=excluded.raids,wins=excluded.wins,rounds=excluded.rounds,
first_places=excluded.first_places,best_rating=excluded.best_rating;
END;
COMMIT;
)SQL";
}
