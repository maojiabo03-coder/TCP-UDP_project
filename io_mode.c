#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

int main(int argc, char **argv) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }

    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               &on, sizeof(on));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(9100);

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0
        || listen(s, 1) < 0) {
        perror("bind/listen");
        return 1;
    }

    int c = accept(s, NULL, NULL);
    if (c < 0) {
        perror("accept");
        return 1;
    }

    // 切换模式并读取
    if (argc > 1 && !strcmp(argv[1], "nonblock")) {
        int f = fcntl(c, F_GETFL);
        if (f < 0 || fcntl(c, F_SETFL, f | O_NONBLOCK) < 0) {
            perror("fcntl");
            return 1;
        }
    }

    char buf[128];

    for (;;) {
        puts("read wait");

        ssize_t n = read(c, buf, sizeof(buf));

        if (n > 0)
            printf("read: %.*s\n", (int)n, buf);

        else if (n == 0) {
            puts("peer closed");
            break;
        }

        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            puts("would block");

        else {
            perror("read");
            break;
        }

        puts("other_routine");
        sleep(1);
    }

    close(c);
    close(s);

    return 0;
}
