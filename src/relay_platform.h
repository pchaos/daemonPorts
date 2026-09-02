#ifndef GATEKEEPER_RELAY_PLATFORM_H
#define GATEKEEPER_RELAY_PLATFORM_H
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

// ── Platform-specific system headers ──────────────────────────────────
// relay.cpp includes relay_platform.h (via relay.h) and gets everything
// it needs without any direct POSIX/Winsock #include.
#ifndef _WIN32
#include <sys/types.h>      // pid_t, mode_t
#include <sys/socket.h>     // socket, bind, setsockopt, sockaddr, socklen_t
#include <netinet/in.h>     // sockaddr_in, INADDR_ANY, IN6_IS_ADDR_V4MAPPED
#include <arpa/inet.h>      // inet_pton, inet_ntop
#include <netdb.h>          // getaddrinfo, freeaddrinfo, addrinfo
#include <cerrno>           // errno, EADDRINUSE, EACCES, EINVAL
#include <unistd.h>         // close, read, write, usleep
#include <signal.h>         // kill, SIGTERM, SIGKILL
#include <pthread.h>        // pthread_t, pthread_create, etc.
using pid_t = ::pid_t;
using PlatformThread = pthread_t;
using PlatformHandle = int;
#else  // _WIN32
// Order matters: winsock2.h must be before windows.h
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>       // getaddrinfo, freeaddrinfo, inet_pton
#include <windows.h>
#include <io.h>
#include <process.h>        // _getpid (Windows)
using pid_t = intptr_t;
using PlatformThread = std::thread;
using PlatformHandle = intptr_t;
// ssize_t is not a standard Windows type; define it as intptr_t
// (same signed-size semantics as POSIX ssize_t)
using ssize_t = intptr_t;
#endif

// SOCK_CLOEXEC is a Linux extension; not available on macOS or Windows.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

// SHUT_WR/SHUT_RDWR are POSIX constants; not defined on Windows.
// Windows SD_SEND=1 == SHUT_WR, SD_BOTH=2 == SHUT_RDWR (same values).
#ifndef SHUT_WR
#define SHUT_WR 1
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

// ── Portable socket interface ─────────────────────────────────────────
// relay.cpp uses only these platform:: wrappers. No direct socket()
// calls, no POSIX headers, no Winsock headers.

