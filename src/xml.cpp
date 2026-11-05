
#include "sf2/xml.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sf2::xml {
namespace {

inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool is_name_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
           uint8_t(c) >= 0x80;
}

inline bool is_name_char(char c) noexcept {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
        return;
    }
    if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
        return;
    }
    if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
        return;
    }
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
}

void decode_entities(std::string_view in, std::string &out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out += in[i++];
            continue;
        }
        size_t semi = in.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out += in[i++];
            continue;
        }
        std::string_view ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp")
            out += '&';
        else if (ent == "lt")
            out += '<';
        else if (ent == "gt")
            out += '>';
        else if (ent == "quot")
            out += '"';
        else if (ent == "apos")
            out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            std::string digits(ent.substr(hex ? 2 : 1));
            char *end = nullptr;
            unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
            if (end && !*end && cp && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF))
                append_utf8(out, uint32_t(cp));
            else {
                out += in.substr(i, semi - i + 1);
            }
        } else {
            out += in.substr(i, semi - i + 1);
        }
        i = semi + 1;
    }
}

}

bool Doc::parse(std::string_view s, std::string &err, const Limits &lim) {
    nodes_.clear();
    if (s.size() > lim.max_bytes) {
        err = "document too large";
        return false;
    }

    if (s.size() >= 3 && uint8_t(s[0]) == 0xEF && uint8_t(s[1]) == 0xBB && uint8_t(s[2]) == 0xBF)
        s.remove_prefix(3);

    std::vector<uint32_t> stack;
    std::string scratch;
    size_t i = 0;
    bool have_root = false;

    auto fail = [&](const char *what) {
        std::ostringstream o;
        o << what << " at byte " << i;
        err = o.str();
        nodes_.clear();
        return false;
    };

    while (i < s.size()) {
        if (s[i] != '<') {
            size_t start = i;
            while (i < s.size() && s[i] != '<')
                ++i;
            if (!stack.empty()) {
                std::string_view raw = s.substr(start, i - start);

                bool blank = true;
                for (char c : raw)
                    if (!is_space(c)) {
                        blank = false;
                        break;
                    }
                if (!blank) {
                    decode_entities(raw, scratch);
                    std::string &t = nodes_[stack.back()].text;
                    if (t.size() + scratch.size() > lim.max_text)
                        return fail("text node too large");
                    t += scratch;
                }
            }
            continue;
        }

        if (s.compare(i, 4, "<!--") == 0) {
            size_t e = s.find("-->", i + 4);
            if (e == std::string_view::npos)
                return fail("unterminated comment");
            i = e + 3;
            continue;
        }
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            size_t e = s.find("]]>", i + 9);
            if (e == std::string_view::npos)
                return fail("unterminated CDATA");
            if (!stack.empty()) {
                std::string &t = nodes_[stack.back()].text;
                std::string_view raw = s.substr(i + 9, e - i - 9);
                if (t.size() + raw.size() > lim.max_text)
                    return fail("text node too large");
                t.append(raw);
            }
            i = e + 3;
            continue;
        }
        if (s.compare(i, 2, "<?") == 0) {
            size_t e = s.find("?>", i + 2);
            if (e == std::string_view::npos)
                return fail("unterminated PI");
            i = e + 2;
            continue;
        }
        if (s.compare(i, 2, "<!") == 0) {

            size_t depth = 0;
            size_t j = i;
            while (j < s.size()) {
                if (s[j] == '<')
                    ++depth;
                else if (s[j] == '>') {
                    if (--depth == 0) {
                        ++j;
                        break;
                    }
                }
                ++j;
            }
            if (j > s.size())
                return fail("unterminated declaration");
            i = j;
            continue;
        }

        if (s.compare(i, 2, "</") == 0) {
            i += 2;
            size_t start = i;
            while (i < s.size() && is_name_char(s[i]))
                ++i;
            std::string_view name = s.substr(start, i - start);
            while (i < s.size() && is_space(s[i]))
                ++i;
            if (i >= s.size() || s[i] != '>')
                return fail("malformed closing tag");
            ++i;
            if (stack.empty())
                return fail("closing tag without an open element");
            if (nodes_[stack.back()].name != name)
                return fail("mismatched closing tag");
            stack.pop_back();
            continue;
        }

        ++i;
        size_t start = i;
        if (i >= s.size() || !is_name_start(s[i]))
            return fail("malformed tag name");
        while (i < s.size() && is_name_char(s[i]))
            ++i;
        std::string_view name = s.substr(start, i - start);
        if (name.size() > lim.max_name)
            return fail("tag name too long");

        if (nodes_.size() >= lim.max_nodes)
            return fail("too many elements");
        if (stack.size() >= lim.max_depth)
            return fail("nesting too deep");
        if (stack.empty() && have_root)
            return fail("more than one root element");

        const uint32_t idx = uint32_t(nodes_.size());
        nodes_.emplace_back();
        nodes_[idx].name.assign(name);
        if (!stack.empty())
            nodes_[stack.back()].kids.push_back(idx);
        else
            have_root = true;

        for (;;) {
            while (i < s.size() && is_space(s[i]))
                ++i;
            if (i >= s.size())
                return fail("unterminated tag");
            if (s[i] == '/') {
                if (i + 1 >= s.size() || s[i + 1] != '>')
                    return fail("malformed empty tag");
                i += 2;
                break;
            }
            if (s[i] == '>') {
                ++i;
                stack.push_back(idx);
                break;
            }

            size_t astart = i;
            if (!is_name_start(s[i]))
                return fail("malformed attribute name");
            while (i < s.size() && is_name_char(s[i]))
                ++i;
            std::string_view akey = s.substr(astart, i - astart);
            while (i < s.size() && is_space(s[i]))
                ++i;
            if (i >= s.size() || s[i] != '=')
                return fail("attribute without a value");
            ++i;
            while (i < s.size() && is_space(s[i]))
                ++i;
            if (i >= s.size() || (s[i] != '"' && s[i] != '\''))
                return fail("unquoted attribute");
            const char quote = s[i++];
            size_t vstart = i;
            while (i < s.size() && s[i] != quote)
                ++i;
            if (i >= s.size())
                return fail("unterminated attribute value");
            std::string_view aval = s.substr(vstart, i - vstart);
            ++i;

            Node &n = nodes_[idx];
            if (n.attrs.size() >= lim.max_attrs)
                return fail("too many attributes");
            if (akey.size() > lim.max_name)
                return fail("attribute name too long");
            decode_entities(aval, scratch);
            n.attrs.push_back(Attr{std::string(akey), scratch});
        }
    }

    if (!stack.empty()) {
        err = "unclosed element";
        nodes_.clear();
        return false;
    }
    if (nodes_.empty()) {
        err = "no root element";
        return false;
    }
    return true;
}

