

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sf2/packet.hpp"
#include "sf2/session.hpp"

namespace sf2 {

class Worker;

struct Ctx {
    Worker &w;
    Session &s;
    const packet::Request &req;
    const Config &cfg;
    uint64_t now_ms;
    int64_t wall_ms;
};

enum class Need : uint8_t { Any, Handshaked, LoggedIn, Named };
enum class Cost : uint8_t { Cheap, Heavy, Chat, Lookup };

using HandlerFn = void (*)(Ctx &);

struct HandlerDef {
    int c = 1;
    int a = 13;
    const char *cmd = "";
    Need need = Need::LoggedIn;
    Cost cost = Cost::Cheap;
    HandlerFn fn = nullptr;
};

class Registry {
  public:
    static Registry &instance();

    void add(const HandlerDef &d);
    void add_all(const HandlerDef *defs, size_t n);

    const HandlerDef *find(int c, int a, std::string_view cmd) const;
    std::vector<std::string> commands() const;

    size_t size() const noexcept {
        return by_cmd_.size() + by_sys_.size();
    }

  private:
    static uint64_t sys_key(int c, int a) {
        return (uint64_t(uint32_t(c)) << 32) | uint32_t(a);
    }

    std::unordered_map<std::string, HandlerDef> by_cmd_;
    std::unordered_map<uint64_t, HandlerDef> by_sys_;
};

void register_core_handlers(const Config &cfg);

void register_raid_handlers(const Config &cfg);

bool raid_catalogue_loaded();

void raid_tick(Worker &w, uint64_t mono_ms, int64_t wall_ms);

void on_player_online(Worker &w, Session &s);

void push_season_state(Worker &w, Session &s);
void sync_season_clock(Worker &w);
void sync_session_season(Worker &w, Session &s);
void push_season_notices(Worker &w, Session &s);

void on_player_gone(Worker &w, int64_t player_id);

}
