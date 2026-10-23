// N4/N5 冒烟测试: 并发流式 /run 互不干扰 + 超时连进程组杀孙进程
// 用法: g++ -std=c++11 -O1 -pthread -Isrc scripts/smoke-n45.cpp src/relay_posix.cpp -o /tmp/smoke-n45 && /tmp/smoke-n45
#include "relay_platform.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>

static std::string readAll(int fd) {
    std::string out; char buf[4096]; ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    return out;
}

int main() {
    std::atomic<bool> stop{false};

    // ── N5: 超时后孙进程 (sleep 6241) 必须被连组杀 ──
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 1; }
        int ec = platform::runShellStream("sleep 6241 & echo done", sv[1], 2000, stop);
        ::close(sv[1]);
        std::string out = readAll(sv[0]);
        ::close(sv[0]);
        int r = ::system("pgrep -f 'sleep 6241$' >/dev/null 2>&1 && echo ALIVE || echo DEAD");
        int alive = (r >= 0 && !WIFEXITED(r)) ? 1 : (WIFEXITED(r) ? WEXITSTATUS(r) : 1);
        (void)alive;
        printf("N5: exitCode=%d (期望 -2=timeout), 输出=%zuB, 孙进程状态: ", ec, out.size());
        FILE* p = popen("pgrep -f 'sleep 6241$' >/dev/null 2>&1 && echo ALIVE || echo DEAD", "r");
        char st[16]; if (p) { fgets(st, sizeof(st), p); pclose(p); }
        printf("%s", st);
        bool pass = (ec == -2) && strstr(st, "DEAD");
        printf("  → %s\n", pass ? "PASS" : "FAIL");
        if (!pass) return 1;
    }

    // ── N4: 两个并发流式请求, 各自独立收尾 ──
    {
        std::vector<std::thread> ts;
        std::vector<int> codes(2, -99);
        std::vector<std::string> outs(2);
        for (int i = 0; i < 2; ++i) {
            ts.emplace_back([&, i]{
                int sv[2];
                if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
                codes[i] = platform::runShellStream(
                    "for j in 1 2 3 4 5; do echo L$j; sleep 0.05; done", sv[1], 5000, stop);
                ::close(sv[1]);
                outs[i] = readAll(sv[0]);
                ::close(sv[0]);
            });
        }
        for (auto& t : ts) t.join();
        bool ok = codes[0] == 0 && codes[1] == 0;
        bool betw = outs[0].find("L5") != std::string::npos && outs[1].find("L5") != std::string::npos;
        printf("N4: code=[%d,%d] (期望 0,0), 完整输出=%s → %s\n",
               codes[0], codes[1], betw ? "是" : "否", (ok && betw) ? "PASS" : "FAIL");
        if (!ok || !betw) return 1;
    }

    printf("ALL PASS\n");
    return 0;
}