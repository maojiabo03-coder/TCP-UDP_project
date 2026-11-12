/*
 * qa_server.c —— 小组限时答题服务器（第一、二、三题共用）
 *
 * 编译: gcc qa_server.c -o qa_server
 * 运行: ./qa_server <端口> <地址复用开关 0|1>
 *
 * 服务器终端命令:
 *   /start 题目          发起一轮，限时 10 秒
 *   /list                查看在线成员
 *   /drop 编号 normal    正常移除 (SO_LINGER l_onoff=0, 发 FIN)
 *   /drop 编号 abort     强制移除 (SO_LINGER l_onoff=1,l_linger=0, 发 RST)
 *   /stop                通知成员后正常关闭全部连接与监听并退出
 *
 * 消息约定: 每条 TCP 消息以 \n 结束；接收端累计字节，按行解析。
 *   客户端 -> 服务器:  HELLO 主机名 系统信息
 *                      ANSWER 轮次 内容
 *   服务器 -> 客户端:  直接可读的一行文字，客户端原样显示
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CLIENTS   16
#define BUF_SIZE      2048
#define ROUND_SECONDS 10.0          /* 每轮答题时长 */

typedef struct {
    int  fd;                        /* -1 表示这个位置空闲 */
    int  id;                        /* 成员编号，每次新连接都分配新号 */
    char host[64];                  /* 客户端 gethostname() 的结果 */
    char sys[128];                  /* 客户端 uname() 的结果 */
    char addr[64];                  /* 对端 IP:端口 */
    char buf[BUF_SIZE];             /* 累计收到但还没凑成一行的字节 */
    int  len;
    int  answered;                  /* 本轮是否已有有效提交 */
    char answer[256];               /* 本轮第一次有效提交的内容 */
} Client;

static Client clients[MAX_CLIENTS];
static fd_set master;               /* 主监视集合：stdin + 监听 + 所有客户端 */
static int    maxfd;
static int    listen_fd = -1;
static int    next_id = 1;

static int    round_no = 0;         /* 当前/最近一轮的编号 */
static int    round_active = 0;     /* 是否正在答题 */
static double round_start;          /* 本轮开始时刻（单调时钟，秒） */
static double deadline;             /* 本轮截止时刻 = 开始 + 10 秒，固定不变 */
static char   start_wall[32];       /* 本轮开始的墙上时间，用于显示 */

/* ---------- 小工具 ---------- */

/* 单调时钟：不受系统改时间影响，适合算“还剩多少秒” */
static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* 墙上时间 HH:MM:SS.mmm，用于截图里的开始/结束时间 */
static void wall_clock(char *out, size_t n)
{
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    snprintf(out, n, "%02d:%02d:%02d.%03d",
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
}

/* 给某个连接发一行（自动补 \n） */
static void send_line(int fd, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    /* MSG_NOSIGNAL: 对端已断开时返回错误，而不是让进程被 SIGPIPE 杀掉 */
    send(fd, line, n, MSG_NOSIGNAL);
}

static void broadcast(const char *text)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1)
            send_line(clients[i].fd, "%s", text);
}

static int online_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1) n++;
    return n;
}

static void print_online(void)
{
    printf("[在线成员 %d 人]\n", online_count());
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd == -1) continue;
        printf("  %d号  %s  (%s)  来自 %s  fd=%d\n",
               c->id, c->host[0] ? c->host : "(未报到)", c->sys, c->addr, c->fd);
    }
}

static Client *find_by_id(int id)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd != -1 && clients[i].id == id)
            return &clients[i];
    return NULL;
}

/* 关闭一个连接，并把它从监视集合里拿掉（第三题要求用 FD_CLR） */
static void remove_client(Client *c)
{
    FD_CLR(c->fd, &master);
    close(c->fd);
    c->fd = -1;

    maxfd = listen_fd;                       /* 重新计算最大描述符 */
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd > maxfd) maxfd = clients[i].fd;
}

/* ---------- 答题轮次 ---------- */

