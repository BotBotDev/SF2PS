#include "sf2/protocol_data.hpp"

#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace sf2::protocol_data {
namespace {

std::mutex load_mutex;
std::vector<std::unique_ptr<const std::vector<uint8_t>>> payloads;
const std::vector<uint8_t> *payload = nullptr;

}

bool load(const std::string &path, std::string &error) {
    std::lock_guard<std::mutex> lock(load_mutex);
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "cannot open anticheat_file: " + path;
        return false;
    }
    const auto length = input.tellg();
    if (length <= 0 || length > 4096) {
        error = "anticheat_file must contain between 1 and 4096 bytes: " + path;
        return false;
    }
    auto data = std::make_unique<std::vector<uint8_t>>(static_cast<size_t>(length));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(data->data()),
                    static_cast<std::streamsize>(data->size()))) {
        error = "cannot read anticheat_file: " + path;
        return false;
    }
    payloads.push_back(std::move(data));
    payload = payloads.back().get();
    error.clear();
    return true;
}

ByteView anticheat_blob() {
    std::lock_guard<std::mutex> lock(load_mutex);
    return payload ? ByteView(*payload) : ByteView{};
}

}
