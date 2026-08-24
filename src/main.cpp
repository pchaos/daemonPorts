#include "config.h"
#include "relay.h"
#include "tcp_monitor.h"
#include "port_group.h"
#include "control_server.h"
#include <map>
#include <mutex>
#include <algorithm>
#include <random>
#include <chrono>
#include <iomanip>


#include <iostream>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <sstream>
#ifndef _WIN32
#include <netinet/tcp.h>
#include <signal.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#endif
// TCP 状态常量 — macOS 的 <netinet/tcp.h> 不包含 tcp_fsm.h
#ifndef TCP_LISTEN
#define TCP_LISTEN 0x0A
#endif
#ifndef TCP_ESTABLISHED
#define TCP_ESTABLISHED 0x01
#endif

static std::mutex g_groupsMutex;
static std::vector<std::unique_ptr<PortGroup>> g_groups;

// systemd sd_notify 支持（编译时添加 -DHAVE_SYSTEMD 启用）
#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#define NOTIFY_DID_SEND() (sd_notify(0, "READY=1") > 0)
#else
#define NOTIFY_DID_SEND() false
#endif

std::mutex g_relaysMutex;
std::vector<std::unique_ptr<PortRelay>> g_relays;
static std::atomic<bool> g_stop{false};
static std::mutex g_pendingMutex;
static std::vector<std::unique_ptr<PortRelay>> g_pendingRemoval;
static std::string g_configPath;
static std::vector<PortConfig> g_currentCfgs;
static std::atomic<bool> g_reloadInProgress{false};

// Signal flags — async-signal-safe, only set here
static std::atomic<bool> g_shutdownRequested{false};

static void handleSignal(int) {
    g_stop.store(true);
    g_shutdownRequested.store(true);
}

#ifdef _WIN32
static BOOL WINAPI ctrlHandler(DWORD) {
    g_stop.store(true);
    for (auto& g : g_groups) {
        if (g) g->signalStop();
    }
    for (auto& r : g_relays) r->signalStop();
    return TRUE;
}
#endif

