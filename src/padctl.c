/* padctl — send a command to /tmp/padmode.sock and print the reply */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv) {
    char cmd[128];
    if (argc <= 1) {
        snprintf(cmd, sizeof cmd, "status");
    } else {
        int off = 0;
        for (int i = 1; i < argc && off < (int)sizeof cmd - 1; i++) {
            if (i > 1) cmd[off++] = ' ';
            off += snprintf(cmd + off, sizeof cmd - off, "%s", argv[i]);
        }
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/padmode.sock");
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect"); close(fd); return 1;
    }
    write(fd, cmd, strlen(cmd));
    shutdown(fd, SHUT_WR);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, stdout);   /* the whole reply (leds, peek are long) */
    close(fd);
    return 0;
}
