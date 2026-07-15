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
        auto* m = entry->get("mode");
        std::string mode = m ? m->as_str() : "simple";
        auto* h = entry->get("hold_port");
        bool holdPort = h ? h->as_bool() : false;

        // simple 模式 / mixed+hold_port=false：必须提供 command
        // mixed+hold_port=true：command 可选，每个 protocol 自带 command
        if (!l || (!c && !(mode == "mixed" && holdPort))) {
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
        cfg.command = c ? c->as_str() : "";
        if (auto* d = entry->get("delay")) cfg.delayMs = (int)d->as_num();
        if (auto* r = entry->get("refresh_seconds")) cfg.refreshSeconds = (int)r->as_num();
        if (auto* r = entry->get("auto_restart")) cfg.autoRestart = r->as_bool();
        if (auto* s = entry->get("stack_size")) cfg.stackSize = (int)s->as_num();

        // 混合模式字段
        if (auto* m = entry->get("mode")) cfg.mode = m->as_str();
        if (auto* h = entry->get("hold_port")) cfg.holdPort = h->as_bool();

        // 解析 protocols 数组
        if (auto* protos = entry->get("protocols")) {
            if (protos->is_arr()) {
                for (size_t j = 0; j < protos->a.size(); j++) {
                    auto* p = protos->idx(j);
                    if (!p) continue;
                    if (p->is_str()) {
                        // 简写形式: "http", "socks5"
                        ProtocolConfig pc;
                        pc.type = p->as_str();
                        cfg.protocols.push_back(pc);
                    } else if (p->is_obj()) {
                        ProtocolConfig pc;
                        if (auto* t = p->get("type")) pc.type = t->as_str();
                        if (auto* c2 = p->get("command")) pc.command = c2->as_str();
                        if (auto* p2 = p->get("proxy_to")) pc.proxyTo = p2->as_str();
                        if (auto* d = p->get("delay")) pc.delayMs = (int)d->as_num();
                        if (auto* e2 = p->get("enabled")) pc.enabled = e2->as_bool();
                        if (pc.enabled && !pc.type.empty())
                            cfg.protocols.push_back(pc);
                    }
                }
            }
        }

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