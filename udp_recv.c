/*
 * udp_recv.c —— UDP 消息积压实验：接收端（第四题）
 *
 * 编译: gcc udp_recv.c -o udp_recv
 * 运行: ./udp_recv <端口> <SO_RCVBUF 申请值>
 *   例: ./udp_recv 5200 4096
 *       ./udp_recv 5200 262144
 *
 * 流程: 绑定 -> 设置并查询 SO_RCVBUF -> 显示 READY -> 暂停（等回车）
 *       -> 发送端发完后按回车 -> 把内核队列里积压的消息读出来
 *       -> 连续 2 秒没有新数据就结束，统计收到的不同编号和缺失编号
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MSG_SIZE  512       /* 每条消息固定 512 字节 */
#define MSG_COUNT 100       /* 编号 1~100 */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "用法: %s <端口> <SO_RCVBUF 申请值>\n", argv[0]);
        return 1;
    }
    int port    = atoi(argv[1]);
    int request = atoi(argv[2]);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);       /* 每轮都是新建的套接字 */
    if (fd < 0) { perror("socket"); return 1; }

    /* 设置接收缓冲区：这是“内核里”替我们暂存数据报的队列容量 */
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &request, sizeof request) < 0)
        perror("setsockopt SO_RCVBUF");

    /* 查询实际生效值：Linux 会把申请值加倍（留给内核记账开销），
       并且受 /proc/sys/net/core/rmem_max 上限限制，所以不一定等于申请值 */
    int actual = 0;
    socklen_t len = sizeof actual;
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual, &len) < 0)
        perror("getsockopt SO_RCVBUF");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }

    printf("SO_RCVBUF 申请值=%d  实际生效值=%d\n", request, actual);
    printf("READY  已绑定 UDP 端口 %d。现在暂停读取：等发送端发完后，按回车开始读取...\n", port);
    fflush(stdout);

    /* 暂停：这段时间程序不调用 recvfrom，到达的数据报只能排在内核接收队列里，
       队列满了以后新到的数据报被内核直接丢弃（发送端并不知道） */
    int ch;
    while ((ch = getchar()) != '\n' && ch != EOF)
        ;

    int  seen[MSG_COUNT + 1] = {0};   /* seen[i]=1 表示编号 i 收到过 */
    int  total = 0;                   /* 收到的数据报总数 */
    char buf[2048];

    for (;;) {
        /* 用 select 等最多 2 秒；2 秒内都没有新数据就认为结束 */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 2, 0 };

        int n = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (n == 0) break;            /* 超时：连续 2 秒没有新数据 */

        ssize_t r = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (r < 0) { perror("recvfrom"); break; }
        buf[r] = '\0';
        total++;

        int seq = 0;
        if (sscanf(buf, "MSG %d", &seq) == 1 && seq >= 1 && seq <= MSG_COUNT)
            seen[seq] = 1;            /* 只统计 1~100 的不同编号 */
    }

    int distinct = 0;
    for (int i = 1; i <= MSG_COUNT; i++)
        if (seen[i]) distinct++;

    printf("\n===== 统计结果 =====\n");
    printf("SO_RCVBUF 申请值=%d  实际生效值=%d\n", request, actual);
    printf("收到数据报总数=%d  不同编号数=%d / %d  缺失=%d 条\n",
           total, distinct, MSG_COUNT, MSG_COUNT - distinct);

    printf("缺失编号:");
    if (distinct == MSG_COUNT) {
        printf(" 无");
    } else {
        /* 连续缺失的编号合并成 a-b 的形式，方便截图 */
        for (int i = 1; i <= MSG_COUNT; i++) {
            if (seen[i]) continue;
            int j = i;
            while (j + 1 <= MSG_COUNT && !seen[j + 1]) j++;
            if (i == j) printf(" %d", i);
            else        printf(" %d-%d", i, j);
            i = j;
        }
    }
    printf("\n");

    close(fd);
    return 0;
}
