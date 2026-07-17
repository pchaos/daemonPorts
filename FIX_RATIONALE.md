Added missing `<unistd.h>` include to `src/port_group.cpp` to provide declaration for `close()` used when releasing listening sockets. This resolves compilation error during build.
