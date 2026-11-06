/* padctl — send a command to /tmp/padmode.sock and print the reply */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "status";
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/padmode.sock");
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("connect"); close(fd); return 1;
    }
    write(fd, cmd, strlen(cmd));
    shutdown(fd, SHUT_WR);
    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; printf("%s", buf); }
    close(fd);
    return 0;
}
