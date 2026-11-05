
#include "sf2/server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "sf2/handlers.hpp"
#include "sf2/protocol_data.hpp"
#include "sf2/log.hpp"
#include "sf2/responses.hpp"
#include "sf2/protobuf.hpp"

namespace sf2 {
namespace {

uint64_t mono_ms() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}

bool set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

constexpr size_t kReadChunk = 32 * 1024;
constexpr int kMaxEvents = 256;

}

int64_t Worker::wall_ms() const noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void Hub::attach(Worker *w) {
    workers_.push_back(w);
}

bool Hub::acquire_ip(const std::string &ip) {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t &n = per_ip_[ip];
    if (n >= cfg_.max_conns_per_ip)
        return false;
    ++n;
    return true;
}

void Hub::release_ip(const std::string &ip) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = per_ip_.find(ip);
    if (it == per_ip_.end())
        return;
    if (--it->second == 0)
        per_ip_.erase(it);
}

bool Hub::acquire_slot() {
    std::lock_guard<std::mutex> lk(mu_);
    if (live_ >= cfg_.max_connections)
        return false;
    ++live_;
    stats.live.store(live_, std::memory_order_relaxed);
    return true;
}

void Hub::release_slot() {
    std::lock_guard<std::mutex> lk(mu_);
    if (live_)
        --live_;
    stats.live.store(live_, std::memory_order_relaxed);
}

bool Hub::allow_login(const std::string &ip, uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (now_ms - login_prune_ms_ > 60000) {
        login_prune_ms_ = now_ms;
        if (login_bkt_.size() > 8192)
            login_bkt_.clear();
    }
    auto it = login_bkt_.find(ip);
    if (it == login_bkt_.end()) {
        it = login_bkt_.emplace(ip, TokenBucket(cfg_.rate_login_per_ip_s, cfg_.burst_login_ip))
                 .first;
    }
    return it->second.allow(now_ms);
}

bool Hub::allow_chat(int64_t pid, uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (now_ms - chat_prune_ms_ >= 1000) {
        chat_prune_ms_ = now_ms;
        for (auto it = chat_last_ms_.begin(); it != chat_last_ms_.end();) {
            if (now_ms >= it->second && now_ms - it->second >= 1000)
                it = chat_last_ms_.erase(it);
            else
                ++it;
        }
    }
    auto it = chat_last_ms_.find(pid);
    if (it != chat_last_ms_.end() && (now_ms < it->second || now_ms - it->second < 1000))
        return false;
    chat_last_ms_[pid] = now_ms;
    return true;
}

void Hub::claim_player(int64_t pid, Worker *w, int fd, uint64_t gen) {
    if (pid <= 0)
        return;
    Presence old{nullptr, -1, 0};
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = presence_.find(pid);
        if (it != presence_.end())
            old = it->second;
        presence_[pid] = Presence{w, fd, gen};
    }
    if (cfg_.single_session && old.w && !(old.w == w && old.fd == fd)) {
        Mail m;
        m.kind = Mail::Kind::Kick;
        m.fd = old.fd;
        m.gen = old.gen;
        old.w->post(std::move(m));
    }
}

bool Hub::release_player(int64_t pid, int fd, uint64_t gen) {
    if (pid <= 0)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = presence_.find(pid);
    if (it != presence_.end() && it->second.fd == fd && it->second.gen == gen) {
        presence_.erase(it);
        return true;
    }
    return false;
}

bool Hub::send_to_player(int64_t player_id, std::shared_ptr<const std::vector<uint8_t>> packet) {
    if (player_id <= 0 || !packet)
        return false;
    Presence target{nullptr, -1, 0};
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = presence_.find(player_id);
        if (it == presence_.end())
            return false;
        target = it->second;
    }
    if (!target.w)
        return false;
    Mail m;
    m.kind = Mail::Kind::Direct;
    m.packet = std::move(packet);
    m.fd = target.fd;
    m.gen = target.gen;
    target.w->post(std::move(m));
    return true;
}

bool Hub::is_online(int64_t player_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return presence_.count(player_id) != 0;
}

std::vector<int64_t> Hub::online_of(const std::vector<int64_t> &ids) const {
    std::vector<int64_t> out;
    out.reserve(ids.size());
    std::lock_guard<std::mutex> lk(mu_);
    for (int64_t id : ids)
        if (presence_.count(id))
            out.push_back(id);
    return out;
}

