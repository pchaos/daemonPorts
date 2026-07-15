#ifndef GATEKEEPER_JSON_H
#define GATEKEEPER_JSON_H

#include <string>
#include <vector>
#include <utility>

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    enum Type { T_NULL, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ } type = T_NULL;
    bool    b = false;
    double  n = 0;
    std::string s;
    JsonArray  a;
    JsonObject o;

    bool is_obj() const { return type == T_OBJ; }
    bool is_arr() const { return type == T_ARR; }
    bool is_str() const { return type == T_STR; }

    const JsonValue* get(const std::string& key) const {
        for (auto& p : o) if (p.first == key) return &p.second;
        return nullptr;
    }
    const JsonValue* idx(size_t i) const {
        return i < a.size() ? &a[i] : nullptr;
    }
    std::string as_str() const { return s; }
    bool as_bool() const { return b; }
    double as_num() const { return n; }
};

JsonValue parse_json(const std::string& in);

#endif // GATEKEEPER_JSON_H