static void start_round(const char *question)
{
    if (round_active) {
        printf("第 %d 轮还没结束，请等它截止。\n", round_no);
        return;
    }
    if (online_count() == 0) {
        printf("没有在线成员，无法发题。\n");
        return;
    }
    if (online_count() < 3)
        printf("提示: 当前在线 %d 人（少于 3 人），仍然发题。\n", online_count());

    round_no++;
    round_active = 1;
    round_start  = now_mono();
    deadline     = round_start + ROUND_SECONDS;   /* 截止时刻只在这里定一次 */
    wall_clock(start_wall, sizeof start_wall);

    for (int i = 0; i < MAX_CLIENTS; i++) {       /* 清空上一轮的提交状态 */
        clients[i].answered  = 0;
        clients[i].answer[0] = '\0';
    }

    char msg[512];
    snprintf(msg, sizeof msg,
             "【第 %d 轮】限时 %.0f 秒，题目: %s  （作答: /answer %d 内容）",
             round_no, ROUND_SECONDS, question, round_no);
    broadcast(msg);
    printf("round=%d 开始  时间 %s  题目: %s\n", round_no, start_wall, question);
}

static void end_round(void)
{
    char end_wall[32], line[512];
    double elapsed = now_mono() - round_start;
    wall_clock(end_wall, sizeof end_wall);
    round_active = 0;

    printf("round=%d  elapsed=%.1fs   开始 %s  结束 %s\n",
           round_no, elapsed, start_wall, end_wall);
    snprintf(line, sizeof line, "【第 %d 轮结束】用时 %.1f 秒", round_no, elapsed);
    broadcast(line);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd == -1) continue;
        if (c->answered)
            snprintf(line, sizeof line, "%d号 %s  已提交  答案: %s",
                     c->id, c->host, c->answer);
        else
            snprintf(line, sizeof line, "%d号 %s  未提交", c->id, c->host);
        printf("%s\n", line);
        broadcast(line);
    }
}

/* ---------- 处理客户端发来的一行 ---------- */

static void handle_client_line(Client *c, char *line)
{
    if (strncmp(line, "HELLO ", 6) == 0) {
        /* HELLO 主机名 系统信息...  —— 客户端连接后自动发送 */
        char *p  = line + 6;
        char *sp = strchr(p, ' ');
        if (sp) { *sp = '\0'; snprintf(c->sys, sizeof c->sys, "%s", sp + 1); }
        snprintf(c->host, sizeof c->host, "%s", p);
        printf("成员报到: %d号  主机名=%s  系统=%s  来自 %s\n",
               c->id, c->host, c->sys, c->addr);
        send_line(c->fd, "欢迎，你是 %d 号成员 (%s)", c->id, c->host);

    } else if (strncmp(line, "ANSWER ", 7) == 0) {
        /* ANSWER 轮次 内容 */
        int r = 0, off = 0;
        if (sscanf(line + 7, "%d %n", &r, &off) < 1 || line[7 + off] == '\0') {
            send_line(c->fd, "格式错误，应为: /answer 轮次 内容");
            return;
        }
        const char *content = line + 7 + off;

        if (!round_active || r != round_no) {
            /* 轮次对不上，或者本轮已截止 —— 迟到答案按已结束处理 */
            printf("拒绝: %d号 %s 提交第 %d 轮答案，但该轮已结束（迟到）\n",
                   c->id, c->host, r);
            send_line(c->fd, "拒绝: 第 %d 轮已结束，迟到答案无效", r);
        } else if (c->answered) {
            /* 每人每轮只保留第一次有效提交 */
            send_line(c->fd, "拒绝: 第 %d 轮你已提交过，保留第一次答案", r);
        } else {
            c->answered = 1;
            snprintf(c->answer, sizeof c->answer, "%s", content);
            printf("记录: round=%d  %d号 %s  +%.1fs  答案: %s\n",
                   round_no, c->id, c->host, now_mono() - round_start, c->answer);
            send_line(c->fd, "已记录第 %d 轮答案", r);
        }
    } else {
        send_line(c->fd, "未知消息: %s", line);
    }
}