// ── Portable error code constants ────────────────────────────────────
// These map to errno values on POSIX and WSA* values on Windows.
#ifndef _WIN32
#define PLATFORM_EINVAL EINVAL
#define PLATFORM_EADDRINUSE EADDRINUSE
#define PLATFORM_EACCES EACCES
#else
// WSA error codes are distinct from errno values
#define PLATFORM_EINVAL WSAEINVAL
#define PLATFORM_EADDRINUSE WSAEADDRINUSE
#define PLATFORM_EACCES WSAEACCES
#endif
namespace platform {

// Socket lifecycle
PlatformHandle socket_ai(int family, int type, int protocol);
int bind_fd(PlatformHandle fd, const void* addr, int len);
int listen_fd(PlatformHandle fd, int backlog);
PlatformHandle accept_fd(PlatformHandle fd, void* addr, int* len);
int connect_fd(PlatformHandle fd, const void* addr, int len);
int close_fd(PlatformHandle fd);

// I/O
ssize_t read_fd(PlatformHandle fd, void* buf, size_t len);
ssize_t write_fd(PlatformHandle fd, const void* buf, size_t len);
int shutdown_fd(PlatformHandle fd, int how);
ssize_t recv_peek_fd(PlatformHandle fd, void* buf, size_t len);

// Socket options
int set_sockopt(PlatformHandle fd, int level, int optname, const void* optval, int optlen);

// Set recv timeout on a socket (0 disables timeout)
int set_recv_timeout(PlatformHandle fd, int seconds);

// Set socket to non-blocking mode (0 = blocking, 1 = non-blocking)
int set_nonblock(PlatformHandle fd, int nonblock);

// Address resolution (getaddrinfo wrapper)
int getaddrinfo_wrap(const char* node, const char* service,
                     const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo_wrap(struct addrinfo* res);

// Address conversion (inet_pton wrapper)
int inet_pton_wrap(int af, const char* src, void* dst);

// Error handling (translates WSAGetLastError on Windows)
int  last_error();
bool last_error_is(int code);
std::string last_error_str();

// ── Process / thread helpers ────────────────────────────────────────
pid_t launchProcess(const std::string& command);
void killProcess(pid_t pid);
void termProcess(pid_t pid);
bool isChildAlive(pid_t pid);
pid_t waitChild(pid_t pid);
bool isProcessRunning(pid_t pid);
// Current process ID (portable wrapper for getpid/_getpid)
inline pid_t currentPid() {
#ifndef _WIN32
    return ::getpid();
#else
    return (pid_t)_getpid();
#endif
}
void createThread(PlatformThread& thread, void* (*func)(void*), void* arg, int stackSizeKB);
void joinThread(PlatformThread& thread);
bool threadValid(const PlatformThread& thread);

// ── Procfs helpers (Linux only; other platforms return empty) ────────
pid_t findPidUsingPort(uint16_t port);
std::string findProcessUsingPort(uint16_t port);

// Parse a command string into an argument array (POSIX sh subset:
// whitespace splitting, single/double quotes, backslash escapes).
// No shell metacharacter expansion is performed — this is deliberate:
// since rev 2777c79.. the process launcher no longer goes through a
// shell, so `;`, `|`, `$()` etc. are passed literally as arguments
// instead of being interpreted (H1/H3 fix).
// Returns an empty vector on parse error (unterminated quote).
inline std::vector<std::string> parseCommandLine(const std::string& cmd) {
    std::vector<std::string> args;
    size_t i = 0;
    const size_t n = cmd.size();
    while (i < n) {
        // Skip whitespace
        while (i < n && (cmd[i] == ' ' || cmd[i] == '\t' || cmd[i] == '\n' || cmd[i] == '\r')) ++i;
        if (i >= n) break;
        std::string arg;
        bool closed = false;  // true when the argument ended normally
        while (i < n) {
            char c = cmd[i];
            if (c == '\'' || c == '"') {
                char quote = c;
                ++i;
                bool term = false;
                while (i < n) {
                    if (quote == '"' && cmd[i] == '\\' && i + 1 < n) {
                        // Inside double quotes: backslash escapes only quote and backslash.
                        char nx = cmd[i + 1];
                        if (nx == '"' || nx == '\\') { arg += nx; i += 2; continue; }
                        arg += cmd[i]; ++i; continue;
                    }
                    if (cmd[i] == quote) { term = true; ++i; break; }
                    arg += cmd[i]; ++i;
                }
                if (!term) return {};  // unterminated quote
                continue;
            }
            if (c == '\\' && i + 1 < n) {
                // Outside quotes: backslash escapes the next character
                arg += cmd[i + 1]; i += 2; continue;
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') break;
            arg += c; ++i;
        }
        args.push_back(arg);
        closed = (i < n) || !args.empty();
        (void)closed;
    }
    return args;
}

// Blocking execution of a command WITHOUT a shell (H3 fix).
// POSIX builds fork+execvp; Windows builds CreateProcessA directly
// (no cmd.exe involvement). Returns the process exit code, or -1 on
// launch failure.
int runCommand(const std::string& command);

// Execute a shell command synchronously, capturing combined stdout+stderr.
// Uses /bin/sh -c (POSIX) or cmd.exe /c (Windows). Returns exit code, or
// -1 on launch failure. Output is appended to `output`, truncated at
// maxOutputBytes. Runs for at most `timeoutMs` ms; killed on timeout.
int runShellCommand(const std::string& command, int timeoutMs,
                     int maxOutputBytes, std::string& output);

// Stream a shell command's combined stdout+stderr to a socket descriptor
// using HTTP chunked transfer-encoding. Returns the exit code (or -1 on
// launch failure / -2 on timeout). Each line is flushed immediately.
// `stop` is checked between chunks to allow graceful shutdown.
int runShellStream(const std::string& command, PlatformHandle socket,
                   int timeoutMs, const std::atomic<bool>& stop);
// Generate a 128-bit random auth token as 32 lowercase hex chars.
// Uses a CSPRNG: /dev/urandom on POSIX, BCryptGenRandom on Windows.
// Returns an empty string on RNG failure.
std::string generateAuthToken();
} // namespace platform
#endif
