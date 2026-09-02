#include "config.h"
#include "json.h"

#include <iostream>
#include <fstream>
#include <sstream>

ControlConfig g_controlConfig;
SystemMonitorConfig g_sysMonConfig;

// 启动时解析一次顶层 system_monitor 段（热加载不触碰）。
// 与 control 段语义一致：采样器在启动时按此配置启停。
void parseSystemMonitorConfig(const std::string& json) {
    SystemMonitorConfig cfg;   // 默认值起步，缺省键保持默认
    JsonValue root = parse_json(json);
    if (!root.is_obj()) return;
    auto* sm = root.get("system_monitor");
    if (!sm || !sm->is_obj()) { g_sysMonConfig = cfg; return; }

    if (auto* e = sm->get("enabled")) cfg.enabled = e->as_bool();
    if (auto* iv = sm->get("interval_seconds")) cfg.intervalSeconds = (int)iv->as_num();
    if (auto* fi = sm->get("fast_interval_seconds")) cfg.fastIntervalSeconds = (int)fi->as_num();
    if (auto* mh = sm->get("memory_high_threshold")) cfg.memoryHighThreshold = mh->as_num();
    if (auto* sh = sm->get("swap_high_threshold")) cfg.swapHighThreshold = sh->as_num();

    // 应急免密名单（默认 reboot/shutdown）
    if (auto* cmds = sm->get("emergency_commands")) {
        if (cmds->is_arr()) {
            std::vector<std::string> list;
            for (size_t i = 0; i < cmds->a.size(); ++i) {
                auto* c = cmds->idx(i);
                if (c && c->is_str() && !c->as_str().empty()) list.push_back(c->as_str());
            }
            if (!list.empty()) cfg.emergencyCommands = std::move(list);
        }
    }

    // 驱逐配置
    if (auto* ev = sm->get("eviction")) {
        if (ev->is_obj()) {
            EvictionConfig ec;
            if (auto* e = ev->get("enabled")) ec.enabled = e->as_bool();
            if (auto* mc = ev->get("memory_critical")) ec.memoryCritical = mc->as_num();
            if (auto* sc = ev->get("swap_critical")) ec.swapCritical = sc->as_num();
            if (auto* ss = ev->get("sustain_seconds")) ec.sustainSeconds = (int)ss->as_num();
            // 钳制阈值到 (0,1)
            if (ec.memoryCritical <= 0.0 || ec.memoryCritical > 1.0) ec.memoryCritical = 0.90;
            if (ec.swapCritical <= 0.0 || ec.swapCritical > 1.0) ec.swapCritical = 0.90;
            if (ec.sustainSeconds < 1) ec.sustainSeconds = 900;
            cfg.eviction = ec;
        }
    }
    // 钳制阈值
    if (cfg.intervalSeconds < 1) cfg.intervalSeconds = 300;
    if (cfg.fastIntervalSeconds < 1) cfg.fastIntervalSeconds = 60;
    if (cfg.memoryHighThreshold <= 0.0 || cfg.memoryHighThreshold > 1.0) cfg.memoryHighThreshold = 0.66;
    if (cfg.swapHighThreshold <= 0.0 || cfg.swapHighThreshold > 1.0) cfg.swapHighThreshold = 0.5;

    g_sysMonConfig = cfg;
}