/* 某个客户端可读：recv 一次，累计到缓冲区，凑齐一行处理一行 */
static void handle_client_readable(Client *c)
{
    ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len, 0);

    if (n > 0) {
        c->len += n;
        char *nl;
        while ((nl = memchr(c->buf, '\n', c->len)) != NULL) {
            *nl = '\0';
            handle_client_line(c, c->buf);
            int used = (nl - c->buf) + 1;          /* 这一行占了多少字节 */
            c->len -= used;
            memmove(c->buf, nl + 1, c->len);       /* 剩下的挪到开头 */
        }
        if (c->len >= (int)sizeof(c->buf) - 1)     /* 一行太长，丢弃 */
            c->len = 0;
    } else if (n == 0) {
        printf("%d号 %s 正常离线 (recv 返回 0)\n", c->id, c->host);
        remove_client(c);
        print_online();
    } else if (errno != EINTR && errno != EAGAIN) {
        printf("%d号 %s 异常断开: errno=%d (%s)\n",
               c->id, c->host, errno, strerror(errno));
        remove_client(c);
        print_online();
    }
}

/* ---------- 新连接 ---------- */

static void accept_client(void)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) { perror("accept"); return; }

    Client *c = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd == -1) { c = &clients[i]; break; }
    if (!c) {
        send_line(fd, "服务器已满");
        close(fd);
        return;
    }

    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->id = next_id++;                              /* 分配成员编号 */
    snprintf(c->addr, sizeof c->addr, "%s:%d",
             inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

    FD_SET(fd, &master);                            /* 加入监视集合 */
    if (fd > maxfd) maxfd = fd;
    printf("新连接: 分配 %d号  来自 %s  fd=%d\n", c->id, c->addr, fd);
}

/* ---------- 第三题：/drop ---------- */

static void drop_member(int id, const char *mode)
{
    Client *c = find_by_id(id);
    if (!c) { printf("没有 %d 号在线成员。\n", id); return; }

    int abort_mode = (strcmp(mode, "abort") == 0);
    if (!abort_mode && strcmp(mode, "normal") != 0) {
        printf("用法: /drop 编号 normal|abort\n");
        return;
    }

    /* SO_LINGER 只设在“目标连接”上，监听套接字和其他连接不受影响 */
    struct linger lg;
    lg.l_onoff  = abort_mode ? 1 : 0;   /* normal: 关闭 linger，close 走正常四次挥手 (FIN) */
    lg.l_linger = 0;                    /* abort : 开启且等待 0 秒，close 直接发 RST   */
    if (setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg) < 0)
        perror("setsockopt SO_LINGER");

    struct linger chk;
    socklen_t len = sizeof chk;
    getsockopt(c->fd, SOL_SOCKET, SO_LINGER, &chk, &len);
    printf("/drop %d %s: SO_LINGER 实际值 l_onoff=%d l_linger=%d\n",
           id, mode, chk.l_onoff, chk.l_linger);

    if (!abort_mode) {
        /* 正常移除前先把它已发来的数据读完：
           如果接收缓冲区里还有没读的数据就 close，内核会发 RST 而不是 FIN */
        char tmp[512];
        while (recv(c->fd, tmp, sizeof tmp, MSG_DONTWAIT) > 0)
            ;
    }

    printf("移除 %d号 %s (%s)\n", c->id, c->host, abort_mode ? "强制/RST" : "正常/FIN");
    remove_client(c);                   /* 里面做 FD_CLR + close */
    print_online();
}

/* ---------- 第二题：/stop ---------- */

static void stop_server(void)
{
    printf("停服: 通知成员并正常关闭全部连接...\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &clients[i];
        if (c->fd == -1) continue;
        send_line(c->fd, "服务器即将停服，连接将正常关闭");
        char tmp[512];
        while (recv(c->fd, tmp, sizeof tmp, MSG_DONTWAIT) > 0)
            ;
        close(c->fd);       /* 服务器先 close = 主动关闭方，之后这条连接在服务器侧进入 TIME_WAIT */
        c->fd = -1;
    }
    close(listen_fd);
    printf("监听已关闭，服务器退出。用 ss -tan 查看 TIME-WAIT。\n");
    exit(0);
}

/* ---------- 服务器终端命令 ---------- */

