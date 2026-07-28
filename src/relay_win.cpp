#include "relay_platform.h"
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>

namespace platform {

pid_t launchProcess(const std::string& command) {
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    return (pid_t)pi.dwProcessId;
}

void killProcess(pid_t pid) {
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (hProc) { TerminateProcess(hProc, 1); CloseHandle(hProc); }
}
void termProcess(pid_t pid) { killProcess(pid); }

bool isChildAlive(pid_t pid) {
    HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!hProc) return false;
    DWORD ret = WaitForSingleObject(hProc, 0);
    CloseHandle(hProc);
    return ret == WAIT_TIMEOUT;
}

pid_t waitChild(pid_t pid) {
    HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!hProc) return -1;
    while (WaitForSingleObject(hProc, 200) != WAIT_OBJECT_0) {}
    CloseHandle(hProc);
    return pid;
}

bool isProcessRunning(pid_t pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    if (!hProc) return false;
    DWORD exitCode;
    BOOL ok = GetExitCodeProcess(hProc, &exitCode);
    CloseHandle(hProc);
    return ok && exitCode == STILL_ACTIVE;
}

void createThread(PlatformThread& thread, void* (*func)(void*), void* arg, int) {
    thread = std::thread(func, arg);
}
void joinThread(PlatformThread& thread) { if (thread.joinable()) thread.join(); }
bool threadValid(const PlatformThread& thread) { return thread.joinable(); }

// ── Stubs for Linux-only procfs functions ──
pid_t findPidUsingPort(uint16_t) { return 0; }
std::string findProcessUsingPort(uint16_t) { return ""; }

} // namespace platform
