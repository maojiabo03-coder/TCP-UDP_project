#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define TCP_PORT 9000
#define UDP_PORT 9099
#define MAX_CLIENTS 3
#define BUF_SIZE 1024

typedef struct {
    int fd;
    char buffer[BUF_SIZE];
    int used;
} Client;

// 发送完整的 TCP 消息
int send_all(int fd, const char *msg) {
    size_t len = strlen(msg);
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, msg + sent, len - sent, 0);

        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }

        sent += n;
    }

    return 0;
}

// 关闭客户端连接
void close_client(Client *c) {
    if (c->fd >= 0)
        close(c->fd);

    c->fd = -1;
    c->used = 0;
}

// 处理一条签到消息
void handle_checkin(
    int client_fd,
    const char *msg,
    int udp_fd,
    struct sockaddr_in *broadcast_addr,
    int announced[]
) {
    char id;

    // 只接受 CHECKIN A、B、C
    if (strlen(msg) != 9 ||
        strncmp(msg, "CHECKIN ", 8) != 0 ||
        msg[8] < 'A' || msg[8] > 'C') {

        send_all(client_fd, "ERROR\n");
        return;
    }

    id = msg[8];

    printf("[TCP] Received: CHECKIN %c\n", id);

    // TCP 确认只发送给签到者
    char reply[32];
    snprintf(reply, sizeof(reply), "OK %c\n", id);

    if (send_all(client_fd, reply) < 0) {
        perror("TCP send");
        return;
    }

    printf("[TCP] Sent: OK %c\n", id);

    int index = id - 'A';

    // 每个编号只广播一次
    if (announced[index]) {
        printf("[INFO] %c already announced\n", id);
        return;
    }

    // UDP 广播公告
    char notice[32];
    snprintf(notice, sizeof(notice), "ANN %c", id);

    ssize_t n = sendto(
        udp_fd,
        notice,
        strlen(notice),
        0,
        (struct sockaddr *)broadcast_addr,
        sizeof(*broadcast_addr)
    );

    if (n < 0) {
        perror("UDP broadcast");
        return;
    }

    announced[index] = 1;

    printf("[UDP] Broadcast: %s\n", notice);
}

// 处理客户端收到的数据，按换行分割消息
void receive_client(
    Client *c,
    int udp_fd,
    struct sockaddr_in *broadcast_addr,
    int announced[]
) {
    char temp[512];

    ssize_t n = recv(c->fd, temp, sizeof(temp), 0);

    if (n <= 0) {
        printf("[TCP] Client disconnected\n");
        close_client(c);
        return;
    }

    for (int i = 0; i < n; i++) {
        if (temp[i] == '\n') {
            c->buffer[c->used] = '\0';

            // 兼容 CRLF
            if (c->used > 0 &&
                c->buffer[c->used - 1] == '\r') {
                c->buffer[c->used - 1] = '\0';
            }

            handle_checkin(
                c->fd,
                c->buffer,
                udp_fd,
                broadcast_addr,
                announced
            );

            c->used = 0;
        } else {
            if (c->used >= BUF_SIZE - 1) {
                printf("[ERROR] Message too long\n");
                close_client(c);
                return;
            }

            c->buffer[c->used++] = temp[i];
        }
    }
}

int main(int argc, char *argv[]) {
    int listen_fd, udp_fd;
    int opt = 1;

    Client clients[MAX_CLIENTS];
    int announced[3] = {0, 0, 0};

    struct sockaddr_in server_addr;
    struct sockaddr_in broadcast_addr;

    // 默认使用受限广播地址
    const char *broadcast_ip =
        argc >= 2 ? argv[1] : "255.255.255.255";

    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].used = 0;
    }

    // ===== 1. 创建 TCP 服务器 =====
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0) {
        perror("TCP socket");
        return 1;
    }

    setsockopt(
        listen_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    );

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(
        listen_fd,
        (struct sockaddr *)&server_addr,
        sizeof(server_addr)
    ) < 0) {
        perror("TCP bind");
        return 1;
    }

    if (listen(listen_fd, 3) < 0) {
        perror("listen");
        return 1;
    }

    // ===== 2. 创建 UDP 广播套接字 =====
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (udp_fd < 0) {
        perror("UDP socket");
        return 1;
    }

    setsockopt(
        udp_fd,
        SOL_SOCKET,
        SO_BROADCAST,
        &opt,
        sizeof(opt)
    );

    memset(&broadcast_addr, 0, sizeof(broadcast_addr));

    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(UDP_PORT);

    if (inet_pton(
        AF_INET,
        broadcast_ip,
        &broadcast_addr.sin_addr
    ) != 1) {
        fprintf(stderr, "Invalid broadcast IP\n");
        return 1;
    }

    printf("Server started\n");
    printf("TCP port: %d\n", TCP_PORT);
    printf("UDP broadcast: %s:%d\n",
           broadcast_ip, UDP_PORT);

    // ===== 3. select 多客户端处理 =====
    while (1) {
        fd_set readfds;

        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        int maxfd = listen_fd;

        // 将所有已连接客户端加入监听集合
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                FD_SET(clients[i].fd, &readfds);

                if (clients[i].fd > maxfd)
                    maxfd = clients[i].fd;
            }
        }

        int ready = select(
            maxfd + 1,
            &readfds,
            NULL,
            NULL,
            NULL
        );

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("select");
            break;
        }

        // ===== 4. 接受新 TCP 连接 =====
        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(client_addr);

            int new_fd = accept(
                listen_fd,
                (struct sockaddr *)&client_addr,
                &len
            );

            if (new_fd >= 0) {
                int slot = -1;

                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }

                if (slot >= 0 &&
                    new_fd < FD_SETSIZE) {

                    clients[slot].fd = new_fd;
                    clients[slot].used = 0;

                    printf(
                        "[TCP] Connected: %s:%d\n",
                        inet_ntoa(client_addr.sin_addr),
                        ntohs(client_addr.sin_port)
                    );
                } else {
                    printf("[TCP] Server full\n");
                    close(new_fd);
                }
            }
        }

        // ===== 5. 处理客户端签到 =====
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 &&
                FD_ISSET(clients[i].fd, &readfds)) {

                receive_client(
                    &clients[i],
                    udp_fd,
                    &broadcast_addr,
                    announced
                );
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        close_client(&clients[i]);

    close(listen_fd);
    close(udp_fd);

    return 0;
}
