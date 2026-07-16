#include "config.h"
#include "relay.h"

#include <iostream>
#include <memory>
#include <vector>
#include <atomic>
#include <signal.h>
#include <unistd.h>
#include <sstream>

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

    // 通知 systemd 启动完成
    if (NOTIFY_DID_SEND()) {
        std::cout << "systemd: READY=1" << std::endl;
    }

    pause();

    std::cout << "门卫程序退出" << std::endl;
    return 0;
}