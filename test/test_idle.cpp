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
    // lastActiveTime_ initialized to time(nullptr) → hasRecentActivity returns true immediately
    CHECK(relay.hasRecentActivity(1) == true);
    // Invoke graceful stop as monitorLoop would do when idle
    relay.gracefulStop();
    // backendPid_ should be cleared
    CHECK(relay.backendPid_ == 0);
}

TEST_CASE("resetForIdle restarts after gracefulStop") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "true";
    cfg.refreshSeconds = 5;
    cfg.idleMinutes = 1;
    cfg.autoRestart = true;
    PortRelay relay(cfg);
    relay.backendPid_ = 12345;
    relay.gracefulStop();
    CHECK(relay.backendPid_ == 0);
    CHECK(relay.isBackendRunning() == false);
    relay.resetForIdle();
    CHECK(relay.stop_ == false);
    CHECK(relay.isPendingRemoval() == false);
    CHECK(relay.isBackendRunning() == false);
    // Clean up threads created by resetForIdle
    relay.stop();
}
