#include "doctest.h"
#include "control_server.h"
#include <sstream>

// Test HTTP request parsing and server enable flag
TEST_CASE("ControlServer - HTTP request parsing") {
    ControlConfig emptyCfg;
    ControlServer server(emptyCfg); // disabled, no listen
    CHECK(server.isEnabled() == false);

    ControlConfig cfg;
    cfg.listen = ":19999";
    ControlServer enabledServer(cfg);
    CHECK(enabledServer.isEnabled() == true);
}

TEST_CASE("ControlServer - token auth") {
    ControlConfig cfg;
    cfg.listen = ":19999";
    cfg.auth.type = "token";
    cfg.auth.token = "secret123";
    ControlServer server(cfg);
    CHECK(server.isEnabled() == true);
}
