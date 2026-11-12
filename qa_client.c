/*
 * qa_client.c —— 小组限时答题客户端（B/C/D 三人运行同一份程序）
 *
 * 编译: gcc qa_client.c -o qa_client
 * 运行: ./qa_client <服务器名称或IP> <端口>
 *
 * 客户端终端命令:
 *   /answer 轮次 内容     提交答案
 *   /quit                 主动退出
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/utsname.h>

#define BUF_SIZE 2048

/* 用 getaddrinfo() 把“名称或 IP + 端口”解析成地址，逐个尝试连接 */
static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;       /* IPv4 / IPv6 都可以 */
    hints.ai_socktype = SOCK_STREAM;     /* TCP */

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s): %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (p = res; p != NULL; p = p->ai_next) {       /* 结果是一个链表 */
        char ip[NI_MAXHOST];
        getnameinfo(p->ai_addr, p->ai_addrlen, ip, sizeof ip, NULL, 0, NI_NUMERICHOST);

        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            printf("解析 %s -> %s，连接成功\n", host, ip);
            break;
        }
        printf("解析 %s -> %s，连接失败: %s\n", host, ip, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "用法: %s <服务器名称或IP> <端口>\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int sock = connect_to(argv[1], argv[2]);
    if (sock < 0) {
        fprintf(stderr, "无法连接服务器\n");
        return 1;
    }

    /* 获取本机设备信息，连接后自动发给服务器 */
    char hostname[64] = "unknown";
    struct utsname u;
    gethostname(hostname, sizeof hostname - 1);
    if (uname(&u) < 0) { perror("uname"); return 1; }

    char msg[512];
    int  mlen = snprintf(msg, sizeof msg, "HELLO %s %s %s %s\n",
                         hostname, u.sysname, u.release, u.machine);
    send(sock, msg, mlen, MSG_NOSIGNAL);
    printf("本机: 主机名=%s  系统=%s %s %s\n", hostname, u.sysname, u.release, u.machine);
    printf("命令: /answer 轮次 内容 | /quit\n");

    char rbuf[BUF_SIZE];  int rlen = 0;      /* 套接字的累计缓冲 */
    char ibuf[BUF_SIZE];  int ilen = 0;      /* 键盘输入的累计缓冲 */
    int  watch_stdin = 1;

    for (;;) {
        /* 同时等“键盘输入”和“服务器消息”，所以打字的时候也能持续接收 */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        if (watch_stdin) FD_SET(STDIN_FILENO, &rfds);

        if (select(sock + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* ---- 服务器来消息 / 连接结束 ---- */
        if (FD_ISSET(sock, &rfds)) {
            ssize_t n = recv(sock, rbuf + rlen, sizeof(rbuf) - 1 - rlen, 0);

            if (n > 0) {                              /* 累计并按行显示 */
                rlen += n;
                char *nl;
                while ((nl = memchr(rbuf, '\n', rlen)) != NULL) {
                    *nl = '\0';
                    printf("[服务器] %s\n", rbuf);
                    int used = (nl - rbuf) + 1;
                    rlen -= used;
                    memmove(rbuf, nl + 1, rlen);
                }
                if (rlen >= (int)sizeof(rbuf) - 1) rlen = 0;

            } else if (n == 0) {                      /* 对方发了 FIN */
                printf("recv() 返回 0 -> 服务器正常关闭了连接（正常结束）\n");
                break;

            } else {                                  /* n < 0，看 errno */
                int e = errno;
                if (e == EINTR || e == EAGAIN) continue;
                printf("recv() 返回 %zd, errno=%d (%s)", n, e, strerror(e));
                if (e == ECONNRESET) printf(" -> 连接被重置（强制移除）");
                printf("\n");
                break;
            }
        }

        /* ---- 键盘输入 ---- */
        if (watch_stdin && FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, ibuf + ilen, sizeof(ibuf) - 1 - ilen);
            if (n <= 0) { watch_stdin = 0; continue; }   /* 输入关闭后只接收 */
            ilen += n;

            char *nl;
            while ((nl = memchr(ibuf, '\n', ilen)) != NULL) {
                *nl = '\0';
                if (strncmp(ibuf, "/answer ", 8) == 0) {
                    /* /answer 轮次 内容  ->  ANSWER 轮次 内容\n */
                    mlen = snprintf(msg, sizeof msg, "ANSWER %s\n", ibuf + 8);
                    if (send(sock, msg, mlen, MSG_NOSIGNAL) < 0)
                        printf("send 失败: errno=%d (%s)\n", errno, strerror(errno));
                } else if (strcmp(ibuf, "/quit") == 0) {
                    close(sock);
                    return 0;
                } else if (ibuf[0] != '\0') {
                    printf("命令: /answer 轮次 内容 | /quit\n");
                }
                int used = (nl - ibuf) + 1;
                ilen -= used;
                memmove(ibuf, nl + 1, ilen);
            }
            if (ilen >= (int)sizeof(ibuf) - 1) ilen = 0;
        }
    }

    close(sock);
    return 0;
}