void Hub::broadcast(std::shared_ptr<const std::vector<uint8_t>> packet, int64_t exclude_player) {
    for (Worker *w : workers_) {
        Mail m;
        m.kind = Mail::Kind::Broadcast;
        m.packet = packet;
        m.exclude_player = exclude_player;
        w->post(std::move(m));
    }
}

Worker::Worker(int index, const Config &cfg, Hub &hub) : index_(index), cfg_(cfg), hub_(hub) {
    plim_.max_frame = cfg_.max_frame_bytes;
    plim_.max_decompressed = cfg_.max_decompressed_bytes;
    db_ = std::make_unique<Db>(cfg_);
}

Worker::~Worker() {
    if (epfd_ >= 0)
        ::close(epfd_);
    if (listen_ >= 0)
        ::close(listen_);
    if (timerfd_ >= 0)
        ::close(timerfd_);
    if (eventfd_ >= 0)
        ::close(eventfd_);
}

bool Worker::setup_listener(std::string &err) {
    listen_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_ < 0) {
        err = std::string("socket: ") + strerror(errno);
        return false;
    }

    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    if (setsockopt(listen_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) != 0) {
        err = std::string("SO_REUSEPORT: ") + strerror(errno);
        return false;
    }
#endif

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(cfg_.bind_port);
    if (cfg_.bind_host.empty() || cfg_.bind_host == "0.0.0.0")
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, cfg_.bind_host.c_str(), &sa.sin_addr) != 1) {
        err = "bad bind_host: " + cfg_.bind_host;
        return false;
    }
    if (::bind(listen_, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0) {
        err = std::string("bind: ") + strerror(errno);
        return false;
    }
    if (::listen(listen_, cfg_.backlog) != 0) {
        err = std::string("listen: ") + strerror(errno);
        return false;
    }
    return true;
}