// 统一 TCP 连接监控线程：轮询所有端口的连接状态并更新活跃时间戳
static int gcd(int a, int b) {
    while (b) { int t = b; b = a % b; a = t; }
    return a;
}
static void monitorLoop() {
    // 计算所有端口间隔的最大公约数作为睡眠间隔
    int interval = 0;
    for (auto& r : g_relays) {
        if (r->monitorEnabled()) {
            interval = (interval == 0) ? r->monitorIntervalSec() : gcd(interval, r->monitorIntervalSec());
        }
    }

    std::cout << "TCP 连接监控已启动，各端口独立轮询间隔：" << std::endl;
    for (auto& r : g_relays) {
        if (r->monitorEnabled()) {
            std::cout << "  [" << r->name() << "] " << r->monitorIntervalSec() << " 秒" << " (tick=" << interval << "s)" << std::endl;
        }
    }

    while (!g_stop.load()) {
        time_t now = time(nullptr);
        // Monitor active connections
        {
            std::lock_guard<std::mutex> lock(g_relaysMutex);
            for (auto& r : g_relays) {
                if (!r->monitorEnabled()) continue;
                time_t elapsed = now - r->lastSampleTime_;
                if (elapsed < r->monitorIntervalSec()) continue;
                r->lastSampleTime_ = now;
                int port = r->monitorPort();
                if (port <= 0) continue;

                TcpSnapshot cur = queryPortConnections(port);
                int nonListen = 0;
                for (auto& e : cur.entries) {
                    if (e.state != TCP_LISTEN) nonListen++;
                }
                bool active = nonListen > 0;
                r->updateActivity(active);

                bool shouldPrint = true;
                const std::string& dedup = r->logDedupMode();
                if (dedup == "off") {
                    // always print
                } else if (dedup == "throttle") {
                    if (nonListen == r->lastNonListen_) {
                        if (++r->logDedupCounter() < 5) shouldPrint = false;
                        else r->logDedupCounter() = 0;
                    } else {
                        r->logDedupCounter() = 0;
                    }
                } else {
                    if (nonListen == r->lastNonListen_) shouldPrint = false;
                }
                if (!shouldPrint) continue;
                r->lastNonListen_ = nonListen;

                std::cout << "  [" << r->name() << "] ACTIVE=" << (active ? "1" : "0")
                          << "  connections=" << cur.entries.size()
                          << "  non-listen=" << nonListen
                          << (active ? "" : " (idle)")
                          << std::endl;
            }
        }
        // Idle detection and move to pending removal
        {
            std::lock_guard<std::mutex> lock(g_relaysMutex);
            for (auto it = g_relays.begin(); it != g_relays.end(); ) {
                auto& r = *it;
                if (!r->monitorEnabled()) { ++it; continue; }
                if (!r->isBackendRunning()) { ++it; continue; }
                int idleMin = r->idleMinutes();
                if (r->hasRecentActivity(idleMin)) { ++it; continue; }
                std::cout << "  [" << r->name() << "] 已空闲 " << idleMin << " 分钟，关闭后端" << std::endl;
                if (auto* g = r->group()) g->resetLaunch();
                r->setPendingRemoval();
                {
                    std::lock_guard<std::mutex> plock(g_pendingMutex);
                    g_pendingRemoval.push_back(std::move(r));
                }
                it = g_relays.erase(it);
            }
        }
        // Process pending removal list
        {
            std::lock_guard<std::mutex> plock(g_pendingMutex);
            for (auto it = g_pendingRemoval.begin(); it != g_pendingRemoval.end(); ) {
                auto& relay = *it;
                if (!relay->isBackendRunning()) {
                    if (relay->autoRestart()) {
                        relay->resetForIdle();
                        std::cout << "  [" << relay->name() << "] 后端已退出，重新监听端口" << std::endl;
                        std::lock_guard<std::mutex> lock(g_relaysMutex);
                        g_relays.push_back(std::move(*it));
                        it = g_pendingRemoval.erase(it);
                    } else {
                        std::cout << "  [" << relay->name() << "] 待清理后端已退出，移除" << std::endl;
                        it = g_pendingRemoval.erase(it);
                    }
                } else if (relay->monitorEnabled()) {
                    if (!relay->hasRecentActivity(relay->idleMinutes())) {
                        std::cout << "  [" << relay->name() << "] 待清理后端已空闲 " << relay->idleMinutes() << " 分钟，正在关闭" << std::endl;
                        relay->gracefulStop();
                    }
                    ++it;
                } else {
                    ++it;
                }
            }
        }
        // Sleep interval
        for (int i = 0; i < interval && !g_stop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

static void printHelp() {
    std::cout << "=== 用法 ===\n"
              << "Usage: ./gatekeeper [options] <config.json>\n"
              << "       ./gatekeeper -                        # 从 stdin 读取配置\n\n"
              << "=== 选项 ===\n"
              << "  -h, --help      显示此帮助信息\n"
              << "  --version       显示版本号\n\n"
              << "=== 工作模式 ===\n"
              << "  simple  (默认)  引导后释放端口，后端直接接管。适合单端口单后端场景。\n"
              << "  mixed           协议感知引导或常驻端口做协议路由。适合多协议聚合场景。\n"
              << "  proxy           SOCKS5 代理服务器。适合将 gatekeeper 作为 SOCKS5 代理使用。\n\n"
              << "=== 配置示例（simple 模式）===\n"
              << R"({
  "ports": [
    {
      "listen": ":3000",
      "command": "./web-app --port 3000"
    }
  ]
})" << "\n\n"
              << "=== 配置示例（mixed 模式）===\n"
              << R"({
  "ports": [
    {
      "listen": ":3128",
      "mode": "mixed",
      "command": "./sing-box run",
      "protocols": ["http", "socks5", "socks4"]
    }
  ]
})" << "\n\n"
              << "=== 配置示例（proxy 模式）===\n"
              << R"({
  "ports": [
    {
      "listen": ":1080",
      "mode": "proxy",
      "http_target": "127.0.0.1:8080"
    }
  ]
})" << "\n\n"
              << "=== 特殊用法 ===\n"
              << "  ./gatekeeper -   从标准输入读取配置，适用于动态生成配置\n";
}