bool Doc::load(const std::string &path, std::string &err, const Limits &lim) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    const std::string text = buf.str();
    if (!parse(text, err, lim)) {
        err = path + ": " + err;
        return false;
    }
    return true;
}

const Node *Doc::child(const Node &n, std::string_view name) const noexcept {
    for (uint32_t k : n.kids)
        if (nodes_[k].name == name)
            return &nodes_[k];
    return nullptr;
}

const Node *Doc::path(const Node &n, std::string_view p) const noexcept {
    const Node *cur = &n;
    size_t at = 0;
    while (at <= p.size()) {
        size_t slash = p.find('/', at);
        std::string_view part =
            p.substr(at, slash == std::string_view::npos ? p.size() - at : slash - at);
        if (part.empty())
            break;
        cur = child(*cur, part);
        if (!cur)
            return nullptr;
        if (slash == std::string_view::npos)
            break;
        at = slash + 1;
    }
    return cur;
}

std::vector<const Node *> Doc::children(const Node &n, std::string_view name) const {
    std::vector<const Node *> out;
    out.reserve(n.kids.size());
    for (uint32_t k : n.kids)
        if (name.empty() || nodes_[k].name == name)
            out.push_back(&nodes_[k]);
    return out;
}

std::string_view Doc::attr(const Node &n, std::string_view key, std::string_view dflt) noexcept {
    for (const Attr &a : n.attrs)
        if (a.name == key)
            return a.value;
    return dflt;
}

bool Doc::has_attr(const Node &n, std::string_view key) noexcept {
    for (const Attr &a : n.attrs)
        if (a.name == key)
            return true;
    return false;
}

int64_t Doc::attr_i(const Node &n, std::string_view key, int64_t dflt) noexcept {
    std::string_view v = attr(n, key);
    if (v.empty())
        return dflt;
    std::string tmp(v);
    char *end = nullptr;
    long long r = std::strtoll(tmp.c_str(), &end, 10);
    return (end && *end == 0) ? int64_t(r) : dflt;
}

double Doc::attr_d(const Node &n, std::string_view key, double dflt) noexcept {
    std::string_view v = attr(n, key);
    if (v.empty())
        return dflt;
    std::string tmp(v);
    char *end = nullptr;
    double r = std::strtod(tmp.c_str(), &end);
    return (end && *end == 0) ? r : dflt;
}

bool Doc::attr_b(const Node &n, std::string_view key, bool dflt) noexcept {
    std::string_view v = attr(n, key);
    if (v.empty())
        return dflt;
    return v == "1" || v == "true" || v == "True" || v == "TRUE" || v == "yes";
}

}
