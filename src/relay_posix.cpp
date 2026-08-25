#include "relay_platform.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

namespace platform {

#ifndef __linux__
pid_t findPidUsingPort(uint16_t) { return 0; }
std::string findProcessUsingPort(uint16_t) { return ""; }
#endif

// ── Socket lifecycle ─────────────────────────────────────────────────
PlatformHandle socket_ai(int family, int type, int protocol) {
    return ::socket(family, type, protocol);
}
int bind_fd(PlatformHandle fd, const void* addr, int len) {
    return ::bind(fd, static_cast<const struct sockaddr*>(addr), len);
}
int listen_fd(PlatformHandle fd, int backlog) {
    return ::listen(fd, backlog);
}
PlatformHandle accept_fd(PlatformHandle fd, void* addr, int* len) {
    return ::accept(fd, static_cast<struct sockaddr*>(addr),
                    len ? reinterpret_cast<socklen_t*>(len) : nullptr);
}
int connect_fd(PlatformHandle fd, const void* addr, int len) {
    return ::connect(fd, static_cast<const struct sockaddr*>(addr), len);
}
int close_fd(PlatformHandle fd) { return ::close(fd); }

// ── I/O ───────────────────────────────────────────────────────────────
ssize_t read_fd(PlatformHandle fd, void* buf, size_t len) {
    return ::read(fd, buf, len);
}
ssize_t write_fd(PlatformHandle fd, const void* buf, size_t len) {
    return ::write(fd, buf, len);
}
int shutdown_fd(PlatformHandle fd, int how) {
    return ::shutdown(fd, how);
}
ssize_t recv_peek_fd(PlatformHandle fd, void* buf, size_t len) {
    return ::recv(fd, buf, len, MSG_PEEK);
}

// ── Socket options ────────────────────────────────────────────────────
int set_sockopt(PlatformHandle fd, int level, int optname,
                const void* optval, int optlen) {
    return ::setsockopt(fd, level, optname, optval,
                        static_cast<socklen_t>(optlen));
}

int set_recv_timeout(PlatformHandle fd, int seconds) {
    struct timeval tv = { seconds, 0 };
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int set_nonblock(PlatformHandle fd, int nonblock) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags);
}

// ── Address resolution ────────────────────────────────────────────────
int getaddrinfo_wrap(const char* node, const char* service,
                     const struct addrinfo* hints, struct addrinfo** res) {
    return ::getaddrinfo(node, service, hints, res);
}
void freeaddrinfo_wrap(struct addrinfo* res) {
    ::freeaddrinfo(res);
}

int inet_pton_wrap(int af, const char* src, void* dst) {
    return ::inet_pton(af, src, dst);
}

// ── Error handling ────────────────────────────────────────────────────
int last_error() { return errno; }
bool last_error_is(int code) { return errno == code; }
std::string last_error_str() {
    char buf[256];
#ifdef _GNU_SOURCE
    return strerror_r(errno, buf, sizeof(buf)) ? "unknown error" : buf;
#else
    return strerror_r(errno, buf, sizeof(buf)) == 0 ? buf : "unknown error";
#endif
}

// ── Process / thread ────────────────────────────────────────────────
// Executes the command via execvp() with an argv array — deliberately NO
// shell is involved, so config-supplied metacharacters (`;`, `|`, `$()`,
// backticks, redirections) are passed literally and cannot inject commands.
pid_t launchProcess(const std::string& command) {
    std::vector<std::string> args = parseCommandLine(command);
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

// Blocking, shell-free execution (same security model as launchProcess).
// Returns the child's exit code (0 on success), or -1 if launch failed.
int runCommand(const std::string& command) {
    std::vector<std::string> args = parseCommandLine(command);
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

void killProcess(pid_t pid) { ::kill(pid, SIGKILL); }
void termProcess(pid_t pid) { ::kill(pid, SIGTERM); }

bool isChildAlive(pid_t pid) {
    int status;
    return ::waitpid(pid, &status, WNOHANG) == 0;
}

pid_t waitChild(pid_t pid) {
    int status;
    return ::waitpid(pid, &status, 0);
}

bool isProcessRunning(pid_t pid) {
    return ::kill(pid, 0) == 0;
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

// ── Procfs stubs for non-Linux POSIX (macOS, BSD) ──
#ifndef __linux__
pid_t findPidUsingPort(uint16_t) { return 0; }
std::string findProcessUsingPort(uint16_t) { return ""; }
#endif

} // namespace platform