static void printVersion() {
    std::cout << "gatekeeper v" << GATEKEEPER_VERSION << std::endl;
}

struct ReloadSummary {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> modified;
    bool success = true;
};

bool configsEqual(const PortConfig& a, const PortConfig& b) {
    return a.listenAddr == b.listenAddr
        && a.command == b.command
        && a.mode == b.mode
        && a.holdPort == b.holdPort
        && a.delayMs == b.delayMs
        && a.autoRestart == b.autoRestart
        && a.enabled == b.enabled
        && a.groupName == b.groupName
        && a.stopCommand == b.stopCommand
        && a.idleMinutes == b.idleMinutes
        && a.refreshSeconds == b.refreshSeconds
        && a.retrySeconds == b.retrySeconds
        && a.maxRetrySeconds == b.maxRetrySeconds
        && a.stackSize == b.stackSize
        && a.httpTarget == b.httpTarget
        && a.monitor.enabled == b.monitor.enabled
        && a.monitor.intervalSec == b.monitor.intervalSec
        && a.monitor.logDedup == b.monitor.logDedup
        && a.launchOnStart == b.launchOnStart;
}

ReloadSummary reloadConfig(const std::vector<PortConfig>& newCfgs) {
    ReloadSummary summary;
    if (g_reloadInProgress.exchange(true)) {
        std::cerr << "错误: 配置热加载正在进行中，拒绝新请求" << std::endl;
        summary.success = false;
        return summary;
    }
    std::map<std::string, const PortConfig*> oldMap;
    for (auto& c : g_currentCfgs) oldMap[c.listenAddr] = &c;
    std::map<std::string, const PortConfig*> newMap;
    for (auto& c : newCfgs) newMap[c.listenAddr] = &c;
    std::vector<PortConfig> toAdd;
    std::vector<std::string> toRemove;
    std::vector<PortConfig> toModify;
    std::vector<std::string> toModifyOld;
    for (auto it = newMap.begin(); it != newMap.end(); ++it) {
        const std::string& addr = it->first;
        const PortConfig* cfg = it->second;
        auto oldIt = oldMap.find(addr);
        if (oldIt == oldMap.end()) { toAdd.push_back(*cfg); summary.added.push_back(addr); }
        else if (!configsEqual(*cfg, *oldIt->second)) { toModify.push_back(*cfg); toModifyOld.push_back(addr); summary.modified.push_back(addr); }
    }
    for (auto it = oldMap.begin(); it != oldMap.end(); ++it) {
        if (newMap.find(it->first) == newMap.end()) { toRemove.push_back(it->first); summary.removed.push_back(it->first); }
    }
    // Remove old ports
    if (!toRemove.empty()) {
        std::lock_guard<std::mutex> lock(g_relaysMutex);
        for (auto it = g_relays.begin(); it != g_relays.end(); ) {
            if (std::find(toRemove.begin(), toRemove.end(), (*it)->name()) != toRemove.end()) {
                (*it)->setPendingRemoval();
                { std::lock_guard<std::mutex> plock(g_pendingMutex); g_pendingRemoval.push_back(std::move(*it)); }
                it = g_relays.erase(it);
            } else { ++it; }
        }
    }
    // Modify old ports (move to pending removal)
    if (!toModifyOld.empty()) {
        std::lock_guard<std::mutex> lock(g_relaysMutex);
        for (auto it = g_relays.begin(); it != g_relays.end(); ) {
            if (std::find(toModifyOld.begin(), toModifyOld.end(), (*it)->name()) != toModifyOld.end()) {
                (*it)->setPendingRemoval();
                { std::lock_guard<std::mutex> plock(g_pendingMutex); g_pendingRemoval.push_back(std::move(*it)); }
                it = g_relays.erase(it);
            } else { ++it; }
        }
    }
    // Add new and modified ports
    if (!toAdd.empty() || !toModify.empty()) {
        std::lock_guard<std::mutex> lock(g_relaysMutex);
        for (auto& cfg : toAdd) { auto relay = std::unique_ptr<PortRelay>(new PortRelay(cfg)); relay->start(); g_relays.push_back(std::move(relay)); }
        for (auto& cfg : toModify) { auto relay = std::unique_ptr<PortRelay>(new PortRelay(cfg)); relay->start(); g_relays.push_back(std::move(relay)); }
    }
    g_currentCfgs = newCfgs;
    std::cout << "配置热加载完成: +" << summary.added.size() << " / -" << summary.removed.size() << " / ~" << summary.modified.size() << " (添加/删除/修改)" << std::endl;
    for (auto& name : summary.added) std::cout << "  + " << name << std::endl;
    for (auto& name : summary.removed) std::cout << "  - " << name << std::endl;
    for (auto& name : summary.modified) std::cout << "  ~ " << name << std::endl;
    g_reloadInProgress = false;
    return summary;
}