std::vector<PortConfig> parseConfig(const std::string& json) {
    JsonValue root = parse_json(json);

    if (!root.is_obj()) { std::cerr << "错误: 配置格式无效\n"; return {}; }
    // Parse top-level control block if present
    if (auto* ctrl = root.get("control")) {
        if (ctrl->is_obj()) {
            if (auto* l = ctrl->get("listen")) g_controlConfig.listen = l->as_str();
            if (auto* a = ctrl->get("auth")) {
                if (a->is_obj()) {
                    if (auto* t = a->get("type")) g_controlConfig.auth.type = t->as_str();
                    if (auto* tk = a->get("token")) g_controlConfig.auth.token = tk->as_str();
                    if (auto* mc = ctrl->get("max_connections")) g_controlConfig.maxConnections = (int)mc->as_num();
                    if (auto* rs = ctrl->get("rate_limit_seconds")) g_controlConfig.rateLimitSeconds = (int)rs->as_num();
                }
            }
            if (auto* p = ctrl->get("pin")) g_controlConfig.pin = p->as_str();
            if (auto* cmds = ctrl->get("commands")) {
                if (cmds->is_arr()) {
                    for (size_t i = 0; i < cmds->a.size(); i++) {
                        auto* c = cmds->idx(i);
                        if (!c || !c->is_obj()) continue;
                        CommandConfig cc;
                        if (auto* n = c->get("name")) cc.name = n->as_str();
                        if (auto* cmd = c->get("command")) cc.command = cmd->as_str();
                        if (!cc.name.empty() && !cc.command.empty())
                            g_controlConfig.commands.push_back(std::move(cc));
                    }
                }
            }
        }
    }
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
        if (!l || (!c && !(mode == "mixed" && holdPort) && mode != "proxy")) {
            std::cerr << "警告: 端口[" << i << "] 配置不完整，跳过\n"; continue;
        }

        PortConfig cfg;
        cfg.name = entry->get("name") ? entry->get("name")->as_str() : "";
        if (auto* g = entry->get("group")) cfg.groupName = g->as_str();
        if (auto* e = entry->get("enabled")) cfg.enabled = e->as_bool();
        if (!cfg.enabled) {
            std::string label = cfg.name.empty() ? l->as_str() : cfg.name;
            std::cout << "  " << label << " 已禁用，跳过\n";
            continue;
        }
        cfg.listenAddr = l->as_str();
        cfg.command = c ? c->as_str() : "";
        if (auto* sc = entry->get("stop_command")) cfg.stopCommand = sc->as_str();
        if (auto* im = entry->get("idle_minutes")) cfg.idleMinutes = (int)im->as_num();
        if (auto* d = entry->get("delay")) cfg.delayMs = (int)d->as_num();
        if (auto* r = entry->get("refresh_seconds")) cfg.refreshSeconds = (int)r->as_num();
        if (auto* rt = entry->get("retry_seconds")) cfg.retrySeconds = (int)rt->as_num();
        if (auto* mr = entry->get("max_retry_seconds")) cfg.maxRetrySeconds = (int)mr->as_num();
        if (auto* r = entry->get("auto_restart")) cfg.autoRestart = r->as_bool();
        if (auto* los = entry->get("launch_on_start")) cfg.launchOnStart = los->as_bool();
        if (auto* s = entry->get("stack_size")) cfg.stackSize = (int)s->as_num();

        // 混合模式字段
        if (auto* m = entry->get("mode")) cfg.mode = m->as_str();
        if (auto* h = entry->get("hold_port")) cfg.holdPort = h->as_bool();

        // proxy 模式字段：auth 配置
        if (auto* a = entry->get("auth")) {
            if (a->is_obj()) {
                if (auto* t = a->get("type")) cfg.auth.type = t->as_str();
                if (auto* u = a->get("username")) cfg.auth.username = u->as_str();
                if (auto* p = a->get("password")) cfg.auth.password = p->as_str();
            }
        }
        // proxy 模式字段：HTTP 转发目标
        if (auto* ht = entry->get("http_target")) cfg.httpTarget = ht->as_str();

        // TCP 监控配置
        if (auto* mon = entry->get("monitor")) {
            if (mon->is_obj()) {
                if (auto* e = mon->get("enabled")) cfg.monitor.enabled = e->as_bool();
                if (auto* iv = mon->get("interval_seconds")) cfg.monitor.intervalSec = (int)iv->as_num();
                if (auto* ld = mon->get("log_dedup")) cfg.monitor.logDedup = ld->as_str();
            }
        }

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