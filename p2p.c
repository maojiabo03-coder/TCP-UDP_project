#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>

#define TCP_PORT 9000
#define UDP_PORT 9099
#define BUF_SIZE 2048

static int tcp_fd;
static int udp_fd;

/* 获取单调时钟时间，单位毫秒 */
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 等待套接字就绪，超过截止时间返回0 */
static int wait_ready(int fd, short events, long long deadline) {
    for (;;) {
        long long remaining = deadline - now_ms();
        if (remaining <= 0)
            return 0;

        struct pollfd p = {0};
        p.fd = fd;
        p.events = events;

        int result = poll(&p, 1, (int)remaining);
        if (result < 0 && errno == EINTR)
            continue;

        return result;
    }
}

/* 后台线程：接收TCP请求，然后用UDP回复 */
static void *receive_requests(void *unused) {
    (void)unused;

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;

        int client = accept(
            tcp_fd, (struct sockaddr *)&peer, &peer_len);

        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        char buffer[BUF_SIZE];
        size_t used = 0;
        int complete = 0;
        long long deadline = now_ms() + 2000;

        /* TCP可能分多次到达，累积到换行符为止 */
        while (used < sizeof buffer - 1) {
            int ready = wait_ready(client, POLLIN, deadline);
            if (ready <= 0)
                break;

            ssize_t n = recv(
                client, buffer + used,
                sizeof buffer - 1 - used, MSG_DONTWAIT);

            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN ||
                    errno == EWOULDBLOCK)
                    continue;
                break;
            }

            if (n == 0)
                break;

            used += (size_t)n;
            buffer[used] = '\0';

            char *newline = memchr(buffer, '\n', used);
            if (newline != NULL) {
                *newline = '\0';
                used = (size_t)(newline - buffer);
                complete = 1;
                break;
            }
        }

        close(client);

        if (!complete) {
            printf("\n[TCP] Incomplete or oversized request.\n");
            continue;
        }

        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr,
                  peer_ip, sizeof peer_ip);

        printf("\n[TCP Received] From %s: %s\n",
               peer_ip, buffer);

        /*
         * IP使用TCP连接对端的IP。
         * 端口改成对方监听的UDP 9099。
         */
        peer.sin_port = htons(UDP_PORT);

        ssize_t sent = sendto(
            udp_fd, buffer, used, 0,
            (struct sockaddr *)&peer, sizeof peer);

        if (sent < 0)
            perror("sendto");
        else
            printf("[UDP Reply] To %s:%d: %s\n",
                   peer_ip, UDP_PORT, buffer);
    }

    return NULL;
}

