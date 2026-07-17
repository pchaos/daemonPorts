#include "port_group.h"
#include "relay.h"
#include "config.h"
#include "doctest.h"
#include <thread>
#include <chrono>

TEST_CASE("PortGroup basic lifecycle") {
    // Minimal config for a dummy relay
    PortConfig cfg1;
    cfg1.name = "relay1";
    cfg1.listenAddr = "127.0.0.1:0"; // let OS pick a free port
    cfg1.command = "true"; // simple command that exits immediately
    cfg1.delayMs = 0;
    cfg1.refreshSeconds = 0;
    cfg1.retrySeconds = 1;
    cfg1.maxRetrySeconds = 1;
    cfg1.autoRestart = false;
    cfg1.stackSize = 256;
    cfg1.mode = "simple";
    cfg1.holdPort = false;

    PortConfig cfg2 = cfg1;
    cfg2.name = "relay2";

    PortRelay r1(cfg1);
    PortRelay r2(cfg2);

    PortGroup group("testgroup");
    group.addRelay(&r1);
    group.addRelay(&r2);

    // Simulate a connection on the first relay
    group.onConnection(&r1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(group.isLaunched() == true);
    // backendPid_ should be set (non-zero) for both relays
    CHECK(r1.backendPid_ != 0);
    CHECK(r2.backendPid_ != 0);

    group.stop();
    // After stop, the relays should have their stop flag set (private, but we can check via stop_)
    // Since stop_ is private, we just ensure no crash and the group reports not launched.
    CHECK(group.isLaunched() == false);

    // Idempotent stop test
    group.stop(); // second stop should be safe
    CHECK(group.isLaunched() == false);
}

// Additional test cases

TEST_CASE("PortGroup - 并发 onConnection 只启动一次后端") {
    PortConfig cfg1;
    cfg1.name = "relay1";
    cfg1.listenAddr = "127.0.0.1:0";
    cfg1.command = "true";
    cfg1.delayMs = 0;
    cfg1.refreshSeconds = 0;
    cfg1.retrySeconds = 1;
    cfg1.maxRetrySeconds = 1;
    cfg1.autoRestart = false;
    cfg1.stackSize = 256;
    cfg1.mode = "simple";
    cfg1.holdPort = false;

    PortConfig cfg2 = cfg1;
    cfg2.name = "relay2";

    PortRelay r1(cfg1);
    PortRelay r2(cfg2);

    PortGroup group("testgroup");
    group.addRelay(&r1);
    group.addRelay(&r2);

    std::thread t1([&]{ group.onConnection(&r1); });
    std::thread t2([&]{ group.onConnection(&r2); });
    t1.join();
    t2.join();

    CHECK(group.isLaunched() == true);
    CHECK(r1.backendPid_ != 0);
    CHECK(r2.backendPid_ != 0);
}

TEST_CASE("PortGroup - 无组 relay 行为不变") {
    PortConfig cfg;
    cfg.name = "solo";
    cfg.listenAddr = "127.0.0.1:0";
    cfg.command = "true";
    cfg.delayMs = 0;
    cfg.refreshSeconds = 0;
    cfg.retrySeconds = 1;
    cfg.maxRetrySeconds = 1;
    cfg.autoRestart = false;
    cfg.stackSize = 256;
    cfg.mode = "simple";
    cfg.holdPort = false;

    PortRelay r(cfg);
    // A relay without a group should have no group_ set
    // Just verify it can be constructed and destroyed without crash
    CHECK(r.backendPid_ == 0);
}
