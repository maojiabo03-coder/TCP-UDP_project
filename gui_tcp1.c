#include <gtk/gtk.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define PORT 9000
#define MAX_BUFFER (1024 * 1024)

static GtkTextBuffer *logbuf;
static GtkWidget *ip_entry;
static GtkWidget *msg_entry;

static int server_mode;
static int listen_fd = -1;
static int conn_fd = -1;
static int connecting = 0;

static GString *send_queue;
static GString *recv_buffer;

/* 在窗口日志中追加文字 */
static void add_log(const char *text) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(logbuf, &end);
    gtk_text_buffer_insert(logbuf, &end, text, -1);
}

/* 显示系统错误 */
static void log_error(const char *operation) {
    gchar *line = g_strdup_printf(
        "[Error] %s: %s\n", operation, strerror(errno));
    add_log(line);
    g_free(line);
}

/* 设置为非阻塞模式 */
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 关闭当前连接并清理缓冲区 */
static void close_connection(void) {
    if (conn_fd >= 0)
        close(conn_fd);

    conn_fd = -1;
    connecting = 0;

    g_string_truncate(send_queue, 0);
    g_string_truncate(recv_buffer, 0);
}

/* Listen / Connect 按钮 */
static void on_start(GtkButton *button, gpointer data) {
    (void)button;
    (void)data;

    if (server_mode) {
        if (listen_fd >= 0) {
            add_log("[Info] Already listening.\n");
            return;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            log_error("socket");
            return;
        }

        int on = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                       &on, sizeof on) < 0) {
            log_error("setsockopt");
            close(fd);
            return;
        }

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (set_nonblock(fd) < 0 ||
            bind(fd, (struct sockaddr *)&addr,
                 sizeof addr) < 0 ||
            listen(fd, 5) < 0) {
            log_error("listen setup");
            close(fd);
            return;
        }

        listen_fd = fd;
        add_log("[Listen] TCP port 9000, waiting for client...\n");
    } else {
        if (conn_fd >= 0) {
            add_log("[Info] Already connected or connecting.\n");
            return;
        }

        const char *ip =
            gtk_entry_get_text(GTK_ENTRY(ip_entry));

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);

        if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
            add_log("[Error] Please enter a valid IPv4 address.\n");
            return;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            log_error("socket");
            return;
        }

        if (set_nonblock(fd) < 0) {
            log_error("fcntl");
            close(fd);
            return;
        }

        int result = connect(
            fd, (struct sockaddr *)&addr, sizeof addr);

        if (result < 0 && errno != EINPROGRESS) {
            log_error("connect");
            close(fd);
            return;
        }

        conn_fd = fd;
        connecting = (result < 0);

        if (connecting)
            add_log("[TCP] Connecting...\n");
        else
            add_log("[TCP] Connected to server.\n");
    }
}

/* Send 按钮：把消息加入发送队列 */
static void on_send(GtkButton *button, gpointer data) {
    (void)button;
    (void)data;

    if (conn_fd < 0 || connecting) {
        add_log("[Info] Connect first, then send.\n");
        return;
    }

    const char *text =
        gtk_entry_get_text(GTK_ENTRY(msg_entry));

    if (*text == '\0')
        return;

    size_t length = strlen(text);

    if (send_queue->len + length + 1 > MAX_BUFFER) {
        add_log("[Error] Send queue full. Try again later.\n");
        return;
    }

    /* 每条消息以换行符结束 */
    g_string_append(send_queue, text);
    g_string_append_c(send_queue, '\n');

    gchar *line = g_strdup_printf("[TCP Queued] %s\n", text);
    add_log(line);
    g_free(line);

    gtk_entry_set_text(GTK_ENTRY(msg_entry), "");
}

