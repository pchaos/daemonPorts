#include "config.h"
#include "relay.h"

#include <iostream>
#include <memory>
#include <vector>
#include <atomic>
#include <signal.h>
#include <unistd.h>

static std::vector<std::unique_ptr<PortRelay>> g_relays;
static std::atomic<bool> g_stop{false};

static void handleSignal(int) {
    g_stop.store(true);
    for (auto& r : g_relays) r->stop();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " config.json" << std::endl;
        return 1;
    }

    auto cfgs = loadConfig(argv[1]);
    if (cfgs.empty()) {
        std::cerr << "错误: 没有有效的端口配置" << std::endl;
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  handleSignal);
    signal(SIGTERM, handleSignal);

    std::cout << "门卫程序启动，管理 " << cfgs.size() << " 个端口:" << std::endl;
    for (auto& c : cfgs) {
        std::cout << "  " << c.listenAddr << " -> \"" << c.command << "\"" << std::endl;
        auto relay = std::unique_ptr<PortRelay>(new PortRelay(c));
        relay->start();
        g_relays.push_back(std::move(relay));
    }

    pause();

    std::cout << "门卫程序退出" << std::endl;
    return 0;
}