

#pragma once

#include <cstdint>
#include <optional>

#include "sf2/bytes.hpp"
#include "sf2/sfs.hpp"

namespace sf2::packet {

inline constexpr uint8_t kBinary = 0x80;
inline constexpr uint8_t kBigSize = 0x40;
inline constexpr uint8_t kCompressed = 0x20;
inline constexpr uint8_t kKnownFlags = kBinary | kBigSize | kCompressed;

struct Limits {
    size_t max_frame = 64 * 1024;
    size_t max_decompressed = 256 * 1024;
    sfs::Limits sfs{};
};

enum class FrameState { Incomplete, Ready, Invalid };

struct FrameInfo {
    FrameState state = FrameState::Incomplete;
    size_t total = 0;
    size_t header = 0;
    size_t body = 0;
    uint8_t flags = 0;
    const char *error = nullptr;
};

FrameInfo probe(ByteView buf, const Limits &lim) noexcept;

sfs::Value parse(ByteView frame, const Limits &lim, std::vector<uint8_t> &scratch);

struct Request {
    int c = -1;
    int a = -1;
    std::string_view cmd;
    const sfs::Value *params = nullptr;
    const sfs::Value *inner = nullptr;
    int32_t req_id = 0;
    ByteView body;
};

Request route(const sfs::Value &root) noexcept;

void frame_into(Buffer &out, ByteView payload, bool compress);
std::vector<uint8_t> frame(ByteView payload, bool compress);

}
