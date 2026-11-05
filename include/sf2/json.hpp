

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sf2/bytes.hpp"

namespace sf2::json {

enum class Kind : uint8_t { Null, Bool, Int, Double, String, Array, Object };

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct Value {
    Kind kind = Kind::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0;
    std::string s;
    std::shared_ptr<Array> arr;
    std::shared_ptr<Object> obj;

    static Value make_object() {
        Value v;
        v.kind = Kind::Object;
        v.obj = std::make_shared<Object>();
        return v;
    }

    static Value make_array() {
        Value v;
        v.kind = Kind::Array;
        v.arr = std::make_shared<Array>();
        return v;
    }

    static Value of(int64_t n) {
        Value v;
        v.kind = Kind::Int;
        v.i = n;
        return v;
    }

    static Value of(std::string_view t) {
        Value v;
        v.kind = Kind::String;
        v.s = t;
        return v;
    }

    bool is_object() const noexcept {
        return kind == Kind::Object;
    }

    bool is_array() const noexcept {
        return kind == Kind::Array;
    }

    bool is_string() const noexcept {
        return kind == Kind::String;
    }

    bool is_int() const noexcept {
        return kind == Kind::Int;
    }

    const Value *find(std::string_view key) const noexcept {
        if (kind != Kind::Object || !obj)
            return nullptr;
        for (const auto &kv : *obj)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }

    size_t size() const noexcept {
        if (kind == Kind::Array && arr)
            return arr->size();
        if (kind == Kind::Object && obj)
            return obj->size();
        return 0;
    }

    void push(Value v) {
        if (arr)
            arr->push_back(std::move(v));
    }

    void put(std::string k, Value v) {
        if (obj)
            obj->emplace_back(std::move(k), std::move(v));
    }
};

struct ParseLimits {
    size_t max_depth = 16;
    size_t max_items = 20000;
    size_t max_string = 64 * 1024;
};

bool parse(std::string_view text, Value &out, const ParseLimits &lim, std::string &err);

void dump_to(std::string &out, const Value &v);
std::string dump(const Value &v);

}
