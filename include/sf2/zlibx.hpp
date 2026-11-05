

#pragma once

#include <cstddef>
#include <string>

#include "sf2/bytes.hpp"

namespace sf2::zx {

void deflate_to(Buffer &out, ByteView in);
std::vector<uint8_t> deflate(ByteView in);

bool inflate_bounded(ByteView in, size_t max_out, std::vector<uint8_t> &out);

}
