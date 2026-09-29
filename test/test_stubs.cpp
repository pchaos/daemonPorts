// Test stubs for symbols defined in main.cpp
// These are only used when compiling the test target (which doesn't include main.cpp)
#include "config.h"
#include "relay.h"
#include <vector>
#include <mutex>

std::mutex g_relaysMutex;
std::vector<std::unique_ptr<PortRelay>> g_relays;

struct ReloadSummary {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> modified;
    bool success = true;
};

ReloadSummary reloadFromFile() { ReloadSummary s; s.success = false; return s; }

// Stub for relay.cpp's runtime config persistence (defined in main.cpp)
bool persistRefreshSeconds(const std::string&, int) { return false; }
ReloadSummary reloadFromJson(const std::string&) { ReloadSummary s; s.success = false; return s; }