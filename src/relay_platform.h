#ifndef GATEKEEPER_RELAY_PLATFORM_H
#define GATEKEEPER_RELAY_PLATFORM_H

#include <string>
#include <cstdint>
#include <sys/types.h>  // pid_t

#ifdef _WIN32
#include <thread>
using PlatformThread = std::thread;
#else
#include <pthread.h>
using PlatformThread = pthread_t;
#endif

namespace platform {

pid_t launchProcess(const std::string& command);
void killProcess(pid_t pid);
void termProcess(pid_t pid);
bool isChildAlive(pid_t pid);
pid_t waitChild(pid_t pid);
bool isProcessRunning(pid_t pid);

void createThread(PlatformThread& thread, void* (*func)(void*), void* arg, int stackSizeKB);
void joinThread(PlatformThread& thread);
bool threadValid(const PlatformThread& thread);

pid_t findPidUsingPort(uint16_t port);
std::string findProcessUsingPort(uint16_t port);

} // namespace platform

#endif