bool Worker::start(std::string &err) {
    if (!db_->open(err))
        return false;
    if (!setup_listener(err))
        return false;

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        err = std::string("epoll_create1: ") + strerror(errno);
        return false;
    }

    timerfd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    itimerspec its{};
    its.it_interval.tv_sec = 1;
    its.it_value.tv_sec = 1;
    timerfd_settime(timerfd_, 0, &its, nullptr);

    eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    auto add = [&](int fd, uint32_t ev) {
        epoll_event e{};
        e.events = ev;
        e.data.fd = fd;
        return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &e) == 0;
    };
    if (!add(listen_, EPOLLIN) || !add(timerfd_, EPOLLIN) || !add(eventfd_, EPOLLIN)) {
        err = std::string("epoll_ctl: ") + strerror(errno);
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void Worker::stop() {
    running_.store(false);
    if (eventfd_ >= 0) {
        uint64_t one = 1;
        ssize_t r = ::write(eventfd_, &one, 8);
        (void)r;
    }
}

void Worker::join() {
    if (thread_.joinable())
        thread_.join();
}

void Worker::post(Mail m) {
    {
        std::lock_guard<std::mutex> lk(mail_mu_);
        mail_.push_back(std::move(m));
    }
    uint64_t one = 1;
    ssize_t r = ::write(eventfd_, &one, 8);
    (void)r;
}

void Worker::run() {
    std::vector<epoll_event> evs(kMaxEvents);
    while (running_.load(std::memory_order_relaxed)) {
        int n = epoll_wait(epfd_, evs.data(), kMaxEvents, 1000);
        now_ms_ = mono_ms();
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERROR("worker %d: epoll_wait: %s", index_, strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            const int fd = evs[size_t(i)].data.fd;
            const uint32_t ev = evs[size_t(i)].events;

            if (fd == listen_) {
                on_accept();
                continue;
            }
            if (fd == timerfd_) {
                uint64_t ticks;
                ssize_t r = ::read(timerfd_, &ticks, 8);
                (void)r;
                sweep_timeouts();
                continue;
            }
            if (fd == eventfd_) {
                uint64_t v;
                ssize_t r = ::read(eventfd_, &v, 8);
                (void)r;
                drain_mailbox();
                continue;
            }

            auto it = sessions_.find(fd);
            if (it == sessions_.end())
                continue;
            Session &s = *it->second;

            if (ev & (EPOLLHUP | EPOLLERR)) {
                close_session(s, "peer hangup");
                continue;
            }
            if (ev & EPOLLOUT)
                on_writable(s);
            if ((ev & EPOLLIN) && s.state != SessionState::Closing)
                on_readable(s);
        }

        for (int fd : doomed_)
            destroy(fd);
        doomed_.clear();
    }

    for (auto &kv : sessions_) {
        const Session &s = *kv.second;
        if (s.player_id && s.state != SessionState::Closing)
            log::audit("logout",
                       "player=%lld session=%llu ip=%s duration_ms=%llu reason=server_shutdown",
                       (long long)s.player_id, (unsigned long long)s.gen, s.ip.c_str(),
                       (unsigned long long)(mono_ms() - s.connected_ms));
        hub_.release_ip(kv.second->ip);
        hub_.release_slot();
        if (kv.second->player_id)
            hub_.release_player(kv.second->player_id, kv.second->fd, kv.second->gen);
        ::close(kv.first);
    }
    sessions_.clear();
}

void Worker::on_accept() {
    for (;;) {
        sockaddr_in sa{};
        socklen_t sl = sizeof sa;
        int fd = ::accept4(listen_, reinterpret_cast<sockaddr *>(&sa), &sl,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                log::throttled(log::Level::Error, "accept-emfile", 5000,
                               "worker %d: out of file descriptors", index_);
                return;
            }
            return;
        }

        char ipbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &sa.sin_addr, ipbuf, sizeof ipbuf);
        std::string ip = ipbuf;

        if (!hub_.acquire_slot()) {
            hub_.stats.rejected_cap.fetch_add(1, std::memory_order_relaxed);
            log::throttled(log::Level::Warn, "cap", 5000, "connection cap reached, refusing %s",
                           ip.c_str());
            ::close(fd);
            continue;
        }
        if (!hub_.acquire_ip(ip)) {
            hub_.release_slot();
            hub_.stats.rejected_ip.fetch_add(1, std::memory_order_relaxed);
            log::throttled(log::Level::Warn, "perip", 5000, "per-IP connection cap reached for %s",
                           ip.c_str());
            ::close(fd);
            continue;
        }

        if (cfg_.tcp_nodelay) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        }
        set_nonblock(fd);

        auto sp = std::make_unique<Session>();
        sp->fd = fd;
        sp->gen = next_gen_++;
        sp->ip = std::move(ip);
        sp->port = ntohs(sa.sin_port);
        sp->connected_ms = now_ms_ ? now_ms_ : mono_ms();
        sp->last_rx_ms = sp->connected_ms;
        sp->configure(cfg_);
        sp->rx.reserve(2048);

        epoll_event e{};
        e.events = EPOLLIN;
        e.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &e) != 0) {
            hub_.release_ip(sp->ip);
            hub_.release_slot();
            ::close(fd);
            continue;
        }
        hub_.stats.accepted.fetch_add(1, std::memory_order_relaxed);
        LOG_DEBUG("[%s] connected (worker %d)", sp->remote().c_str(), index_);
        sessions_.emplace(fd, std::move(sp));
    }
}

