#include "config.h"
#include "json.h"

#include <iostream>
#include <fstream>
#include <sstream>

std::vector<PortConfig> parseConfig(const std::string& json) {
    JsonValue root = parse_json(json);

    if (!root.is_obj()) { std::cerr << "错误: 配置格式无效\n"; return {}; }
    auto* ports = root.get("ports");
    if (!ports || !ports->is_arr()) { std::cerr << "错误: 缺少 ports 数组\n"; return {}; }

    std::vector<PortConfig> cfgs;
    for (size_t i = 0; i < ports->a.size(); i++) {
        auto* entry = ports->idx(i);
        if (!entry || !entry->is_obj()) continue;

        auto* l = entry->get("listen");
        auto* c = entry->get("command");
        if (!l || !c) {
            std::cerr << "警告: 端口[" << i << "] 配置不完整，跳过\n"; continue;
        }

        PortConfig cfg;
        cfg.name = entry->get("name") ? entry->get("name")->as_str() : "";
        if (auto* e = entry->get("enabled")) cfg.enabled = e->as_bool();
        if (!cfg.enabled) {
            std::string label = cfg.name.empty() ? l->as_str() : cfg.name;
            std::cout << "  " << label << " 已禁用，跳过\n";
            continue;
        }
        cfg.listenAddr = l->as_str();
        cfg.command = c->as_str();
        if (auto* d = entry->get("delay")) cfg.delayMs = (int)d->as_num();
        if (auto* r = entry->get("refresh_seconds")) cfg.refreshSeconds = (int)r->as_num();
        if (auto* r = entry->get("auto_restart")) cfg.autoRestart = r->as_bool();

        cfgs.push_back(cfg);
    }
    return cfgs;
}

std::vector<PortConfig> loadConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "错误: 无法打开 " << path << std::endl;
        return {};
    }
    std::stringstream ss; ss << f.rdbuf();
    return parseConfig(ss.str());
}