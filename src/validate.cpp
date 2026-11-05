
#include "sf2/validate.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "sf2/crypto.hpp"

namespace sf2::validate {
namespace {

bool decode_cp(std::string_view s, size_t &i, uint32_t &cp) noexcept {
    unsigned char c = uint8_t(s[i]);
    size_t need;
    uint32_t v;
    if (c < 0x80) {
        cp = c;
        ++i;
        return true;
    } else if ((c & 0xE0) == 0xC0) {
        need = 1;
        v = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        v = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        v = c & 0x07u;
    } else
        return false;

    if (i + need >= s.size())
        return false;
    for (size_t k = 1; k <= need; ++k) {
        unsigned char cc = uint8_t(s[i + k]);
        if ((cc & 0xC0) != 0x80)
            return false;
        v = (v << 6) | (cc & 0x3Fu);
    }

    if (need == 1 && v < 0x80)
        return false;
    if (need == 2 && v < 0x800)
        return false;
    if (need == 3 && v < 0x10000)
        return false;
    if (v > 0x10FFFF)
        return false;
    if (v >= 0xD800 && v <= 0xDFFF)
        return false;
    cp = v;
    i += need + 1;
    return true;
}

bool is_bad_cp(uint32_t cp) noexcept {
    if (cp < 0x20 || cp == 0x7F)
        return true;
    if (cp >= 0x80 && cp <= 0x9F)
        return true;
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D)
        return true;
    if (cp == 0xFEFF)
        return true;
    if (cp >= 0x202A && cp <= 0x202E)
        return true;
    if (cp >= 0x2066 && cp <= 0x2069)
        return true;
    if (cp == 0x00AD)
        return true;
    if (cp >= 0xFFF9 && cp <= 0xFFFB)
        return true;
    if (cp >= 0xE0000 && cp <= 0xE007F)
        return true;
    return false;
}

void append_cp(std::string &out, uint32_t cp) {
    if (cp < 0x80)
        out += char(cp);
    else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

bool has_control(std::string_view s) noexcept {
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp;
        if (!decode_cp(s, i, cp))
            return true;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F))
            return true;
    }
    return false;
}

const char *const kReservedNames[] = {
    "admin",   "administrator", "moderator", "system", "server",    "nekki",
    "support", "console",       "root",      "null",   "undefined",
};

}

bool utf8_ok(std::string_view s, size_t *codepoints) noexcept {
    size_t i = 0, n = 0;
    while (i < s.size()) {
        uint32_t cp;
        if (!decode_cp(s, i, cp))
            return false;
        ++n;
    }
    if (codepoints)
        *codepoints = n;
    return true;
}

std::string sanitize_text(std::string_view in, size_t max_codepoints) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0, n = 0;
    bool last_space = true;
    while (i < in.size() && n < max_codepoints) {
        uint32_t cp;
        if (!decode_cp(in, i, cp))
            break;
        if (is_bad_cp(cp))
            continue;
        bool space = (cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x3000);
        if (space) {
            if (last_space)
                continue;
            last_space = true;
            out += ' ';
        } else {
            last_space = false;
            append_cp(out, cp);
        }
        ++n;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

NameResult check_player_name(std::string_view raw, const Config &cfg) {
    NameResult r;
    if (raw.size() > 4 * cfg.max_name_chars + 16) {
        r.reason = "name too long";
        return r;
    }
    if (!utf8_ok(raw)) {
        r.reason = "name not valid utf-8";
        return r;
    }
    if (raw.find('#') != std::string_view::npos) {
        r.reason = "name contains '#'";
        return r;
    }
    if (has_control(raw)) {
        r.reason = "name contains control characters";
        return r;
    }

    std::string s = sanitize_text(raw, cfg.max_name_chars);
    size_t cps = 0;
    utf8_ok(s, &cps);
    if (cps == 0) {
        r.reason = "name empty";
        return r;
    }
    if (cps > cfg.max_name_chars) {
        r.reason = "name too long";
        return r;
    }

    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    for (const char *bad : kReservedNames)
        if (lower == bad) {
            r.reason = "name reserved";
            return r;
        }

    r.ok = true;
    r.value = std::move(s);
    return r;
}

std::string make_tag() {
    static const char kAlpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint8_t rnd[5];
    crypto::random_bytes(rnd, sizeof rnd);
    std::string t(5, 'A');
    for (int i = 0; i < 5; ++i)
        t[size_t(i)] = kAlpha[rnd[i] % 32];
    return t;
}

NameResult check_chat_text(std::string_view raw, const Config &cfg) {
    NameResult r;
    if (raw.size() > 8 * cfg.max_chat_chars + 64) {
        r.reason = "message too long";
        return r;
    }
    if (!utf8_ok(raw)) {
        r.reason = "message not valid utf-8";
        return r;
    }
    if (has_control(raw)) {
        r.reason = "message contains control characters";
        return r;
    }
    std::string s = sanitize_text(raw, cfg.max_chat_chars);
    if (s.empty()) {
        r.reason = "message empty";
        return r;
    }
    r.ok = true;
    r.value = std::move(s);
    return r;
}

bool check_login_uuid(std::string_view s) noexcept {
    if (s.size() != 36)
        return false;
    for (size_t i = 0; i < 36; ++i) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool check_email(std::string_view s) noexcept {
    if (s.empty())
        return true;
    if (s.size() > 254)
        return false;
    size_t at = s.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= s.size())
        return false;
    if (s.find('@', at + 1) != std::string_view::npos)
        return false;
    if (s.find('.', at) == std::string_view::npos)
        return false;
    for (char c : s)
        if (uint8_t(c) < 0x21 || uint8_t(c) > 0x7E)
            return false;
    return true;
}

bool is_identifier(std::string_view s, size_t max_len) noexcept {
    if (s.empty() || s.size() > max_len)
        return false;
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-')
            continue;
        return false;
    }
    return true;
}