/* 每50毫秒执行一次，处理连接和收发 */
static gboolean poll_net(gpointer data) {
    (void)data;

    /* 服务器等待客户端连接 */
    if (listen_fd >= 0 && conn_fd < 0) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;

        int fd = accept(
            listen_fd, (struct sockaddr *)&peer, &peer_len);

        if (fd >= 0) {
            if (set_nonblock(fd) < 0) {
                log_error("fcntl");
                close(fd);
            } else {
                conn_fd = fd;

                char ip[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &peer.sin_addr,
                          ip, sizeof ip);

                gchar *line = g_strdup_printf(
                    "[TCP] Client connected: %s\n", ip);
                add_log(line);
                g_free(line);
            }
        } else if (errno != EAGAIN &&
                   errno != EWOULDBLOCK &&
                   errno != EINTR) {
            log_error("accept");
            close(listen_fd);
            listen_fd = -1;
        }
    }

    if (conn_fd < 0)
        return G_SOURCE_CONTINUE;

    /* 检查非阻塞 connect 是否完成 */
    if (connecting) {
        struct pollfd pfd = {0};
        pfd.fd = conn_fd;
        pfd.events = POLLOUT;

        int ready = poll(&pfd, 1, 0);

        if (ready < 0) {
            if (errno != EINTR) {
                log_error("poll");
                close_connection();
            }
            return G_SOURCE_CONTINUE;
        }

        if (ready == 0)
            return G_SOURCE_CONTINUE;

        int error = 0;
        socklen_t length = sizeof error;

        if (getsockopt(conn_fd, SOL_SOCKET, SO_ERROR,
                       &error, &length) < 0) {
            log_error("getsockopt");
            close_connection();
            return G_SOURCE_CONTINUE;
        }

        if (error != 0) {
            errno = error;
            log_error("connect");
            close_connection();
            return G_SOURCE_CONTINUE;
        }

        connecting = 0;
        add_log("[TCP] Connected to server.\n");
    }

    /* 发送队列中的数据，未发完的部分留到下一轮 */
    if (send_queue->len > 0) {
        ssize_t n = send(
            conn_fd,
            send_queue->str,
            send_queue->len,
            MSG_NOSIGNAL);

        if (n > 0) {
            g_string_erase(send_queue, 0, n);

            if (send_queue->len == 0)
                add_log("[TCP Sent] Send queue completed.\n");
        } else if (n < 0 &&
                   errno != EAGAIN &&
                   errno != EWOULDBLOCK &&
                   errno != EINTR) {
            log_error("send");
            close_connection();
            return G_SOURCE_CONTINUE;
        }
    }

    /*
     * 每轮最多读取16次，避免持续接收大量数据时
     * 长时间占用窗口事件循环。
     */
    for (int i = 0; i < 16; i++) {
        char buf[4096];
        ssize_t n = recv(conn_fd, buf, sizeof buf, 0);

        if (n > 0) {
            g_string_append_len(recv_buffer, buf, n);

            /* 按换行符分离完整消息 */
            char *newline;
            while ((newline = memchr(
                        recv_buffer->str, '\n',
                        recv_buffer->len)) != NULL) {
                gsize length = newline - recv_buffer->str;

                gchar *message = g_utf8_make_valid(
                    recv_buffer->str, (gssize)length);

                gchar *line = g_strdup_printf(
                    "[TCP Received] %s\n", message);

                add_log(line);
                g_free(line);
                g_free(message);

                g_string_erase(recv_buffer, 0, length + 1);
            }

            if (recv_buffer->len > MAX_BUFFER) {
                add_log("[Error] Incoming message too long.\n");
                close_connection();
                break;
            }
        } else if (n == 0) {
            add_log("[TCP] Peer disconnected.\n");
            close_connection();
            break;
        } else {
            if (errno != EAGAIN &&
                errno != EWOULDBLOCK &&
                errno != EINTR) {
                log_error("recv");
                close_connection();
            }
            break;
        }
    }

    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
    server_mode =
        argc > 1 && strcmp(argv[1], "server") == 0;

    gtk_init(NULL, NULL);

    send_queue = g_string_new("");
    recv_buffer = g_string_new("");

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(
        GTK_WINDOW(win),
        server_mode ? "TCP Server" : "TCP Client");
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 400);

    GtkWidget *box =
        gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(win), box);

    ip_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ip_entry), "127.0.0.1");

    msg_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(msg_entry), "Message");

    GtkWidget *start = gtk_button_new_with_label(
        server_mode ? "Listen" : "Connect");
    GtkWidget *send_btn = gtk_button_new_with_label("Send");

    GtkWidget *log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log), FALSE);
    gtk_text_view_set_wrap_mode(
        GTK_TEXT_VIEW(log), GTK_WRAP_WORD_CHAR);
    logbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log));

    GtkWidget *scroll =
        gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(scroll), log);

    if (!server_mode)
        gtk_container_add(GTK_CONTAINER(box), ip_entry);
    else
        g_object_ref_sink(ip_entry);

    gtk_container_add(GTK_CONTAINER(box), start);
    gtk_container_add(GTK_CONTAINER(box), msg_entry);
    gtk_container_add(GTK_CONTAINER(box), send_btn);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    g_signal_connect(start, "clicked",
                     G_CALLBACK(on_start), NULL);
    g_signal_connect(send_btn, "clicked",
                     G_CALLBACK(on_send), NULL);
    g_signal_connect(win, "destroy",
                     G_CALLBACK(gtk_main_quit), NULL);

    guint timer_id = g_timeout_add(50, poll_net, NULL);

    gtk_widget_show_all(win);
    gtk_main();

    g_source_remove(timer_id);
    close_connection();

    if (listen_fd >= 0)
        close(listen_fd);

    if (server_mode)
        g_object_unref(ip_entry);

    g_string_free(send_queue, TRUE);
    g_string_free(recv_buffer, TRUE);

    return 0;
}
