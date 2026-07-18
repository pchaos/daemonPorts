// -*- mode: c++; -*-
#include "port_group.h"
#include <iostream>
#include <unistd.h>

PortGroup::PortGroup(const std::string &name) : name_(name) {}

PortGroup::~PortGroup() {
  // Ensure all relays are stopped when the group is destroyed
  stop();
}

void PortGroup::addRelay(PortRelay *relay) {
  if (!relay)
    return;
  std::lock_guard<std::mutex> lock(mtx_);
  relays_.push_back(relay);
}

void PortGroup::onConnection(PortRelay * /*source*/) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (launched_)
    return; // already launched
  launched_ = true;
  running_ = true;
  // Launch backend for each relay and close its listening socket
  for (PortRelay *r : relays_) {
    if (r->backendPid_ == 0) {
      pid_t pid = r->launchBackend();
      if (pid > 0) {
        r->backendPid_ = pid;
        std::cout << "  [" << r->name_ << "] 后端已启动 (PID=" << pid << ")"
                  << std::endl;
      }
    }
    // Close listening socket so that the port is released to the backend
    int listenFd = r->listenFd_.load();
    if (listenFd >= 0) {
      close(listenFd);
      r->listenFd_.store(-1);
    }
  }
}

void PortGroup::stop() {
  if (stop_.exchange(true))
    return; // already stopping
  std::lock_guard<std::mutex> lock(mtx_);
  for (PortRelay *r : relays_) {
    r->stop();
  }
  launched_ = false;
    running_ = false;
}

void PortGroup::resetLaunch() {
    std::lock_guard<std::mutex> lock(mtx_);
    launched_ = false;
    running_ = false;
}