static void handle_command(char *line)
{
    if (strncmp(line, "/start", 6) == 0) {
        char *q = line + 6;
        while (*q == ' ') q++;
        if (*q == '\0') printf("用法: /start 题目\n");
        else            start_round(q);
    } else if (strcmp(line, "/stop") == 0) {
        stop_server();
    } else if (strcmp(line, "/list") == 0) {
        print_online();
    } else if (strncmp(line, "/drop ", 6) == 0) {
        int  id;
        char mode[16];
        if (sscanf(line + 6, "%d %15s", &id, mode) == 2) drop_member(id, mode);
        else printf("用法: /drop 编号 normal|abort\n");
    } else if (line[0] != '\0') {
        printf("命令: /start 题目 | /list | /drop 编号 normal|abort | /stop\n");
    }
}

/* 终端输入也用 read + 累计 + 按行解析，和套接字的处理方式一致 */
static void handle_stdin(void)
{
    static char buf[BUF_SIZE];
    static int  len = 0;

    ssize_t n = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
    if (n <= 0) {                        /* 终端被关掉：不再监视 stdin */
        FD_CLR(STDIN_FILENO, &master);
        return;
    }
    len += n;
    char *nl;
    while ((nl = memchr(buf, '\n', len)) != NULL) {
        *nl = '\0';
        if (nl > buf && nl[-1] == '\r') nl[-1] = '\0';
        handle_command(buf);
        int used = (nl - buf) + 1;
        len -= used;
        memmove(buf, nl + 1, len);
    }
    if (len >= (int)sizeof(buf) - 1) len = 0;
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "用法: %s <端口> <地址复用 0|1>\n", argv[0]);
        return 1;
    }
    int port  = atoi(argv[1]);
    int reuse = atoi(argv[2]) ? 1 : 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    /* 第二题：SO_REUSEADDR 必须在 bind() 之前设置，
       因为“端口能不能用”就是在 bind() 那一刻检查的 */
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0)
        perror("setsockopt SO_REUSEADDR");

    int on = -1;
    socklen_t len = sizeof on;
    if (getsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, &len) < 0)
        perror("getsockopt SO_REUSEADDR");
    printf("SO_REUSEADDR 申请值=%d  实际值=%d\n", reuse, on);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    int rc = bind(listen_fd, (struct sockaddr *)&addr, sizeof addr);
    printf("bind() 返回值=%d\n", rc);
    if (rc < 0) {
        perror("bind 失败原因");          /* 典型: Address already in use */
        return 1;
    }
    if (listen(listen_fd, 8) < 0) { perror("listen"); return 1; }

    printf("服务器已启动，端口 %d，等待成员连接。\n", port);
    printf("命令: /start 题目 | /list | /drop 编号 normal|abort | /stop\n");

    FD_ZERO(&master);
    FD_SET(STDIN_FILENO, &master);
    FD_SET(listen_fd, &master);
    maxfd = listen_fd;

    for (;;) {
        fd_set rfds = master;            /* select 会改写集合，所以每次用副本 */
        struct timeval tv, *ptv = NULL;  /* 没有答题时 NULL = 一直等 */

        if (round_active) {
            /* 关键：每次都用“固定截止时刻 - 现在”算剩余时间。
               如果每次都写死 10 秒，那么每收到一个答案就会重新等 10 秒，
               答题窗口会被不断延长 */
            double remain = deadline - now_mono();
            if (remain < 0) remain = 0;
            tv.tv_sec  = (long)remain;
            tv.tv_usec = (long)((remain - tv.tv_sec) * 1e6);
            ptv = &tv;
        }

        int n = select(maxfd + 1, &rfds, NULL, NULL, ptv);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* 先看是否到点：到点就结束本轮，此后才到的答案都算迟到 */
        if (round_active && now_mono() >= deadline)
            end_round();

        /* 1) 各客户端的数据 */
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd != -1 && FD_ISSET(clients[i].fd, &rfds))
                handle_client_readable(&clients[i]);

        /* 2) 新连接 */
        if (FD_ISSET(listen_fd, &rfds))
            accept_client();

        /* 3) 管理员在终端输入的命令 */
        if (FD_ISSET(STDIN_FILENO, &rfds))
            handle_stdin();
    }
    return 0;
}
