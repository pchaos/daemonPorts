#include "doctest.h"
#include "config.h"
#include "relay.h"

TEST_CASE("idle detection triggers gracefulStop") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;
    cfg.idleMinutes = 1; // low idle threshold
    cfg.stopCommand = ""; // no external stop command
    PortRelay relay(cfg);
    // Simulate a running backend
    relay.backendPid_ = 12345;
    // No recent activity (lastActiveTime_ defaults to 0)
    CHECK(relay.hasRecentActivity(1) == false);
    // Invoke graceful stop as monitorLoop would do when idle
    relay.gracefulStop();
    // backendPid_ should be cleared
    CHECK(relay.backendPid_ == 0);
}
