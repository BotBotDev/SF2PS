
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "sf2/config.hpp"
#include "sf2/gamedata.hpp"
#include "sf2/handlers.hpp"
#include "sf2/log.hpp"
#include "sf2/server.hpp"

namespace {

sf2::Server *g_server = nullptr;

void on_signal(int sig) {
    if (g_server)
        g_server->stop();
    (void)sig;
}

void usage(const char *argv0) {
    printf("SF2 private server (C++)\n"
           "\n"
           "usage: %s [options]\n"
           "  -c, --config FILE     load key=value configuration (default: sf2server.conf)\n"
           "  -s, --set KEY=VALUE   override one setting (repeatable)\n"
           "      --host HOST       bind address           (default 0.0.0.0)\n"
           "  -p, --port PORT       bind port              (default 9933)\n"
           "  -d, --db PATH         sqlite database path   (default players.db)\n"
           "  -t, --threads N       worker threads         (default: online CPUs)\n"
           "  -l, --log LEVEL       trace|debug|info|warn|error|off\n"
           "  -v, --verbose         same as --log debug\n"
           "      --print-config    show the effective configuration and exit\n"
           "      --commands        list registered commands and exit\n"
           "      --check-data      report where the game data was found and exit\n"
           "  -h, --help            this text\n",
           argv0);
}

}

int main(int argc, char **argv) {
    sf2::Config cfg;
    std::string cfg_path = "sf2server.conf";
    std::vector<std::string> overrides;
    bool print_config = false, print_commands = false, check_data = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "-c" || a == "--config")
            cfg_path = next("--config");
        else if (a == "-s" || a == "--set")
            overrides.emplace_back(next("--set"));
        else if (a == "--host")
            overrides.emplace_back(std::string("bind_host=") + next("--host"));
        else if (a == "-p" || a == "--port")
            overrides.emplace_back(std::string("bind_port=") + next("--port"));
        else if (a == "-d" || a == "--db")
            overrides.emplace_back(std::string("db_path=") + next("--db"));
        else if (a == "-t" || a == "--threads")
            overrides.emplace_back(std::string("threads=") + next("--threads"));
        else if (a == "-l" || a == "--log")
            overrides.emplace_back(std::string("log_level=") + next("--log"));
        else if (a == "-v" || a == "--verbose")
            overrides.emplace_back("log_level=debug");
        else if (a == "--print-config")
            print_config = true;
        else if (a == "--commands")
            print_commands = true;
        else if (a == "--check-data")
            check_data = true;
        else {
            fprintf(stderr, "unknown option: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    std::string err;
    if (!cfg.load_file(cfg_path, err)) {
        fprintf(stderr, "config error: %s\n", err.c_str());
        return 2;
    }
    for (const std::string &kv : overrides) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "bad --set (expected KEY=VALUE): %s\n", kv.c_str());
            return 2;
        }
        if (!cfg.set(kv.substr(0, eq), kv.substr(eq + 1), err)) {
            fprintf(stderr, "config error: %s\n", err.c_str());
            return 2;
        }
    }

    sf2::log::Level lvl = sf2::log::Level::Info;
    if (!sf2::log::parse_level(cfg.log_level, lvl)) {
        fprintf(stderr, "bad log_level: %s\n", cfg.log_level.c_str());
        return 2;
    }
    sf2::log::set_level(lvl);

    for (const std::string &k : cfg.unknown_keys)
        LOG_WARN("ignoring an unknown setting — %s (dropped or renamed in this "
                 "build)",
                 k.c_str());

    if (check_data) {
        sf2::GameData::Report r =
            sf2::GameData::instance().load(cfg.game_data_dir, cfg.config_path);
        if (r.ok) {
            printf("game data OK\n  directory : %s\n  contents  : %s\n", r.resolved.c_str(),
                   r.summary().c_str());
            for (const sf2::RaidDef *d : sf2::GameData::instance().raids())
                printf("  raid %-6lld %-22s party=%lld  hp=%-6lld %llds  %s\n", (long long)d->id,
                       d->alias.c_str(), (long long)d->party_size, (long long)d->shield_total,
                       (long long)(d->duration_ms / 1000), d->hard ? "hard" : "");
            return 0;
        }
        printf("GAME DATA NOT FOUND. Looked for '%s' in:\n", cfg.game_data_dir.c_str());
        for (const std::string &root : r.searched)
            printf("  %s\n", root.c_str());
        return 1;
    }

    if (print_config) {
        printf("%s\n", cfg.dump().c_str());
        return 0;
    }
    if (print_commands) {
        sf2::register_core_handlers(cfg);
        sf2::register_raid_handlers(cfg);
        for (const std::string &c : sf2::Registry::instance().commands())
            printf("%s\n", c.c_str());
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);

    sf2::Server server(std::move(cfg));
    g_server = &server;
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (!server.run(err)) {
        LOG_ERROR("startup failed: %s", err.c_str());
        return 1;
    }
    LOG_INFO("shutdown complete");
    return 0;
}