/* 主动发送一次TCP请求，再等待对应UDP回复 */
static void send_request(
        const char *ip, const char *id, const char *message) {
    struct sockaddr_in destination = {0};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(TCP_PORT);

    if (inet_pton(AF_INET, ip, &destination.sin_addr) != 1) {
        printf("[Error] Invalid IPv4 address.\n");
        return;
    }

    char request[BUF_SIZE];
    int length = snprintf(
        request, sizeof request, "%s %s\n", id, message);

    if (length < 0 || length >= (int)sizeof request) {
        printf("[Error] Message too long.\n");
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl");
        close(fd);
        return;
    }

    /* 建立TCP连接最多等待2秒 */
    long long deadline = now_ms() + 2000;

    int result = connect(
        fd, (struct sockaddr *)&destination, sizeof destination);

    if (result < 0) {
        if (errno != EINPROGRESS) {
            perror("connect");
            close(fd);
            return;
        }

        result = wait_ready(fd, POLLOUT, deadline);
        if (result <= 0) {
            printf("[TCP] Connection timeout or wait error.\n");
            close(fd);
            return;
        }

        int error = 0;
        socklen_t error_len = sizeof error;

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                       &error, &error_len) < 0) {
            perror("getsockopt");
            close(fd);
            return;
        }

        if (error != 0) {
            errno = error;
            perror("connect");
            close(fd);
            return;
        }
    }

    /* TCP可能只发送一部分，因此循环直到发完 */
    size_t offset = 0;
    deadline = now_ms() + 2000;

    while (offset < (size_t)length) {
        if (wait_ready(fd, POLLOUT, deadline) <= 0) {
            printf("[TCP] Send timeout or wait error.\n");
            close(fd);
            return;
        }

        ssize_t n = send(
            fd, request + offset,
            (size_t)length - offset, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN ||
                errno == EWOULDBLOCK)
                continue;

            perror("send");
            close(fd);
            return;
        }

        if (n == 0) {
            printf("[TCP] Connection closed during send.\n");
            close(fd);
            return;
        }

        offset += (size_t)n;
    }

    printf("[TCP Sent] To %s: %s", ip, request);
    close(fd);

    /* TCP请求发完后，最多等2秒UDP回复 */
    deadline = now_ms() + 2000;

    for (;;) {
        int ready = wait_ready(udp_fd, POLLIN, deadline);

        if (ready == 0) {
            printf("[UDP Timeout] ID=%s: no reply within 2 seconds.\n",
                   id);
            return;
        }

        if (ready < 0) {
            perror("poll");
            return;
        }

        char reply[BUF_SIZE];
        struct sockaddr_in source;
        socklen_t source_len = sizeof source;

        ssize_t n = recvfrom(
            udp_fd, reply, sizeof reply - 1, MSG_DONTWAIT,
            (struct sockaddr *)&source, &source_len);

        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN ||
                errno == EWOULDBLOCK)
                continue;

            perror("recvfrom");
            return;
        }

        reply[n] = '\0';

        /*
         * 检查来源IP、端口和原消息。
         * 原消息中包含编号，因此编号也必须一致。
         */
        if (source.sin_addr.s_addr != destination.sin_addr.s_addr ||
            ntohs(source.sin_port) != UDP_PORT ||
            n != length - 1 ||
            memcmp(reply, request, (size_t)n) != 0) {
            printf("[UDP Ignored] Unmatched reply: %s\n", reply);
            continue;
        }

        printf("[UDP Received] %s\n", reply);
        printf("[OK] Reply matches request ID=%s.\n", id);
        return;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (tcp_fd < 0 || udp_fd < 0) {
        perror("socket");
        return 1;
    }

    int on = 1;
    setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);

    local.sin_port = htons(TCP_PORT);
    if (bind(tcp_fd, (struct sockaddr *)&local, sizeof local) < 0 ||
        listen(tcp_fd, 5) < 0) {
        perror("TCP bind/listen");
        return 1;
    }

    local.sin_port = htons(UDP_PORT);
    if (bind(udp_fd, (struct sockaddr *)&local, sizeof local) < 0) {
        perror("UDP bind");
        return 1;
    }

    pthread_t thread;
    int error = pthread_create(
        &thread, NULL, receive_requests, NULL);

    if (error != 0) {
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        return 1;
    }

    pthread_detach(thread);

    printf("Listening: TCP %d, UDP %d\n", TCP_PORT, UDP_PORT);
    printf("Command: SEND peer_IP ID message\n");
    printf("Example: SEND 192.168.1.12 01 hello\n");
    printf("Enter QUIT to exit. Use a new ID for each request.\n");

    char line[BUF_SIZE];

    for (;;) {
        printf("\np2p> ");

        if (fgets(line, sizeof line, stdin) == NULL)
            break;

        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            printf("[Error] Command too long.\n");
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "QUIT") == 0)
            break;

        char ip[64];
        char id[32];
        int position = 0;

        int fields = sscanf(
            line, "SEND %63s %31s %n", ip, id, &position);

        if (fields != 2 || position == 0 ||
            line[position] == '\0') {
            printf("Usage: SEND peer_IP ID message\n");
            continue;
        }

        /* 此函数返回前，不会处理下一条主动请求 */
        send_request(ip, id, line + position);
    }

    /* main退出时，系统回收线程及套接字 */
    return 0;
}
