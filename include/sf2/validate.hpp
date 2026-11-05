

#pragma once

#include <string>
#include <string_view>

#include "sf2/bytes.hpp"
#include "sf2/config.hpp"
#include "sf2/json.hpp"

namespace sf2::validate {

bool utf8_ok(std::string_view s, size_t *codepoints = nullptr) noexcept;

std::string sanitize_text(std::string_view in, size_t max_codepoints);

struct NameResult {
    bool ok = false;
    std::string value;
    const char *reason = nullptr;
};

NameResult check_player_name(std::string_view raw, const Config &cfg);

std::string make_tag();

NameResult check_chat_text(std::string_view raw, const Config &cfg);

bool check_login_uuid(std::string_view s) noexcept;
bool check_email(std::string_view s) noexcept;

struct InventoryResult {
    bool ok = false;
    json::Value value;
    const char *reason = nullptr;
};

InventoryResult check_inventory(std::string_view json_text, const Config &cfg);

bool is_identifier(std::string_view s, size_t max_len = 64) noexcept;

}
