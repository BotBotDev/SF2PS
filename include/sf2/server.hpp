

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sf2/config.hpp"
#include "sf2/db.hpp"
#include "sf2/packet.hpp"
#include "sf2/ratelimit.hpp"
#include "sf2/session.hpp"

namespace sf2 {

class Worker;

struct Mail {
    enum class Kind : uint8_t { Broadcast, Kick, Direct } kind = Kind::Broadcast;
    std::shared_ptr<const std::vector<uint8_t>> packet;
    int64_t exclude_player = 0;
    int fd = -1;
    uint64_t gen = 0;
};

struct Stats {
    std::atomic<uint64_t> accepted{0}, rejected_ip{0}, rejected_cap{0};
    std::atomic<uint64_t> closed{0}, proto_errors{0}, rate_limited{0};
    std::atomic<uint64_t> logins_ok{0}, logins_failed{0};
    std::atomic<uint64_t> packets_in{0}, packets_out{0};
    std::atomic<uint64_t> bytes_in{0}, bytes_out{0};
    std::atomic<uint64_t> live{0};
};

class Hub {
  public:
    explicit Hub(const Config &cfg) : cfg_(cfg) {}

    void attach(Worker *w);

    const std::vector<Worker *> &workers() const noexcept {
        return workers_;
    }

    bool acquire_ip(const std::string &ip);
    void release_ip(const std::string &ip);

    bool acquire_slot();
    void release_slot();

    bool allow_login(const std::string &ip, uint64_t now_ms);

    bool allow_chat(int64_t player_id, uint64_t now_ms);

    void claim_player(int64_t player_id, Worker *w, int fd, uint64_t gen);
    bool release_player(int64_t player_id, int fd, uint64_t gen);

    void broadcast(std::shared_ptr<const std::vector<uint8_t>> packet, int64_t exclude_player);

    bool send_to_player(int64_t player_id, std::shared_ptr<const std::vector<uint8_t>> packet);
    bool is_online(int64_t player_id) const;
    std::vector<int64_t> online_of(const std::vector<int64_t> &ids) const;

    Stats stats;

  private:
    struct Presence {
        Worker *w;
        int fd;
        uint64_t gen;
    };

    const Config &cfg_;
    std::vector<Worker *> workers_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, uint32_t> per_ip_;
    std::unordered_map<std::string, TokenBucket> login_bkt_;
    uint64_t login_prune_ms_ = 0;
    std::unordered_map<int64_t, Presence> presence_;
    std::unordered_map<int64_t, uint64_t> chat_last_ms_;
    uint64_t chat_prune_ms_ = 0;
    uint32_t live_ = 0;
};

class Worker {
  public:
    Worker(int index, const Config &cfg, Hub &hub);
    ~Worker();

    bool start(std::string &err);
    void run();
    void stop();
    void join();

    int index() const noexcept {
        return index_;
    }

    Db &db() noexcept {
        return *db_;
    }

    const Config &cfg() const noexcept {
        return cfg_;
    }

    Hub &hub() noexcept {
        return hub_;
    }

    uint64_t now_ms() const noexcept {
        return now_ms_;
    }

    int64_t wall_ms() const noexcept;

    void send(Session &s, ByteView packet);
    void close_session(Session &s, const char *why);
    void kick(Session &s, int kind, int64_t remaining_ms, std::string_view reason = {});
    bool enforce_moderation(Session &s);
    void refresh_controls(Session &s);
    void sync_gems(Session &s);
    void post(Mail m);

  private:
    bool setup_listener(std::string &err);
    void on_accept();
    void on_readable(Session &s);
    void on_writable(Session &s);
    void drain_frames(Session &s);
    void dispatch(Session &s, const packet::Request &req);
    void flush(Session &s);
    void arm(Session &s);
    void sweep_timeouts();
    void drain_mailbox();
    void destroy(int fd);

    int index_;
    const Config &cfg_;
    Hub &hub_;
    packet::Limits plim_;

    int epfd_ = -1;
    int listen_ = -1;
    int timerfd_ = -1;
    int eventfd_ = -1;

    std::unique_ptr<Db> db_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::unordered_map<int, std::unique_ptr<Session>> sessions_;
    std::vector<int> doomed_;

    std::mutex mail_mu_;
    std::vector<Mail> mail_;

    std::vector<uint8_t> scratch_;
    uint64_t now_ms_ = 0;
    uint64_t next_gen_ = 1;
};

class Server {
  public:
    explicit Server(Config cfg);
    ~Server();

    bool run(std::string &err);
    void stop();

    Hub &hub() noexcept {
        return hub_;
    }

  private:
    Config cfg_;
    Hub hub_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<bool> stopping_{false};
};

}