ReloadSummary reloadFromFile() {
    auto newCfgs = loadConfig(g_configPath);
    if (newCfgs.empty()) {
        std::cerr << "错误: 热加载失败，新配置无效，保留当前配置" << std::endl;
        ReloadSummary s; s.success = false; return s;
    }
    return reloadConfig(newCfgs);
}

ReloadSummary reloadFromJson(const std::string& json) {
    auto newCfgs = parseConfig(json);
    if (newCfgs.empty()) {
        std::cerr << "错误: 热加载失败，新配置无效，保留当前配置" << std::endl;
        ReloadSummary s; s.success = false; return s;
    }
    return reloadConfig(newCfgs);
}

int main(int argc, char* argv[]) {
    // 先处理 --help/-h/--version（不需要配置文件）
    if (argc >= 2) {
        if (strcmp(argv[1], "--version") == 0) {
            printVersion();
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printHelp();
            return 0;
        }
    }

    // Determine config path and parse CLI control port option
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--control-port") == 0) {
            if (i + 1 < argc) {
                g_controlConfig.listen = argv[++i];
            }
            continue;
        }
        // treat "-" as stdin indicator, otherwise first non-flag argument is config path
        if (argv[i][0] != '-') {
            configPath = argv[i];
        }
    }
    if (configPath.empty()) {
        printHelp();
        return 1;
    }

    // "-" 表示从 stdin 读取配置
    std::vector<PortConfig> cfgs;
    if (configPath == "-") {
        std::stringstream ss;
        ss << std::cin.rdbuf();
        cfgs = parseConfig(ss.str());
    } else {
            cfgs = loadConfig(configPath);
            g_currentCfgs = cfgs;
            g_configPath = configPath;
    }
    if (cfgs.empty()) {
        std::cerr << "错误: 没有有效的端口配置" << std::endl;
        return 1;
    }

    #ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  handleSignal);
    signal(SIGTERM, handleSignal);
