/*
 * udp_send.c —— UDP 消息积压实验：发送端（第四题）
 *
 * 编译: gcc udp_send.c -o udp_send
 * 运行: ./udp_send <接收端IP> <端口>
 *   例: ./udp_send 127.0.0.1 5200
 *
 * 发送编号 1~100，每条固定 512 字节，间隔 5 毫秒，统计 sendto() 成功的条数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MSG_SIZE    512
#define MSG_COUNT   100
#define INTERVAL_US 5000    /* 5 毫秒 */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "用法: %s <接收端IP> <端口>\n", argv[0]);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) {
        fprintf(stderr, "IP 地址格式不对: %s\n", argv[1]);
        return 1;
    }

    int  ok = 0;
    char buf[MSG_SIZE];

    for (int seq = 1; seq <= MSG_COUNT; seq++) {
        memset(buf, '.', sizeof buf);                    /* 用 '.' 填满 512 字节 */
        int n = snprintf(buf, sizeof buf, "MSG %03d", seq);
        buf[n] = ' ';                                    /* 去掉 snprintf 写入的 \0 */

        /* sendto 返回值 = 交给内核的字节数。
           它成功只代表“本机内核已经把这条数据报发出去了”，
           并不代表接收方的应用程序收到了 —— UDP 没有确认也没有重传 */
        ssize_t r = sendto(fd, buf, sizeof buf, 0,
                           (struct sockaddr *)&dst, sizeof dst);
        if (r == MSG_SIZE) ok++;
        else               perror("sendto");

        usleep(INTERVAL_US);
    }

    printf("发送完毕: 共 %d 条，每条 %d 字节，间隔 %d 毫秒，sendto 成功 %d 条\n",
           MSG_COUNT, MSG_SIZE, INTERVAL_US / 1000, ok);
    close(fd);
    return 0;
}
