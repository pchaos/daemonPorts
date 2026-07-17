#include "config.h"
#include "relay.h"
#include "tcp_monitor.h"

#include <iostream>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <sstream>
#include <csignal>
#include <chrono>

#ifndef _WIN32
#include <unistd.h>
#endif

// systemd sd_notify 支持（编译时添加 -DHAVE_SYSTEMD 启用）
#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#define NOTIFY_DID_SEND() (sd_notify(0, "READY=1") > 0)
#else
#define NOTIFY_DID_SEND() false
#endif

static std::vector<std::unique_ptr<PortRelay>> g_relays;
static std::atomic<bool> g_stop{false};

static void handleSignal(int) {
    g_stop.store(true);
    for (auto& r : g_relays) r->stop();
}

// 统一 TCP 连接监控线程：轮询所有端口的连接状态并更新活跃时间戳
static void monitorLoop() {
    // 找出最小的轮询间隔（秒）
    int interval = 60;
    for (auto& r : g_relays) {
        if (r->monitorEnabled()) {
            interval = std::min(interval, r->monitorIntervalSec());
        }
    }

    std::cout << "TCP 连接监控已启动，轮询间隔 " << interval << " 秒" << std::endl;

    while (!g_stop.load()) {
        for (auto& r : g_relays) {
            if (!r->monitorEnabled()) continue;
            int port = r->monitorPort();
            if (port <= 0) continue;

            TcpSnapshot cur = queryPortConnections(port);
            int nonListen = 0;
            for (auto& e : cur.entries) {
                if (e.state != TCP_LISTEN) nonListen++;
            }
            bool active = nonListen > 0;
            r->updateActivity(active);

            std::cout << "  [" << r->name() << "] ACTIVE=" << (active ? "1" : "0")
                      << "  connections=" << cur.entries.size()
                      << "  non-listen=" << nonListen
                      << (active ? "" : " (idle)")
                      << std::endl;
        }

        // 逐秒等待，可响应 g_stop
        for (int i = 0; i < interval && !g_stop.load(); i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " config.json" << std::endl;
        return 1;
    }

    // "-" 表示从 stdin 读取配置
    std::vector<PortConfig> cfgs;
    if (std::string(argv[1]) == "-") {
        std::stringstream ss;
        ss << std::cin.rdbuf();
        cfgs = parseConfig(ss.str());
    } else {
        cfgs = loadConfig(argv[1]);
    }
    if (cfgs.empty()) {
        std::cerr << "错误: 没有有效的端口配置" << std::endl;
        return 1;
    }

#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT,  handleSignal);
    signal(SIGTERM, handleSignal);

    std::cout << "门卫程序启动，管理 " << cfgs.size() << " 个端口:" << std::endl;
    bool anyMonitor = false;
    for (auto& c : cfgs) {
        std::cout << "  " << c.listenAddr << " -> \"" << c.command << "\"" << std::endl;
        if (c.monitor.enabled) anyMonitor = true;
        auto relay = std::unique_ptr<PortRelay>(new PortRelay(c));
        relay->start();
        g_relays.push_back(std::move(relay));
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

#ifdef _WIN32
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#else
    pause();
#endif

    std::cout << "门卫程序退出" << std::endl;
    if (monitorThread.joinable()) monitorThread.join();
    return 0;
}