#else
    // Windows: no SIGPIPE, use SetConsoleCtrlHandler for Ctrl+C
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
#endif

    std::cout << "门卫程序启动，管理 " << cfgs.size() << " 个端口:" << std::endl;
    if (!g_controlConfig.listen.empty()) {
        std::cout << "Control 监听端口: " << g_controlConfig.listen << std::endl;
    }
    bool anyMonitor = false;
    struct RelayInfo { PortRelay* relay; const PortConfig* cfg; };
    std::map<std::string, std::vector<RelayInfo>> groupMap; 
    for (auto& c : cfgs) {
        std::cout << "  " << c.listenAddr << " -> \"" << c.command << "\"" << std::endl;
        if (c.monitor.enabled) anyMonitor = true;
        auto relayPtr = std::unique_ptr<PortRelay>(new PortRelay(c));
        if (c.launchOnStart) relayPtr->setLaunchOnStart(true);
        PortRelay* rawPtr = relayPtr.get();
        g_relays.push_back(std::move(relayPtr));
        if (!c.groupName.empty()) {
            groupMap[c.groupName].push_back({rawPtr, &c});
        }
    }

    for (auto& kv : groupMap) {
        const std::string& name = kv.first;
        auto& infos = kv.second;
        const std::string& mode = infos.front().cfg->mode;
        const std::string& command = infos.front().cfg->command;
        for (auto& info : infos) {
            if (info.cfg->mode != mode) {
                std::cerr << "错误: 组 '" << name << "' 中的端口模式不一致" << std::endl;
                return 1;
            }
            if (info.cfg->command != command) {
                std::cerr << "错误: 组 '" << name << "' 中的端口 command 不一致" << std::endl;
                return 1;
            }
        }
        if (mode != "simple") {
            std::cerr << "错误: 仅支持 simple 模式的分组，组 '" << name << "' 使用 mode='" << mode << "'" << std::endl;
            return 1;
        }
        auto groupPtr = std::unique_ptr<PortGroup>(new PortGroup(name));
        for (auto& info : infos) {
            groupPtr->addRelay(info.relay);
            info.relay->setGroup(groupPtr.get());
        }
        g_groups.push_back(std::move(groupPtr));
    }

    for (auto& r : g_relays) r->start();
    // Start control server
    if (g_controlConfig.auth.type == "token" && g_controlConfig.auth.token.empty()) {
        // Generate a random hex token (32 chars = 128 bits)
        std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (int i = 0; i < 4; i++)
            oss << std::setw(16) << rng();
        g_controlConfig.auth.token = oss.str();
    }
    static ControlServer g_controlServer(g_controlConfig);
    if (g_controlServer.isEnabled()) {
        const std::string& ln = g_controlConfig.listen;
        auto pos = ln.find(':');
        std::string host = (pos == std::string::npos) ? ln : ln.substr(0, pos);
        bool exposed = (host.empty() || host == "0.0.0.0");
        if (exposed && g_controlConfig.auth.type != "token") {
            std::cerr << "[ControlServer] 严重警告: 控制端口绑定在所有网卡且无认证，"
                      << "任何能访问该端口的人都可执行任意命令！"
                      << "建议配置 auth 或改用 127.0.0.1" << ln.substr(pos) << std::endl;
        }
        g_controlServer.setRateLimit(g_controlConfig.maxConnections, g_controlConfig.rateLimitSeconds);
        g_controlServer.start();
        std::cout << "[ControlServer] 控制接口: http://" << ln << std::endl;
        if (exposed) {
            std::cout << "[ControlServer] 提示: 已按配置绑定所有网卡(0.0.0.0)" << std::endl;
        }
        if (g_controlConfig.auth.type == "token") {
            std::cout << "[ControlServer] 认证令牌: " << g_controlConfig.auth.token << std::endl;
            std::cout << "[ControlServer] 使用: curl -H 'x-auth-token: " << g_controlConfig.auth.token
                      << "' http://" << ln << "/health" << std::endl;
        } else {
            std::cout << "[ControlServer] 警告: 控制接口未启用认证，限制局域网访问" << std::endl;
        }
    }

    // 启动统一 TCP 监控线程（如果有启用监控的端口）
    std::thread monitorThread;
    if (anyMonitor) {
        monitorThread = std::thread(monitorLoop);
    }

    // 通知 systemd 启动完成
    if (NOTIFY_DID_SEND()) {
        std::cout << "systemd: READY=1" << std::endl;
    }

#ifndef _WIN32
    while (!g_stop.load()) {
        pause();  // wait for signal
        if (g_shutdownRequested.exchange(false)) {
            // Signal received — safe to take mutexes here
            break;
        }
    }
    #else
    // Windows: WaitForSingleObject on an event handle, or just sleep
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    #endif

    std::cout << "门卫程序退出" << std::endl;
    // Stop all groups before exiting (groups stop their relays)
    {
        std::lock_guard<std::mutex> lock(g_groupsMutex);
        for (auto& g : g_groups) {
            if (g) g->stop();
        }
    }
    // Ensure any remaining relays are stopped (non‑grouped)
    {
        std::lock_guard<std::mutex> lock(g_relaysMutex);
        for (auto& r : g_relays) r->stop();
    }
    if (monitorThread.joinable()) monitorThread.join();
    return 0;
}
