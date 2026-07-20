// -*- mode: c++; -*-
#ifndef GATEKEEPER_PORT_GROUP_H
#define GATEKEEPER_PORT_GROUP_H

#include "relay.h"
#include <vector>
#include <atomic>
#include <mutex>

// Forward declaration already provided in relay.h

class PortGroup {
public:
    explicit PortGroup(const std::string& name);
    ~PortGroup();

    // Add a PortRelay to this group (must be called before start)
    void addRelay(PortRelay* relay);

    // Called by a PortRelay when it receives a connection (onConnection)
    void onConnection(PortRelay* source);

    // Stop the whole group and all its relays
    void stop();
    void signalStop();
    void resetLaunch();

    bool isLaunched() const { return launched_.load(); }
    bool isRunning() const { return running_.load(); }

private:
    std::string name_;
    std::vector<PortRelay*> relays_;
    std::atomic<bool> launched_{false}; // backend launched
    std::atomic<bool> running_{false};  // backend still alive
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
};

#endif // GATEKEEPER_PORT_GROUP_H
