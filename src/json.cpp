#include "json.h"
#include <cstdlib>

static void skipws(const std::string& in, size_t& p) {
    while (p < in.size() && (in[p]==' '||in[p]=='\t'||in[p]=='\n'||in[p]=='\r')) p++;
}

static std::string parse_str(const std::string& in, size_t& p) {
    p++;
    std::string r;
    while (p < in.size() && in[p] != '"') {
        if (in[p] == '\\') {
            p++;
            if (p >= in.size()) break;
            switch (in[p]) {
                case '"': r += '"'; break; case '\\': r += '\\'; break;
                case '/': r += '/'; break; case 'b': r += '\b'; break;
                case 'f': r += '\f'; break; case 'n': r += '\n'; break;
                case 'r': r += '\r'; break; case 't': r += '\t'; break;
                default: r += in[p]; break;
            }
        } else r += in[p];
        p++;
    }
    if (p < in.size()) p++;
    return r;
}

static double parse_num(const std::string& in, size_t& p) {
    size_t start = p;
    if (in[p] == '-') p++;
    while (p < in.size() && in[p] >= '0' && in[p] <= '9') p++;
    if (p < in.size() && in[p] == '.') { p++; while (p < in.size() && in[p] >= '0' && in[p] <= '9') p++; }
    if (p < in.size() && (in[p]=='e'||in[p]=='E')) {
        p++; if (p < in.size() && (in[p]=='+'||in[p]=='-')) p++;
        while (p < in.size() && in[p] >= '0' && in[p] <= '9') p++;
    }
    return std::stod(in.substr(start, p-start));
}

static JsonValue parse_val(const std::string& in, size_t& p);

static JsonValue parse_obj(const std::string& in, size_t& p) {
    p++;
    JsonValue v; v.type = JsonValue::T_OBJ;
    skipws(in, p);
    if (p < in.size() && in[p] == '}') { p++; return v; }
    while (true) {
        skipws(in, p); std::string k = parse_str(in, p);
        skipws(in, p); if (p < in.size()) p++;
        v.o.push_back({k, parse_val(in, p)});
        skipws(in, p);
        if (p < in.size() && in[p] == ',') p++;
        else if (p < in.size() && in[p] == '}') { p++; break; }
    }
    return v;
}

static JsonValue parse_arr(const std::string& in, size_t& p) {
    p++;
    JsonValue v; v.type = JsonValue::T_ARR;
    skipws(in, p);
    if (p < in.size() && in[p] == ']') { p++; return v; }
    while (true) {
        v.a.push_back(parse_val(in, p));
        skipws(in, p);
        if (p < in.size() && in[p] == ',') p++;
        else if (p < in.size() && in[p] == ']') { p++; break; }
    }
    return v;
}

static JsonValue parse_val(const std::string& in, size_t& p) {
    skipws(in, p);
    if (p >= in.size()) return JsonValue();
    if (in[p] == '"') { JsonValue v; v.type = JsonValue::T_STR; v.s = parse_str(in, p); return v; }
    if (in[p] == '{') return parse_obj(in, p);
    if (in[p] == '[') return parse_arr(in, p);
    if (in.substr(p,4) == "true")  { p += 4; JsonValue v; v.type = JsonValue::T_BOOL; v.b = true; return v; }
    if (in.substr(p,5) == "false") { p += 5; JsonValue v; v.type = JsonValue::T_BOOL; v.b = false; return v; }
    if (in.substr(p,4) == "null")  { p += 4; return JsonValue(); }
    if (in[p] == '-' || (in[p] >= '0' && in[p] <= '9')) {
        JsonValue v; v.type = JsonValue::T_NUM; v.n = parse_num(in, p); return v;
    }
    return JsonValue();
}

JsonValue parse_json(const std::string& in) {
    size_t p = 0; return parse_val(in, p);
}