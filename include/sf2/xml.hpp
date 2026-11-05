

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sf2::xml {

struct Attr {
    std::string name;
    std::string value;
};

struct Node {
    std::string name;
    std::vector<Attr> attrs;
    std::vector<uint32_t> kids;
    std::string text;
};

struct Limits {
    size_t max_bytes = 64u * 1024 * 1024;
    size_t max_nodes = 2'000'000;
    size_t max_depth = 64;
    size_t max_name = 128;
    size_t max_attrs = 64;
    size_t max_text = 1u * 1024 * 1024;
};

class Doc {
  public:
    bool parse(std::string_view src, std::string &err, const Limits &lim = {});
    bool load(const std::string &path, std::string &err, const Limits &lim = {});

    bool empty() const noexcept {
        return nodes_.empty();
    }

    const Node *root() const noexcept {
        return nodes_.empty() ? nullptr : &nodes_[0];
    }

    const Node *child(const Node &n, std::string_view name) const noexcept;

    const Node *path(const Node &n, std::string_view path) const noexcept;

    std::vector<const Node *> children(const Node &n, std::string_view name = {}) const;

    size_t size() const noexcept {
        return nodes_.size();
    }

    static std::string_view attr(const Node &n, std::string_view key,
                                 std::string_view dflt = {}) noexcept;
    static bool has_attr(const Node &n, std::string_view key) noexcept;
    static int64_t attr_i(const Node &n, std::string_view key, int64_t dflt = 0) noexcept;
    static double attr_d(const Node &n, std::string_view key, double dflt = 0) noexcept;
    static bool attr_b(const Node &n, std::string_view key, bool dflt = false) noexcept;

  private:
    std::vector<Node> nodes_;
};

}