void Worker::on_readable(Session &s) {
    static thread_local std::vector<uint8_t> chunk(kReadChunk);
    for (;;) {
        ssize_t n = ::recv(s.fd, chunk.data(), chunk.size(), 0);
        if (n > 0) {
            s.bytes_in += size_t(n);
            hub_.stats.bytes_in.fetch_add(uint64_t(n), std::memory_order_relaxed);
            s.last_rx_ms = now_ms_;

            if (s.rx.size() + size_t(n) > size_t(cfg_.max_frame_bytes) * 2) {
                close_session(s, "receive buffer overflow");
                return;
            }
            s.rx.insert(s.rx.end(), chunk.begin(), chunk.begin() + n);
            drain_frames(s);
            if (s.state == SessionState::Closing)
                return;
            if (size_t(n) < chunk.size())
                return;
            continue;
        }
        if (n == 0) {
            close_session(s, "peer closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        close_session(s, "recv error");
        return;
    }
}

void Worker::drain_frames(Session &s) {
    size_t off = 0;
    while (off < s.rx.size()) {
        ByteView buf(s.rx.data() + off, s.rx.size() - off);
        packet::FrameInfo fi = packet::probe(buf, plim_);
        if (fi.state == packet::FrameState::Incomplete)
            break;
        if (fi.state == packet::FrameState::Invalid) {
            hub_.stats.proto_errors.fetch_add(1, std::memory_order_relaxed);
            log::throttled(log::Level::Warn, "frame", 2000, "[%s] bad frame: %s",
                           s.remote().c_str(), fi.error ? fi.error : "?");
            close_session(s, "bad frame");
            return;
        }

        if (!s.bkt_packets.allow(now_ms_)) {
            hub_.stats.rate_limited.fetch_add(1, std::memory_order_relaxed);
            log::throttled(log::Level::Warn, "ratelimit", 5000, "[%s] packet rate limit exceeded",
                           s.remote().c_str());
            close_session(s, "packet flood");
            return;
        }

        ++s.packets_in;
        hub_.stats.packets_in.fetch_add(1, std::memory_order_relaxed);

        try {
            sfs::Value root = packet::parse(buf.sub(0, fi.total), plim_, scratch_);
            packet::Request req = packet::route(root);
            dispatch(s, req);
        } catch (const DecodeError &e) {
            hub_.stats.proto_errors.fetch_add(1, std::memory_order_relaxed);
            log::throttled(log::Level::Warn, "decode", 2000, "[%s] malformed packet: %s",
                           s.remote().c_str(), e.what());
            close_session(s, "malformed packet");
            return;
        } catch (const std::exception &e) {
            log::throttled(log::Level::Error, "handler", 2000, "[%s] handler failure: %s",
                           s.remote().c_str(), e.what());
            close_session(s, "handler failure");
            return;
        }

        off += fi.total;
        if (s.state == SessionState::Closing)
            break;
    }

    if (off) {
        s.rx.erase(s.rx.begin(), s.rx.begin() + long(off));
        if (s.rx.capacity() > 64 * 1024 && s.rx.size() < 4096)
            s.rx.shrink_to_fit();
    }
}

void Worker::dispatch(Session &s, const packet::Request &req) {
    if (s.access_revoked || (s.logged_in() && enforce_moderation(s)))
        return;
    const HandlerDef *h = Registry::instance().find(req.c, req.a, req.cmd);
    if (!h || !h->fn) {
        log::throttled(log::Level::Debug, "nohandler", 5000,
                       "[%s] no handler for c=%d a=%d cmd=%.*s", s.remote().c_str(), req.c, req.a,
                       int(req.cmd.size() > 32 ? 32 : req.cmd.size()), req.cmd.data());
        return;
    }

    switch (h->need) {
    case Need::Any:
        break;
    case Need::Handshaked:
        if (s.state == SessionState::New) {
            close_session(s, "command before handshake");
            return;
        }
        break;
    case Need::LoggedIn:
        if (!s.logged_in()) {
            log::throttled(log::Level::Warn, "preauth", 5000, "[%s] pre-auth command refused",
                           s.remote().c_str());
            close_session(s, "command before login");
            return;
        }
        break;
    case Need::Named:
        if (!s.logged_in() || !s.named())
            return;
        break;
    }

    bool ok = true;
    switch (h->cost) {
    case Cost::Cheap:
        break;
    case Cost::Heavy:
        ok = s.bkt_heavy.allow(now_ms_);
        break;
    case Cost::Chat:
        ok = hub_.allow_chat(s.player_id, now_ms_) && s.bkt_chat.allow(now_ms_);
        break;
    case Cost::Lookup:
        ok = s.bkt_lookup.allow(now_ms_);
        break;
    }
    if (!ok) {
        hub_.stats.rate_limited.fetch_add(1, std::memory_order_relaxed);
        log::throttled(log::Level::Debug, "ratelimit-cmd", 5000, "[%s] rate limited: %.*s",
                       s.remote().c_str(), int(req.cmd.size()), req.cmd.data());
        return;
    }

    if (cfg_.enable_raids) {
        sync_season_clock(*this);
        sync_session_season(*this, s);
    }
    Ctx ctx{*this, s, req, cfg_, now_ms_, wall_ms()};
    if (s.logged_in() && cfg_.enable_clans)
        s.player.clan_id = db().clan_of(s.player_id);
    if (req.cmd.substr(0, 5) == "clan_")
        LOG_DEBUG("clan command player=%lld cmd=%.*s", (long long)s.player_id, int(req.cmd.size()),
                  req.cmd.data());
    if (s.logged_in())
        log::audit("command", "player=%lld session=%llu cmd=%s request=%lld bytes=%zu",
                   (long long)s.player_id, (unsigned long long)s.gen, h->cmd, (long long)req.req_id,
                   req.body.size());

    if (s.logged_in())
        if (auto balance = db().gem_balance(s.player_id))
            s.player.gems = *balance;
    h->fn(ctx);
    if (s.named() && (req.cmd == "get_player" || req.cmd == "create_player"))
        s.gem_sync_ready = true;
    sync_gems(s);
}

void Worker::sync_gems(Session &s) {
    if (!s.logged_in() || !s.named() || !s.gem_sync_ready)
        return;
    auto balance = db().gem_balance(s.player_id);
    if (!balance)
        return;
    s.player.gems = *balance;
    if (s.gems_sent && s.last_gems_sent == *balance)
        return;

    auto player = db().by_id(s.player_id);
    if (!player)
        return;
    s.player = std::move(*player);
    const auto confirmed = s.player.gems;
    if (s.gems_sent && s.last_gems_sent == confirmed)
        return;
    Buffer profile, ledger;
    proto::build_player_info(profile, s.player);
    resp::ext_event(ledger, "ledger_update_event", profile.view(), true);
    send(s, ledger.view());
    Buffer payload, event;
    resp::proto_varint(payload, confirmed);
    resp::ext_event(event, "refresh_gems", payload.view());
    send(s, event.view());
    s.gems_sent = true;
    s.last_gems_sent = confirmed;
}

void Worker::send(Session &s, ByteView p) {
    if (s.access_revoked || s.state == SessionState::Closing || p.empty())
        return;

    if (s.tx.size() - s.tx_off + p.size() > cfg_.max_write_queue_bytes) {
        log::throttled(log::Level::Warn, "slowclient", 5000,
                       "[%s] write queue overflow, dropping client", s.remote().c_str());
        close_session(s, "slow client");
        return;
    }
    s.tx.insert(s.tx.end(), p.data(), p.data() + p.size());
    ++s.packets_out;
    hub_.stats.packets_out.fetch_add(1, std::memory_order_relaxed);
    flush(s);
}

void Worker::flush(Session &s) {
    while (s.tx_off < s.tx.size()) {
        ssize_t n = ::send(s.fd, s.tx.data() + s.tx_off, s.tx.size() - s.tx_off, MSG_NOSIGNAL);
        if (n > 0) {
            s.tx_off += size_t(n);
            s.bytes_out += size_t(n);
            hub_.stats.bytes_out.fetch_add(uint64_t(n), std::memory_order_relaxed);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n < 0 && errno == EINTR)
            continue;
        close_session(s, "send error");
        return;
    }

    if (s.tx_off == s.tx.size()) {
        s.tx.clear();
        s.tx_off = 0;
        if (s.want_write) {
            s.want_write = false;
            arm(s);
        }
    } else {
        if (s.tx_off > 64 * 1024) {
            s.tx.erase(s.tx.begin(), s.tx.begin() + long(s.tx_off));
            s.tx_off = 0;
        }
        if (!s.want_write) {
            s.want_write = true;
            arm(s);
        }
    }
}

void Worker::arm(Session &s) {
    epoll_event e{};
    e.events = EPOLLIN | (s.want_write ? EPOLLOUT : 0u);
    e.data.fd = s.fd;
    epoll_ctl(epfd_, EPOLL_CTL_MOD, s.fd, &e);
}

void Worker::on_writable(Session &s) {
    flush(s);
}

void Worker::kick(Session &s, int kind, int64_t remaining_ms, std::string_view reason) {
    if (s.access_revoked || s.state == SessionState::Closing)
        return;
    Buffer body, packet;

    pb::v(body, 1, uint64_t(kind));
    if (!reason.empty())
        pb::str(body, 2, reason);
    if (kind == 1)
        pb::v(body, 3, uint64_t(remaining_ms));
    resp::ext_event(packet, "kick", body.view());
    send(s, packet.view());
    s.access_revoked = true;

    s.disconnect_at_ms = now_ms_ + 2000;
    LOG_INFO("kick player=%lld kind=%d remaining_ms=%lld reason=%.*s", (long long)s.player_id, kind,
             (long long)remaining_ms, int(reason.size()), reason.data());
}

void Worker::refresh_controls(Session &s) {
    if (!s.logged_in() || s.access_revoked)
        return;
    auto current = db().mutes(s.player_id);
    auto emit = [&](int64_t channel, int64_t until) {
        Buffer b, p;
        pb::v(b, 1, uint64_t(channel));
        pb::v(b, 2, uint64_t(until));
        resp::ext_event(p, "mute_event", b.view());
        send(s, p.view());
    };
    for (const auto &old : s.mute_state)
        if (std::find_if(current.begin(), current.end(),
                         [&](const auto &m) { return m.first == old.first; }) == current.end())
            emit(old.first, 0);
    for (const auto &m : current)
        if (std::find(s.mute_state.begin(), s.mute_state.end(), m) == s.mute_state.end())
            emit(m.first, m.second);
    s.mute_state = current;
    s.player.mutes = std::move(current);
    if (s.season_state_pushed) {
        auto notice = db().reboot_notice();
        if (notice && notice->first > s.reboot_seq && notice->second > wall_ms()) {
            Buffer b, p;
            pb::v(b, 1, uint64_t(notice->second));
            resp::ext_event(p, "reboot_event", b.view());
            send(s, p.view());
            s.reboot_seq = notice->first;
        }
    }
}

bool Worker::enforce_moderation(Session &s) {
    if (s.access_revoked)
        return true;
    if (!s.logged_in() || !s.player_id)
        return false;
    auto m = db().moderation(s.player_id);
    if (!m)
        return false;
    if (m->banned_until == -1 || m->banned_until > wall_ms()) {
        kick(s, 1, m->banned_until == -1 ? -1 : m->banned_until - wall_ms(), m->reason);
        return true;
    }
    if (m->kick_seq > s.moderation_seq) {
        s.moderation_seq = m->kick_seq;
        kick(s, m->kick_kind, 0, m->reason);
        return true;
    }
    return false;
}

void Worker::close_session(Session &s, const char *why) {
    if (s.state == SessionState::Closing)
        return;
    if (s.player_id)
        log::audit("logout", "player=%lld session=%llu ip=%s duration_ms=%llu reason=\"%s\"",
                   (long long)s.player_id, (unsigned long long)s.gen, s.ip.c_str(),
                   (unsigned long long)(mono_ms() - s.connected_ms), why);
    s.state = SessionState::Closing;
    LOG_DEBUG("[%s] closing: %s", s.remote().c_str(), why);
    doomed_.push_back(s.fd);
}

void Worker::destroy(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end())
        return;
    Session &s = *it->second;
    if (s.player_id && s.state != SessionState::Closing)
        log::audit("logout",
                   "player=%lld session=%llu ip=%s duration_ms=%llu reason=server_shutdown",
                   (long long)s.player_id, (unsigned long long)s.gen, s.ip.c_str(),
                   (unsigned long long)(mono_ms() - s.connected_ms));

    if (s.player_id) {

        if (hub_.release_player(s.player_id, s.fd, s.gen))
            on_player_gone(*this, s.player_id);
    }
    hub_.release_ip(s.ip);
    hub_.release_slot();
    hub_.stats.closed.fetch_add(1, std::memory_order_relaxed);

    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    sessions_.erase(it);
}

void Worker::sweep_timeouts() {
    const uint64_t now = now_ms_ ? now_ms_ : mono_ms();

    if (index_ == 0)
        raid_tick(*this, now, wall_ms());

    for (auto &kv : sessions_) {
        Session &s = *kv.second;
        if (s.state == SessionState::Closing)
            continue;
        if (s.access_revoked) {
            if (now >= s.disconnect_at_ms)
                close_session(s, "kick delivered");
            continue;
        }
        if (enforce_moderation(s))
            continue;
        sync_gems(s);
        refresh_controls(s);
        if (cfg_.enable_raids) {
            sync_session_season(*this, s);
            if (s.season_state_pushed)
                push_season_notices(*this, s);
        }

        uint64_t idle = now - s.last_rx_ms;
        if (idle > uint64_t(cfg_.idle_timeout_s) * 1000) {
            close_session(s, "idle timeout");
            continue;
        }
        if (s.state == SessionState::New &&
            now - s.connected_ms > uint64_t(cfg_.handshake_timeout_s) * 1000) {
            close_session(s, "handshake timeout");
            continue;
        }
        if (s.state == SessionState::Handshaked &&
            now - s.handshaked_ms > uint64_t(cfg_.login_timeout_s) * 1000) {
            close_session(s, "login timeout");
            continue;
        }
    }
    for (int fd : doomed_)
        destroy(fd);
    doomed_.clear();
}

void Worker::drain_mailbox() {
    std::vector<Mail> batch;
    {
        std::lock_guard<std::mutex> lk(mail_mu_);
        batch.swap(mail_);
    }
    for (Mail &m : batch) {
        if (m.kind == Mail::Kind::Kick) {
            auto it = sessions_.find(m.fd);
            if (it != sessions_.end() && it->second->gen == m.gen)
                kick(*it->second, 0, 0, "another session login");
            continue;
        }
        if (!m.packet)
            continue;
        if (m.kind == Mail::Kind::Direct) {
            auto it = sessions_.find(m.fd);

            if (it != sessions_.end() && it->second->gen == m.gen && it->second->logged_in() &&
                it->second->state != SessionState::Closing)
                send(*it->second, ByteView(*m.packet));
            continue;
        }
        ByteView pv(*m.packet);
        for (auto &kv : sessions_) {
            Session &s = *kv.second;
            if (!s.logged_in() || s.state == SessionState::Closing)
                continue;
            if (m.exclude_player && s.player_id == m.exclude_player)
                continue;
            send(s, pv);
        }
    }
    for (int fd : doomed_)
        destroy(fd);
    doomed_.clear();
}

Server::Server(Config cfg) : cfg_(std::move(cfg)), hub_(cfg_) {}

Server::~Server() = default;

bool Server::run(std::string &err) {
    int n = cfg_.threads > 0 ? cfg_.threads : int(std::thread::hardware_concurrency());
    if (n <= 0)
        n = 1;
    if (n > 64)
        n = 64;

    if (!protocol_data::load(cfg_.anticheat_file, err))
        return false;

    if (!db_bootstrap(cfg_, err))
        return false;
    register_core_handlers(cfg_);
    register_raid_handlers(cfg_);
    if (cfg_.enable_raids && cfg_.require_game_data && !raid_catalogue_loaded()) {
        err = "the raid catalogue could not be found (see the errors above). "
              "Set game_data_dir to the directory holding config.xml, or set "
              "require_game_data = false to run with fallback raid numbers.";
        return false;
    }

    for (int i = 0; i < n; ++i) {
        auto w = std::make_unique<Worker>(i, cfg_, hub_);
        hub_.attach(w.get());
        workers_.push_back(std::move(w));
    }
    for (auto &w : workers_) {
        if (!w->start(err)) {
            for (auto &x : workers_)
                x->stop();
            for (auto &x : workers_)
                x->join();
            return false;
        }
    }

    LOG_INFO("listening on %s:%u with %d worker%s, %zu commands registered", cfg_.bind_host.c_str(),
             unsigned(cfg_.bind_port), n, n == 1 ? "" : "s", Registry::instance().size());
    LOG_INFO("config: %s", cfg_.dump().c_str());

    LOG_INFO("new accounts receive: items=[%s] perks=[%s] gems=%lld", cfg_.new_player_items.c_str(),
             cfg_.new_player_perks.c_str(), (long long)cfg_.new_player_gems);
    if (cfg_.new_player_items.empty() && cfg_.new_player_perks.empty())
        LOG_WARN("new_player_items and new_player_perks are both empty — new "
                 "accounts start with no raid keys and no spheres");

    for (auto &w : workers_)
        w->join();
    return true;
}

void Server::stop() {
    if (stopping_.exchange(true))
        return;
    for (auto &w : workers_)
        w->stop();
}

}
