#include "nty_coroutine.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include "kvs_api.h"
#include "resp_protocol.h"
#include "distributed.h"
#include "net_session.h" // 通用会话层

// 单次发送上限：防止一轮循环把时间都耗在 send 上
#define NTYCO_MAX_SEND_CHUNK   (64 * 1024)
// 发送缓冲的“未发送”数据上限：简单背压策略
#define NTYCO_MAX_OUTBUF       (16 * 1024 * 1024)

static nty_coroutine *co = NULL;

typedef int (*msg_handler)(char **tokens, int count, char *response);
static msg_handler kvs_handler;

extern int g_shutdown_flag;

// 管理每个连接的解析状态
typedef struct {
    int fd;                     // 连接描述符
    net_session_t session;      // 通用会话层：封装 recv/send 缓冲区和 RESP 解析器
    size_t send_offset;         // 发送偏移量
    int is_replication;         // 是否是复制连接

    int resp_handshake_done;    // 是否完成 REPLSYNC 响应握手

    // 统计字段
    unsigned long long total_recv_bytes;
    unsigned long long total_send_bytes;
    unsigned long long total_cmd_count;
} conn_context_t;

// 工具函数：设置 TCP 选项
static void set_tcp_options(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    int buf_size = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
}

// net_session 的 handler
static int ntyco_session_handler(resp_parser_t *parser,
                                 char *response_buf,
                                 size_t response_buf_cap,
                                 void *user_data) {

    conn_context_t *ctx = (conn_context_t *)user_data;

    if (!parser || parser->arg_index <= 0 || !parser->tokens[0]) {
        const char *err = "-ERR invalid command\r\n";
        size_t len = strlen(err);
        if (len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    char *cmd_name = parser->tokens[0];

    // 单独处理 REPLSYNC 命令
    if (strcasecmp(cmd_name, "REPLSYNC") == 0) {
        extern distributed_config_t global_dist_config;

        const char *ok_resp  = "+OK\r\n";
        const char *err_resp = "-ERR REPLSYNC not allowed in current role\r\n";

        // printf("[ntyco] REPLSYNC received on fd=%d, current role=%d\n",
        //        ctx ? ctx->fd : -1, global_dist_config.role);

        if (global_dist_config.role == ROLE_SLAVE) {
            // 将该连接标记为复制连接
            ctx->is_replication = 1;

            size_t ok_len = strlen(ok_resp);
            if (ok_len > response_buf_cap) ok_len = response_buf_cap;
            memcpy(response_buf, ok_resp, ok_len);
            printf("REPLSYNC command received, marked connection as replication\n");
            return (int)ok_len;
        } else {
            size_t err_len = strlen(err_resp);
            if (err_len > response_buf_cap) err_len = response_buf_cap;
            memcpy(response_buf, err_resp, err_len);
            return (int)err_len;
        }
    }

    // 其它命令：根据 is_replication 切换同步上下文
    int old_ctx = get_sync_context();
    if (ctx->is_replication) {
        set_sync_context(2);   // 复制连接上下文
    } else {
        set_sync_context(0);   // 普通上下文
    }

    int resp_len = kvs_handler(parser->tokens,
                               parser->arg_index,
                               response_buf);
    set_sync_context(old_ctx);

    if (resp_len < 0 || (size_t)resp_len > response_buf_cap) {
        const char *err = "-ERR internal handler error\r\n";
        size_t len = strlen(err);
        if (len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    return resp_len;
}

// 每个连接的协程
void server_reader(void *arg) {
    extern distributed_config_t global_dist_config;

    int fd = (int)(intptr_t)arg;
    conn_context_t ctx;

    memset(&ctx, 0, sizeof(conn_context_t));
    ctx.fd = fd;
    ctx.send_offset = 0;
    ctx.is_replication = 0;
    ctx.resp_handshake_done = 0;

    // 初始化会话层
    if (net_session_init(&ctx.session,
                         1024 * 1024,
                         1024 * 1024) < 0) {
        close(fd);
        return;
    }

    set_tcp_options(fd);

    while (!g_shutdown_flag) {

        netbuf_t *recv_buf = &ctx.session.recv_buf;

        // 1) 确保接收缓冲区有空间
        size_t min_space = 4096;
        if (recv_buf->size + min_space > recv_buf->capacity) {
            if (netbuf_reserve(recv_buf, recv_buf->size + min_space) < 0) {
                fprintf(stderr,
                        "Failed to reserve space in recv buffer for fd %d\n",
                        fd);
                goto out;
            }
        }

        size_t remaining = recv_buf->capacity - recv_buf->size;
        if (remaining == 0) {
            // 理论上不会发生
            goto out;
        }

        // 2) 使用 nty_recv（协程友好版本）
        ssize_t n = nty_recv(fd,
                             recv_buf->data + recv_buf->size,
                             remaining,
                             0);

        if (n < 0) {
            if (errno == EINTR) {
                continue; // 重试
            }
            perror("recv");
            goto out;
        } else if (n == 0) {
            // 对端关闭
            goto out;
        }

        recv_buf->size += (size_t)n;
        ctx.total_recv_bytes += (unsigned long long)n;

        // 3) 通用会话层解析数据生成响应
        int cmd_cnt = net_session_process_resp(
            fd,
            &ctx.session,
            ntyco_session_handler,
            &ctx);

        if (cmd_cnt < 0) {
            // 解析 / 业务错误，关闭连接
            goto out;
        }

        if (cmd_cnt == 0) {
            // 没有完整命令，继续接收
            continue;
        }
        if (cmd_cnt > 0) {
            ctx.total_cmd_count += (unsigned long long)cmd_cnt;
        }

        // 4) 发送当前所有待发送数据
        netbuf_t *send_buf = &ctx.session.send_buf;

        if (ctx.is_replication) {

            // 对于复制连接：完成 REPLSYNC 握手后，不再返回任何响应
            if (!ctx.resp_handshake_done) {
                // 第一次发送：把 REPLSYNC 的 +OK\r\n 发出去
                size_t sent_total = 0;
                while (sent_total < send_buf->size) {
                    ssize_t wn = nty_send(fd,
                                          send_buf->data + sent_total,
                                          send_buf->size - sent_total,
                                          0);
                    if (wn < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        perror("ntyco send REPLSYNC");
                        goto out;
                    } else if (wn == 0) {
                        goto out; // 对端关闭
                    }
                    sent_total += (size_t)wn;
                }

                if (sent_total > 0) {
                    ctx.total_send_bytes += (unsigned long long)sent_total;
                    send_buf->size = 0;
                }

                ctx.resp_handshake_done = 1;
            } else {
                // 之后所有来自 ebpf_syncd 的命令都只在本地执行，不返回响应
                send_buf->size = 0;
            }

        } else {

            // 普通连接：按 offset + 限制单轮发送长度的方式发送
            // 当前还有多少“未发送”的数据
            size_t outstanding = 0;
            if (send_buf->size > ctx.send_offset) {
                outstanding = send_buf->size - ctx.send_offset;
            }

            // 简单背压：未发送数据过大则直接关闭连接
            if (outstanding > NTYCO_MAX_OUTBUF) {
                goto out;
            }

            if (outstanding > 0) {
                size_t to_send = outstanding;
                if (to_send > NTYCO_MAX_SEND_CHUNK) {
                    to_send = NTYCO_MAX_SEND_CHUNK;
                }

                ssize_t wn = nty_send(fd,
                                      send_buf->data + ctx.send_offset,
                                      to_send,
                                      0);
                if (wn < 0) {
                    if (errno == EINTR) {
                        // 本轮重来一次
                        continue;
                    }
                    perror("ntyco send");
                    goto out;
                } else if (wn == 0) {
                    goto out; // 对端关闭
                }

                ctx.send_offset      += (size_t)wn;
                ctx.total_send_bytes += (unsigned long long)wn;

                // 如果当前缓冲区的数据都发完了，就清空
                if (ctx.send_offset >= send_buf->size) {
                    send_buf->size = 0;
                    ctx.send_offset = 0;
                }
            }
        }
    }

out:
    net_session_free(&ctx.session);
    nty_close(fd);
}

// 监听协程
void server(void *arg) {
    unsigned short port = *(unsigned short *)arg;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in local, remote;
    memset(&local, 0, sizeof(local));
    local.sin_family      = AF_INET;
    local.sin_port        = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;

    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(fd);
        return;
    }

    if (listen(fd, 1024) < 0) {
        perror("listen");
        close(fd);
        return;
    }

    printf("ntyco listen port : %d\n", port);

    while (!g_shutdown_flag) {
        socklen_t len = sizeof(remote);
        int cli_fd = accept(fd, (struct sockaddr *)&remote, &len);
        if (cli_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // 把每个连接交给一个协程处理
        nty_coroutine *read_co;
        nty_coroutine_create(&read_co,
                             server_reader,
                             (void *)(intptr_t)cli_fd);
    }

    close(fd);
}

// ntyco 启动入口
int ntyco_start(unsigned short port, msg_handler handler) {

    kvs_handler = handler;

    nty_coroutine_create(&co, server, &port);

    // 调度循环，支持关闭标志
    while (!g_shutdown_flag) {
        fprintf(stderr, "[ntyco] schedule_run enter, flag=%d\n", g_shutdown_flag);
        nty_schedule_run();
        fprintf(stderr, "[ntyco] schedule_run return, flag=%d\n", g_shutdown_flag);
        usleep(10000);       // 10ms 休眠，避免空转
    }

    return 0;
}
