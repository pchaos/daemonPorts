#ifndef SD_DAEMON_H
#define SD_DAEMON_H

#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static inline int sd_notify(int unset_environment, const char *state) {
    const char *sock_path;
    struct sockaddr_un addr;
    int fd, r;

    if (!state) return -1;

    sock_path = getenv("NOTIFY_SOCKET");
    if (!sock_path || sock_path[0] != '/') return 0; /* not under systemd */

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        r = -errno;
        goto finish;
    }

    if (write(fd, state, strlen(state)) < 0) {
        r = -errno;
        goto finish;
    }
    r = 1;

finish:
    close(fd);
    if (unset_environment) unsetenv("NOTIFY_SOCKET");
    return r;
}

#endif /* SD_DAEMON_H */