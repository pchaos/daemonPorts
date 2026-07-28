#include "relay_platform.h"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>

namespace platform {

pid_t launchProcess(const std::string& command) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)NULL);
        _exit(127);
    }
    return pid;
}

void killProcess(pid_t pid) { kill(pid, SIGTERM); }
void termProcess(pid_t pid) { kill(pid, SIGKILL); }

bool isChildAlive(pid_t pid) {
    int status;
    return waitpid(pid, &status, WNOHANG) == 0;
}

pid_t waitChild(pid_t pid) {
    int status;
    return waitpid(pid, &status, 0);
}

bool isProcessRunning(pid_t pid) {
    return kill(pid, 0) == 0;
}

void createThread(PlatformThread& thread, void* (*func)(void*), void* arg, int stackSizeKB) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSizeKB * 1024);
    pthread_create(&thread, &attr, func, arg);
    pthread_attr_destroy(&attr);
}

void joinThread(PlatformThread& thread) { pthread_join(thread, nullptr); }
bool threadValid(const PlatformThread& thread) { return thread != 0; }

// ── procfs stubs for non-Linux POSIX (macOS, BSD) ──
#ifndef __linux__
pid_t findPidUsingPort(uint16_t) { return 0; }
std::string findProcessUsingPort(uint16_t) { return ""; }
#endif

} // namespace platform
