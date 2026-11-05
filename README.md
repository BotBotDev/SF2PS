<div align="center">

# Shadow Fight 2 Private Server

**A standalone C++ server implementation for Shadow Fight 2.**

In-game registration · Raids · Clans · Seasons · Chat

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square)
![Build](https://img.shields.io/badge/build-CMake-064F8C?style=flat-square)
![Database](https://img.shields.io/badge/database-SQLite-003B57?style=flat-square)
![Platform](https://img.shields.io/badge/platform-Linux-333333?style=flat-square)

[Getting started](#getting-started) · [Configuration](#configuration) · [Project layout](#project-layout)

</div>

> **Unofficial project.** This is not a Nekki product. It is not affiliated with, endorsed, sponsored, maintained, or supported by Nekki. Shadow Fight 2 and related names, trademarks, and game materials belong to their respective owners.

## Overview

Shadow Fight 2 Private Server implements the game's SFS2X-compatible network protocol with SQLite-backed player storage. Accounts are created through the game: a new client signs in, chooses a player name, and continues with the same account on subsequent connections.

The server includes:

- Player profiles, inventory, currency, and progression.
- Raid matchmaking, battles, rewards, and bot participants.
- Clan management and clan raids.
- Seasonal ratings, leagues, and profile statistics.
- Global and clan chat with message history.
- Request validation, rate limits, and account moderation.

This repository contains the server sources. A compatible game client and the raid configuration archive must be supplied separately. A client-compatible binary payload is included in `config/anticheat.bin`.

> **Tested only with Shadow Fight 2 version 2.32.0.** Other versions have not been verified.
>
> **Two small exercises are left to the reader:**
>
> 1. Obtain `raid_configs_all.zip` yourself and place its decoded XML files in `config/`, with the main file at `config/config.xml`. The archive and these XML files are not included here.
> 2. Redirect a compatible game client to the IP address or hostname of your own server running SF2PS, using TCP port `9933` unless you configured another port. The client modification is yours to work out; launching the server will not persuade the game to find it on its own.
>
> Consider these the entrance exam. The server takes over once you can reach it.

## Getting started

### 1. Install dependencies

You need Linux, a C++17 compiler, CMake 3.13 or later, SQLite3 development files, and zlib development files.

On Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential cmake libsqlite3-dev zlib1g-dev
```

### 2. Add the raid configuration

**Place the decoded raid configuration XML files from `raid_configs_all.zip` in the `config/` directory.** The outer archive contains `raid_configs.zip.enc` and `raid_configs.zip.sha`; simply extracting it does not produce the XML files the server needs. Obtain the decoded contents of the inner raid configuration archive before continuing. The main configuration file must be at:

```text
config/config.xml
```

Avoid an extra directory level such as `config/config/config.xml`. Keep the accompanying XML files and subdirectories from the archive together; the server loads them as game data.

The raid configuration archive is not included in this repository. Create `config/` if it is missing after cloning.

### 3. Client-compatible payload

The login response includes an opaque binary field expected by the client. Its raw payload is included as `config/anticheat.bin` and loaded by default. Set `anticheat_file` to use a different file.

This file is separate from `raid_configs_all.zip`; keep it in place when extracting the raid configuration. The server reads it without modification before starting workers. A missing, empty, oversized, or unreadable file prevents startup with an error rather than sending a placeholder to clients. Loading a file does not validate its compatibility with a particular client.

Other files in `config/` are ignored by Git. Only `config/anticheat.bin` is explicitly included.

### 4. Build

Run these commands from the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The resulting executable is `build/sf2server`.

### 5. Start the server

```sh
./build/sf2server --host 127.0.0.1 --port 9933 --db players.db
```

This starts a local listener and creates the SQLite database if it does not exist. Game data is loaded from `config/` relative to the working directory. Stop the server with `Ctrl+C`.

To accept connections from other devices, use `--host 0.0.0.0` and configure the compatible client to connect to your server address on port `9933`.

## Configuration

The server reads `sf2server.conf` from the working directory when present. Settings use `key=value` syntax; command-line overrides take precedence.

Example `sf2server.conf`:

```ini
bind_host=127.0.0.1
bind_port=9933
db_path=players.db
game_data_dir=config
anticheat_file=config/anticheat.bin
auth_mode=original
enable_raids=true
enable_clans=true
enable_chat=true
log_level=info
```

Load a different configuration file or override individual settings:

```sh
./build/sf2server --config sf2server.conf
./build/sf2server --set game_data_dir=/path/to/config --log debug
```

Client protocol compatibility is required. If you set `client_revision` above zero, the client must negotiate an accepted internal revision before login.

### Command-line reference

| Option | Purpose |
| --- | --- |
| `--config FILE` | Read a configuration file. |
| `--set KEY=VALUE` | Override a setting; may be repeated. |
| `--host HOST` | Set the listener address. |
| `--port PORT` | Set the listener port. |
| `--db PATH` | Set the SQLite database path. |
| `--threads N` | Set the worker thread count. |
| `--log LEVEL` | Set logging to `trace`, `debug`, `info`, `warn`, `error`, or `off`. |
| `--print-config` | Print the effective configuration and exit. |
| `--commands` | List registered protocol commands and exit. |
| `--check-data` | Report game data discovery and exit. |
| `--help` | Show all command-line options. |

## Project layout

```text
.
├── src/              Server implementation
├── include/sf2/      Headers and shared types
├── config/           Included binary payload and separately supplied raid configuration
├── CMakeLists.txt    Build definition
├── LICENSE           Source code license
└── README.md
```

## License

The original server implementation is released under [The Unlicense](LICENSE).

The Unlicense applies to the original server implementation. It does not apply to the included `config/anticheat.bin` payload, the game client, trademarks, or other third-party materials. Including or referencing such materials does not transfer their ownership or grant rights beyond those available from their respective rights holders.

## Disclaimer

This project is provided for independent development and research. It is not an official replacement for Nekki services, and no compatibility, availability, or ongoing maintenance is guaranteed.

The software is provided **as is**, without warranties of any kind. Use it at your own risk. To the extent permitted by applicable law, the authors and contributors are not liable for losses or damages arising from its use, including loss of data, account access, or service availability. You are responsible for complying with applicable laws and any terms governing the software and materials you use with this project.
