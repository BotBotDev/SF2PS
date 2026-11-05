#pragma once

#include <string>

#include "sf2/bytes.hpp"

namespace sf2::protocol_data {

bool load(const std::string &path, std::string &error);
ByteView anticheat_blob();

}
