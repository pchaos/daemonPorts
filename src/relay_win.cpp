#include "relay_platform.h"
#include <cstring>

namespace platform {

// ── Socket lifecycle ─────────────────────────────────────────────────
// PlatformHandle is intptr_t on Windows. We cast to/from SOCKET internally.

PlatformHandle socket_ai(int family, int type, int protocol) {
    SOCKET s = ::WSASocketA(family, type, protocol, nullptr, 0, 0);
    return (s == INVALID_SOCKET) ? -1 : static_cast<PlatformHandle>(s);
}
int bind_fd(PlatformHandle fd, const void* addr, int len) {
    SOCKET s = static_cast<SOCKET>(fd);
    return ::bind(s, static_cast<const struct sockaddr*>(addr), len) == SOCKET_ERROR ? -1 : 0;
}
int listen_fd(PlatformHandle fd, int backlog) {
    SOCKET s = static_cast<SOCKET>(fd);
    return ::listen(s, backlog) == SOCKET_ERROR ? -1 : 0;
}
PlatformHandle accept_fd(PlatformHandle fd, void* addr, int* len) {
    SOCKET s = static_cast<SOCKET>(fd);
    int addrlen = len ? *len : 0;
    SOCKET ac = ::accept(s, static_cast<struct sockaddr*>(addr), &addrlen);
    if (len) *len = addrlen;
    return (ac == INVALID_SOCKET) ? -1 : static_cast<PlatformHandle>(ac);
}
int connect_fd(PlatformHandle fd, const void* addr, int len) {
    SOCKET s = static_cast<SOCKET>(fd);
    return ::connect(s, static_cast<const struct sockaddr*>(addr), len) == SOCKET_ERROR ? -1 : 0;
}
int close_fd(PlatformHandle fd) {
    SOCKET s = static_cast<SOCKET>(fd);
    return ::closesocket(s) == SOCKET_ERROR ? -1 : 0;
}

// ── I/O ───────────────────────────────────────────────────────────────

ssize_t read_fd(PlatformHandle fd, void* buf, size_t len) {
    SOCKET s = static_cast<SOCKET>(fd);
    int r = ::recv(s, static_cast<char*>(buf), static_cast<int>(len), 0);
    return r < 0 ? -1 : r;
}
ssize_t write_fd(PlatformHandle fd, const void* buf, size_t len) {
    SOCKET s = static_cast<SOCKET>(fd);
    int r = ::send(s, static_cast<const char*>(buf), static_cast<int>(len), 0);
    return r < 0 ? -1 : r;
}
int shutdown_fd(PlatformHandle fd, int how) {
    SOCKET s = static_cast<SOCKET>(fd);
    return ::shutdown(s, how) == SOCKET_ERROR ? -1 : 0;
}
ssize_t recv_peek_fd(PlatformHandle fd, void* buf, size_t len) {
    SOCKET s = static_cast<SOCKET>(fd);
    int r = ::recv(s, static_cast<char*>(buf), static_cast<int>(len), MSG_PEEK);
    return r < 0 ? -1 : r;
}

// ── Socket options ────────────────────────────────────────────────────

int set_sockopt(PlatformHandle fd, int level, int optname,
                const void* optval, int optlen) {
    SOCKET s = static_cast<SOCKET>(fd);
    return ::setsockopt(s, level, optname,
                        static_cast<const char*>(optval), optlen) == SOCKET_ERROR ? -1 : 0;
}

int set_recv_timeout(PlatformHandle fd, int seconds) {
    SOCKET s = static_cast<SOCKET>(fd);
    DWORD ms = static_cast<DWORD>(seconds * 1000);
    return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms)) == SOCKET_ERROR ? -1 : 0;
}

int set_nonblock(PlatformHandle fd, int nonblock) {
    SOCKET s = static_cast<SOCKET>(fd);
    u_long mode = nonblock ? 1 : 0;
    return ::ioctlsocket(s, FIONBIO, &mode) == SOCKET_ERROR ? -1 : 0;
}

// ── Address resolution ────────────────────────────────────────────────

int getaddrinfo_wrap(const char* node, const char* service,
                     const struct addrinfo* hints, struct addrinfo** res) {
    return ::getaddrinfo(node, service, hints, res);
}
void freeaddrinfo_wrap(struct addrinfo* res) {
    ::freeaddrinfo(res);
}

// ── Address conversion ────────────────────────────────────────────────

int inet_pton_wrap(int af, const char* src, void* dst) {
    return ::inet_pton(af, src, dst);
}

// ── Error handling ────────────────────────────────────────────────────

int last_error() { return ::WSAGetLastError(); }
bool last_error_is(int code) {
    return ::WSAGetLastError() == static_cast<DWORD>(code);
}
std::string last_error_str() {
    // FormatMessage for Windows error codes
    DWORD err = ::WSAGetLastError();
    LPVOID buf = nullptr;
    DWORD n = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    if (n == 0) return "unknown error";
    std::string s(static_cast<const char*>(buf), n);
    ::LocalFree(buf);
    // Trim trailing CR/LF
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

// ── Process / thread ────────────────────────────────────────────────

pid_t launchProcess(const std::string& command) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    return static_cast<pid_t>(pi.dwProcessId);
}

void killProcess(pid_t pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (h) { TerminateProcess(h, 1); CloseHandle(h); }
}
void termProcess(pid_t pid) { killProcess(pid); }

bool isChildAlive(pid_t pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD ret = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return ret == WAIT_TIMEOUT;
}

pid_t waitChild(pid_t pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return -1;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return pid;
}

bool isProcessRunning(pid_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD exitCode;
    BOOL ok = GetExitCodeProcess(h, &exitCode);
    CloseHandle(h);
    return ok && exitCode == STILL_ACTIVE;
}

void createThread(PlatformThread& thread, void* (*func)(void*), void* arg, int) {
    thread = std::thread(func, arg);
}
void joinThread(PlatformThread& thread) { if (thread.joinable()) thread.join(); }
bool threadValid(const PlatformThread& thread) { return thread.joinable(); }

// ── Stubs for Linux-only procfs functions ────────────────────────────

pid_t findPidUsingPort(uint16_t) { return 0; }
std::string findProcessUsingPort(uint16_t) { return ""; }

} // namespace platform