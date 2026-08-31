#include "relay_platform.h"
#include <cstring>
#include <algorithm>
#include <bcrypt.h>

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

// Blocking, shell-free execution (CreateProcessA does not invoke cmd.exe).
int runCommand(const std::string& command) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
}

// ── Shell command execution (for /run control endpoint) ───────────────
// Uses cmd.exe /c to support pipes, redirects, &&. The caller is
// authenticated (token+PIN), so shell power is the desired capability.

static std::string hexLen(size_t n) {
    const char* hexc = "0123456789abcdef";
    std::string s;
    for (size_t i = n;;) { s += hexc[i & 0xf]; if (!(i >>= 4)) break; }
    std::reverse(s.begin(), s.end());
    return s;
}

int runShellCommand(const std::string& command, int timeoutMs,
                     int maxOutputBytes, std::string& output) {
    output.clear();
    HANDLE pipeR = NULL, pipeW = NULL;
    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&pipeR, &pipeW, &sa, 0)) return -1;
    SetHandleInformation(pipeR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{0}; PROCESS_INFORMATION pi{0};
    si.cb = sizeof(si);
    si.hStdOutput = pipeW; si.hStdError = pipeW; si.hStdInput = NULL;
    si.dwFlags = STARTF_USESTDHANDLES;
    std::string cmd = "cmd.exe /c " + command;
    BOOL ok = CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, TRUE,
                              CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(pipeW); pipeW = NULL;
    if (!ok) { CloseHandle(pipeR); return -1; }
    CloseHandle(pi.hThread);
    int exitCode = -1;
    char buf[4096];
    DWORD deadline = GetTickCount() + (DWORD)timeoutMs;
    while ((int)output.size() < maxOutputBytes) {
        DWORD now = GetTickCount();
        if ((int)(now - deadline) >= 0) break;
        DWORD avail = 0;
        if (!PeekNamedPipe(pipeR, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
            continue;
        }
        DWORD toRead = avail;
        int room = maxOutputBytes - (int)output.size();
        if ((int)toRead > room) toRead = (DWORD)room;
        if (toRead > sizeof(buf)) toRead = sizeof(buf);
        DWORD got = 0;
        if (ReadFile(pipeR, buf, toRead, &got, NULL) && got > 0)
            output.append(buf, got);
        else break;
    }
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
        DWORD ec = 0; if (GetExitCodeProcess(pi.hProcess, &ec)) exitCode = (int)ec;
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pipeR);
    return exitCode;
}

struct StreamCtx { HANDLE pipeR; HANDLE hProc; PlatformHandle socket; int timeoutMs; const std::atomic<bool>* stop; };

static void* streamCopyThread(void* arg) {
    StreamCtx* c = static_cast<StreamCtx*>(arg);
    char buf[4096];
    DWORD deadline = GetTickCount() + (DWORD)c->timeoutMs;
    while (!c->stop->load()) {
        DWORD now = GetTickCount();
        if ((int)(now - deadline) >= 0) break;
        DWORD avail = 0;
        if (!PeekNamedPipe(c->pipeR, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            if (WaitForSingleObject(c->hProc, 50) == WAIT_OBJECT_0) break;
            continue;
        }
        if (avail > sizeof(buf)) avail = sizeof(buf);
        DWORD got = 0;
        if (ReadFile(c->pipeR, buf, avail, &got, NULL) && got > 0) {
            std::string chunk(buf, got);
            std::string frame = hexLen(chunk.size()) + "\r\n" + chunk + "\r\n";
            if (platform::write_fd(c->socket, frame.data(), frame.size()) <= 0) break;
        } else break;
    }
    return nullptr;
}

int runShellStream(const std::string& command, PlatformHandle socket,
                   int timeoutMs, const std::atomic<bool>& stop) {
    HANDLE pipeR = NULL, pipeW = NULL;
    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&pipeR, &pipeW, &sa, 0)) return -1;
    SetHandleInformation(pipeR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{0}; PROCESS_INFORMATION pi{0};
    si.cb = sizeof(si);
    si.hStdOutput = pipeW; si.hStdError = pipeW; si.hStdInput = NULL;
    si.dwFlags = STARTF_USESTDHANDLES;
    std::string cmd = "cmd.exe /c " + command;
    BOOL ok = CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, TRUE,
                              CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(pipeW); pipeW = NULL;
    if (!ok) { CloseHandle(pipeR); return -1; }
    CloseHandle(pi.hThread);
    StreamCtx ctx{pipeR, pi.hProcess, socket, timeoutMs, &stop};
    streamCopyThread(&ctx);
    int exitCode = -2;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
        DWORD ec = 0; if (GetExitCodeProcess(pi.hProcess, &ec)) exitCode = (int)ec;
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pipeR);
    return exitCode;
}

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

std::string generateAuthToken() {
    unsigned char buf[16];  // 128 bits
    NTSTATUS status = BCryptGenRandom(nullptr, buf, sizeof(buf),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) return "";
    static const char* hexd = "0123456789abcdef";
    std::string hex;
    hex.reserve(32);
    for (unsigned char b : buf) {
        hex += hexd[b >> 4];
        hex += hexd[b & 0x0f];
    }
    return hex;
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