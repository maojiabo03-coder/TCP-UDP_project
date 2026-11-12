#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define TCP_PORT 9000
#define UDP_PORT 9099
#define BUF_SIZE 1024

int main(int argc, char *argv[]) {

    if (argc != 3) {
        printf("Usage: %s A|B|C SERVER_IP\n", argv[0]);
        return 1;
    }

    char id = argv[1][0];
    const char *server_ip = argv[2];

    if (strlen(argv[1]) != 1 ||
        id < 'A' || id > 'C') {
        printf("Client ID must be A, B or C\n");
        return 1;
    }

    int tcp_fd, udp_fd;
    int opt = 1;

    struct sockaddr_in server_addr;
    struct sockaddr_in udp_addr;

    // ===== 1. 创建 UDP 接收套接字 =====
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (udp_fd < 0) {
        perror("UDP socket");
        return 1;
    }

    // 允许多个客户端监听同一个 UDP 端口
    if (setsockopt(
        udp_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    ) < 0) {
        perror("SO_REUSEADDR");
        return 1;
    }

    memset(&udp_addr, 0, sizeof(udp_addr));

    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons(UDP_PORT);
    udp_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(
        udp_fd,
        (struct sockaddr *)&udp_addr,
        sizeof(udp_addr)
    ) < 0) {
        perror("UDP bind");
        close(udp_fd);
        return 1;
    }

    // ===== 2. 创建 TCP 套接字 =====
    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (tcp_fd < 0) {
        perror("TCP socket");
        close(udp_fd);
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(TCP_PORT);

    if (inet_pton(
        AF_INET,
        server_ip,
        &server_addr.sin_addr
    ) != 1) {
        printf("Invalid server IP\n");
        close(tcp_fd);
        close(udp_fd);
        return 1;
    }

    if (connect(
        tcp_fd,
        (struct sockaddr *)&server_addr,
        sizeof(server_addr)
    ) < 0) {
        perror("TCP connect");
        close(tcp_fd);
        close(udp_fd);
        return 1;
    }

    printf("Client %c connected to %s:%d\n",
           id, server_ip, TCP_PORT);

    printf("UDP listening on port %d\n", UDP_PORT);
    printf("Type CHECKIN %c to sign in\n", id);
    printf("Type /quit to exit\n");

    char input[BUF_SIZE];
    char buffer[BUF_SIZE];

    // TCP 消息缓冲区
    char tcp_pending[BUF_SIZE];
    int tcp_used = 0;

    // ===== 3. select 同时监听三个输入 =====
    while (1) {

        fd_set readfds;
        FD_ZERO(&readfds);

        // 监听键盘
        FD_SET(STDIN_FILENO, &readfds);

        // 监听 TCP
        FD_SET(tcp_fd, &readfds);

        // 监听 UDP
        FD_SET(udp_fd, &readfds);

        int maxfd = tcp_fd > udp_fd ? tcp_fd : udp_fd;

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

        // ===== 4. 处理键盘输入 =====
        if (FD_ISSET(STDIN_FILENO, &readfds)) {

            if (fgets(input, sizeof(input), stdin) == NULL)
                break;

            input[strcspn(input, "\n")] = '\0';

            if (strcmp(input, "/quit") == 0)
                break;

            char expected[32];
            snprintf(
                expected,
                sizeof(expected),
                "CHECKIN %c",
                id
            );

            if (strcmp(input, expected) != 0) {
                printf("Please enter: %s\n", expected);
                continue;
            }

            char message[32];
            snprintf(
                message,
                sizeof(message),
                "CHECKIN %c\n",
                id
            );

            if (send(
                tcp_fd,
                message,
                strlen(message),
                0
            ) < 0) {
                perror("TCP send");
                break;
            }

            printf("[TCP] Sent: CHECKIN %c\n", id);
        }

        // ===== 5. 接收 TCP 确认 =====
        if (FD_ISSET(tcp_fd, &readfds)) {

            int n = recv(
                tcp_fd,
                buffer,
                sizeof(buffer),
                0
            );

            if (n <= 0) {
                printf("[TCP] Server disconnected\n");
                break;
            }

            // TCP 是字节流，按换行提取完整消息
            for (int i = 0; i < n; i++) {
                if (buffer[i] == '\n') {
                    tcp_pending[tcp_used] = '\0';

                    printf("[TCP] %s\n", tcp_pending);
                    tcp_used = 0;
                } else if (tcp_used < BUF_SIZE - 1) {
                    tcp_pending[tcp_used++] = buffer[i];
                } else {
                    printf("[TCP] Message too long\n");
                    tcp_used = 0;
                }
            }
        }

        // ===== 6. 接收 UDP 广播 =====
        if (FD_ISSET(udp_fd, &readfds)) {

            struct sockaddr_in sender_addr;
            socklen_t sender_len = sizeof(sender_addr);

            int n = recvfrom(
                udp_fd,
                buffer,
                sizeof(buffer) - 1,
                0,
                (struct sockaddr *)&sender_addr,
                &sender_len
            );

            if (n > 0) {
                buffer[n] = '\0';

                printf("[UDP] %s\n", buffer);
            }
        }
    }

    close(tcp_fd);
    close(udp_fd);

    printf("Client %c exited\n", id);

    return 0;
}
