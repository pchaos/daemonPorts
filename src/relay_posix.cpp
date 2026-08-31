#include "relay_platform.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <chrono>
#include <sstream>
#include <dirent.h>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <cerrno>

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

// ── Shell command execution (for /run control endpoint) ───────────────
// These intentionally use /bin/sh -c to support pipes, redirects, &&.
// Unlike runCommand/launchProcess, the caller is authenticated (token+PIN),
// so shell power is the desired capability, not an injection vector.

static pid_t g_streamChildPid = 0;
static PlatformHandle g_streamPipeRd = -1;
static void streamTimeoutHandler(int) {
    if (g_streamChildPid > 0) { ::kill(g_streamChildPid, SIGKILL); g_streamChildPid = 0; }
    if (g_streamPipeRd >= 0) { ::close(g_streamPipeRd); g_streamPipeRd = -1; }
}

int runShellCommand(const std::string& command, int timeoutMs,
                     int maxOutputBytes, std::string& output) {
    output.clear();
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) < 0) return -1;
    pid_t pid = ::fork();
    if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return -1; }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(pipefd[1]);
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeoutMs);
    char buf[4096];
    int exitCode = -1;
    bool timedOut = false;
    while (output.size() < (size_t)maxOutputBytes) {
        auto now = std::chrono::steady_clock::now();
        int remaining = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - now).count();
        if (remaining <= 0) { timedOut = true; break; }
        struct pollfd pfd = { pipefd[0], POLLIN, 0 };
        int pr = ::poll(&pfd, 1, remaining);
        if (pr <= 0) { timedOut = true; break; }  // timeout or poll error
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            int room = maxOutputBytes - (int)output.size();
            output.append(buf, static_cast<size_t>(n > room ? room : n));
            if ((int)output.size() >= maxOutputBytes) break;
        } else break;  // EOF
    }
    if (timedOut) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        exitCode = -2;  // timeout
    } else {
        int status = 0;
        if (::waitpid(pid, &status, 0) >= 0 && WIFEXITED(status))
            exitCode = WEXITSTATUS(status);
    }
    ::close(pipefd[0]);
    return exitCode;
}

int runShellStream(const std::string& command, PlatformHandle socket,
                   int timeoutMs, const std::atomic<bool>& stop) {
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) < 0) return -1;
    pid_t pid = ::fork();
    if (pid < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return -1; }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(pipefd[1]);
    g_streamChildPid = pid;
    g_streamPipeRd = pipefd[0];
    auto oldAlarm = ::signal(SIGALRM, streamTimeoutHandler);
    ::alarm(std::max(1, timeoutMs / 1000));
    int exitCode = -2;  // assume timeout
    char buf[4096];
    while (!stop.load()) {
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            std::string chunk(buf, static_cast<size_t>(n));
            std::string hexLen;
            {  // chunk size in hex
                size_t len = chunk.size();
                const char* hexc = "0123456789abcdef";
                if (len == 0) hexLen = "0";
                for (size_t i = 0, tmp = len;; ) { hexLen += hexc[tmp & 0xf]; if (!(tmp >>= 4)) break; }
                std::reverse(hexLen.begin(), hexLen.end());
            }
            std::string frame = hexLen + "\r\n" + chunk + "\r\n";
            if (platform::write_fd(socket, frame.data(), frame.size()) <= 0) break;
        } else if (n == 0) {
            exitCode = 0;  // EOF — child exited; will be refined by waitpid
            break;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) > 0) {
        if (WIFEXITED(status)) exitCode = WEXITSTATUS(status);
        else exitCode = -1;
    } else {
        // EOF reached but the child (e.g. `sh -c` reaping its own
        // subshell) hasn't been reaped yet. Give it a short grace
        // window before killing, so the true exit code is preserved.
        bool reaped = false;
        for (int i = 0; i < 20 && !stop.load(); ++i) {
            struct timespec ts = {0, 50 * 1000 * 1000};  // 50ms
            ::nanosleep(&ts, nullptr);
            if (::waitpid(pid, &status, WNOHANG) > 0) { reaped = true; break; }
        }
        if (reaped) {
            if (WIFEXITED(status)) exitCode = WEXITSTATUS(status);
            else exitCode = -1;
        } else if (exitCode == 0) {
            // still running after grace window — force kill
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            exitCode = -1;
        }
    }
    ::alarm(0);
    ::signal(SIGALRM, oldAlarm);
    ::close(pipefd[0]);
    g_streamChildPid = 0;
    g_streamPipeRd = -1;
    return exitCode;
}

static char hexNibble(unsigned char b) {
    return b < 10 ? static_cast<char>('0' + b) : static_cast<char>('a' + b - 10);
}

std::string generateAuthToken() {
    const int bytes = 16;  // 128 bits
    unsigned char buf[16];
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    size_t got = 0;
    while (got < sizeof(buf)) {
        ssize_t n = ::read(fd, buf + got, sizeof(buf) - got);
        if (n <= 0) { ::close(fd); return ""; }
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    std::string hex;
    hex.reserve(32);
    for (int i = 0; i < bytes; ++i) {
        hex += hexNibble(buf[i] >> 4);
        hex += hexNibble(buf[i] & 0x0f);
    }
    return hex;
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

} // namespace platform