InventoryResult check_inventory(std::string_view text, const Config &cfg) {
    InventoryResult r;
    if (text.size() > cfg.max_inventory_bytes) {
        r.reason = "inventory too large";
        return r;
    }
    if (!cfg.accept_client_inventory) {
        r.reason = "client inventory refused by policy";
        return r;
    }

    json::ParseLimits lim;
    lim.max_depth = 8;
    lim.max_items = 4 * cfg.max_inventory_items * cfg.max_perks_per_item + 256;
    lim.max_string = 128;

    json::Value parsed;
    std::string err;
    if (!json::parse(text, parsed, lim, err)) {
        r.reason = "inventory not valid json";
        return r;
    }
    if (!parsed.is_object()) {
        r.reason = "inventory not an object";
        return r;
    }

    const json::Value *v = parsed.find("v");
    const json::Value *lot = parsed.find("lot");
    const json::Value *eq = parsed.find("eq");
    if (v && !v->is_int()) {
        r.reason = "inventory.v not an integer";
        return r;
    }
    if (lot && !lot->is_int()) {
        r.reason = "inventory.lot not an integer";
        return r;
    }
    if (eq && !eq->is_array()) {
        r.reason = "inventory.eq not an array";
        return r;
    }
    if (eq && eq->size() > cfg.max_inventory_items) {
        r.reason = "too many equipped items";
        return r;
    }

    json::Value out = json::Value::make_object();
    out.put("v", json::Value::of(v ? v->i : 2));
    out.put("lot", json::Value::of(lot ? lot->i : 0));

    json::Value eqa = json::Value::make_array();
    if (eq && eq->arr) {
        for (const json::Value &item : *eq->arr) {
            if (!item.is_object()) {
                r.reason = "inventory item not an object";
                return r;
            }
            const json::Value *nm = item.find("Name");
            if (!nm || !nm->is_string() || !is_identifier(nm->s)) {
                r.reason = "bad item name";
                return r;
            }
            json::Value o = json::Value::make_object();
            o.put("Name", json::Value::of(nm->s));

            const json::Value *perks = item.find("Perks");
            if (perks) {
                if (!perks->is_array() || perks->size() > cfg.max_perks_per_item) {
                    r.reason = "bad perk list";
                    return r;
                }
                json::Value pa = json::Value::make_array();
                for (const json::Value &p : *perks->arr) {
                    if (!p.is_string() || !is_identifier(p.s)) {
                        r.reason = "bad perk name";
                        return r;
                    }
                    pa.push(json::Value::of(p.s));
                }
                o.put("Perks", std::move(pa));
            }

            if (cfg.inventory_preserve_unknown && item.obj) {
                size_t extra = 0;
                for (const auto &kv : *item.obj) {
                    if (kv.first == "Name" || kv.first == "Perks")
                        continue;
                    if (++extra > cfg.max_inventory_keys) {
                        r.reason = "too many keys on an equipped item";
                        return r;
                    }
                    o.put(kv.first, kv.second);
                }
            }
            eqa.push(std::move(o));
        }
    }
    out.put("eq", std::move(eqa));

    if (cfg.inventory_preserve_unknown && parsed.obj) {
        size_t extra = 0;
        for (const auto &kv : *parsed.obj) {
            if (kv.first == "v" || kv.first == "lot" || kv.first == "eq")
                continue;
            if (!is_identifier(kv.first, 48)) {
                r.reason = "bad inventory key";
                return r;
            }
            if (++extra > cfg.max_inventory_keys) {
                r.reason = "too many top-level inventory keys";
                return r;
            }
            out.put(kv.first, kv.second);
        }
    }

    r.ok = true;
    r.value = std::move(out);
    return r;
}

}
