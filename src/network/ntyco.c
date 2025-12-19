// ntyco_server.c

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>

#include "nty_coroutine.h"   
#include "net_session.h"     
#include "kvs_api.h"
#include "resp_protocol.h"
#include "distributed.h"

extern int g_shutdown_flag;  

typedef int (*msg_handler)(char **tokens, int count, char *response);
static msg_handler kvs_handler = NULL;

// 每个连接的上下文
typedef struct ntyco_conn_ctx {
    int fd;

    net_session_t session;   // 通用会话：含 recv_buf / send_buf / resp_parser
    size_t send_offset;      // send_buf 当前发送偏移
    int is_replication;      // 是否是复制连接（REPLSYNC 之后置 1）
} ntyco_conn_ctx_t;

extern kvs_config_t g_cfg;


// 设置 TCP_NODELAY
static int ntyco_set_tcp_nodelay(int fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

// 设置 TCP 缓冲区
static int ntyco_set_tcp_buffer_size(int fd, int rcv, int snd) {
    if (rcv > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    }
    if (snd > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    }
    return 0;
}


static int ntyco_init_conn_ctx(ntyco_conn_ctx_t *ctx, int fd) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = fd;
    ctx->send_offset = 0;
    ctx->is_replication = 0;

    // 会话层缓冲区：给一个 8MB recv / 8MB send（和 reactor 保持类似级别）:contentReference[oaicite:5]{index=5}
    if (net_session_init(&ctx->session,
                         8 * 1024 * 1024,
                         8 * 1024 * 1024) < 0) {
        return -1;
    }

    // TCP 优化
    ntyco_set_tcp_nodelay(fd);
    ntyco_set_tcp_buffer_size(fd,
                              8 * 1024 * 1024,
                              8 * 1024 * 1024);
    return 0;
}

static void ntyco_free_conn_ctx(ntyco_conn_ctx_t *ctx) {
    if (!ctx) return;

    net_session_free(&ctx->session);

    if (ctx->fd >= 0) {
        nty_close(ctx->fd);  
        ctx->fd = -1;
    }

    kvs_free(ctx);
}

static int ntyco_session_handler(resp_parser_t *parser,
                                 char *response_buf,
                                 size_t response_buf_cap,
                                 void *user_data) {
    ntyco_conn_ctx_t *ctx = (ntyco_conn_ctx_t *)user_data;

    if (!parser || parser->arg_index <= 0 || !parser->tokens[0]) {
        const char *err = "-ERR invalid command\r\n";
        size_t len = strlen(err);
        if (len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    char *cmd_name = parser->tokens[0];

    // 单独处理 REPLSYNC
    if (g_cfg.sync_mode == KVS_SYNC_EBPF && strcasecmp(cmd_name, "REPLSYNC") == 0) {
        extern distributed_config_t global_dist_config;

        const char *ok_resp  = "+OK\r\n";
        const char *err_resp = "-ERR REPLSYNC not allowed in current role\r\n";

        if (global_dist_config.role == ROLE_SLAVE) {
            ctx->is_replication = 1;
            size_t ok_len = strlen(ok_resp);
            if (ok_len > response_buf_cap) ok_len = response_buf_cap;
            memcpy(response_buf, ok_resp, ok_len);
            return (int)ok_len;
        } else {
            size_t err_len = strlen(err_resp);
            if (err_len > response_buf_cap) err_len = response_buf_cap;
            memcpy(response_buf, err_resp, err_len);
            return (int)err_len;
        }
    }

    int old_ctx = get_sync_context();
    if (ctx->is_replication) {
        set_sync_context(2); // 复制上下文
    } else {
        set_sync_context(0); // 普通上下文
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


static int ntyco_process_received_data(ntyco_conn_ctx_t *ctx) {
    return net_session_process_resp(ctx->fd,
                                    &ctx->session,
                                    ntyco_session_handler,
                                    ctx);
}

static void ntyco_conn_routine(void *arg) {
    ntyco_conn_ctx_t *ctx = (ntyco_conn_ctx_t *)arg;
    netbuf_t *recv_buf = &ctx->session.recv_buf;
    netbuf_t *send_buf = &ctx->session.send_buf;

    struct pollfd pfd;
    pfd.fd = ctx->fd;
    // events 在循环内动态设置

    while (!g_shutdown_flag) {
        // 1. 准备监听事件
        pfd.events = POLLIN; // 始终关注可读
        if (send_buf->size > 0) {
            pfd.events |= POLLOUT; // 有数据才关注可写
        }

        // 2. 使用标准 poll 等待事件 (替代原来的单向逻辑)
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pfd.revents & (POLLERR | POLLHUP)) {
            break;
        }

        // 3. 处理发送 (如果可写且有数据)
        if ((pfd.revents & POLLOUT) && send_buf->size > 0) {
            size_t bytes_left = send_buf->size - ctx->send_offset;

            size_t to_try = bytes_left;

            ssize_t sent = send(ctx->fd,
                                send_buf->data + ctx->send_offset,
                                to_try, 0);
            if (sent > 0) {
                ctx->send_offset += (size_t)sent;
                if (ctx->send_offset == send_buf->size) {
                    ctx->send_offset = 0;
                    send_buf->size = 0;
                }
            } else if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // 缓冲区满，下次循环再试
                } else {
                    break; // 真正的错误
                }
            }
            
        }

        // 4. 处理接收 (如果可读)
        if (pfd.revents & POLLIN) {
            // 扩容检查-
            size_t min_space = 64 * 1024;
            if (recv_buf->capacity - recv_buf->size < min_space) {
                if (netbuf_reserve(recv_buf, recv_buf->size + min_space) < 0) {
                    fprintf(stderr, "Failed to reserve recv buffer for fd %d\n", ctx->fd);
                    break;
                }
            }

            size_t remaining = recv_buf->capacity - recv_buf->size;
            if (remaining == 0) {
                fprintf(stderr, "No recv buffer remaining for fd %d\n", ctx->fd);
                break;
            }

            // 使用原生 recv
            ssize_t n = recv(ctx->fd,
                             recv_buf->data + recv_buf->size,
                             remaining,
                             0);
            if (n > 0) {
                recv_buf->size += (size_t)n;

                while(1) {
                    int cmd_cnt = ntyco_process_received_data(ctx);
                    if (cmd_cnt < 0) {
                        fprintf(stderr, "[ntyco] process error\n");
                        goto out;
                    } else if (cmd_cnt == 0) {
                        break;
                    }
                }
            } else if (n == 0) {
                // 对端关闭
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
            }
        }
    }

out:
    // fprintf(stderr,
    //     "[ntyco][fd=%d] exit loop, errno=%d, recv_buf_size=%zu\n",
    //     ctx->fd, errno, ctx->session.recv_buf.size);
    ntyco_free_conn_ctx(ctx);
}


static void ntyco_accept_routine(void *arg) {
    int listen_fd = (int)(intptr_t)arg;


    while (!g_shutdown_flag) {
        struct sockaddr_in clientaddr;
        socklen_t len = sizeof(clientaddr);
        int connfd = nty_accept(listen_fd,
                                (struct sockaddr *)&clientaddr,
                                &len);  // nty_accept 内部自动 poll+挂起:contentReference[oaicite:11]{index=11}
        if (connfd < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                continue;
            } else {
                // 其他错误，可以选择 break 或继续重试
                fprintf(stderr, "nty_accept error: %s\n", strerror(errno));
                continue;
            }
        }

        ntyco_conn_ctx_t *ctx = (ntyco_conn_ctx_t *)kvs_malloc(sizeof(ntyco_conn_ctx_t));
        if (!ctx) {
            fprintf(stderr, "malloc conn ctx failed\n");
            nty_close(connfd);
            continue;
        }

        if (ntyco_init_conn_ctx(ctx, connfd) < 0) {
            fprintf(stderr, "init conn ctx failed\n");
            nty_close(connfd);
            kvs_free(ctx);
            continue;
        }

        // 为每个连接创建一个协程
        nty_coroutine *co = NULL;
        if (nty_coroutine_create(&co, ntyco_conn_routine, ctx) != 0) {
            fprintf(stderr, "nty_coroutine_create failed\n");
            ntyco_free_conn_ctx(ctx);
            continue;
        }
        // 这里不用手动 resume，调度器会在 nty_schedule_run 里处理 ready 队列:contentReference[oaicite:12]{index=12}
    }
}



static int ntyco_init_server(unsigned short port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);  
    if (sockfd < 0) {
        perror("nty_socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        nty_close(sockfd);
        return -1;
    }

#ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEPORT");
        // 不一定要立刻失败，根据需要决定
    }
#endif

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        nty_close(sockfd);
        return -1;
    }

    if (listen(sockfd, 1024) < 0) {
        perror("listen");
        nty_close(sockfd);
        return -1;
    }

    // 设置 listen fd 为非阻塞
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    return sockfd;
}



int ntyco_start(unsigned short port, msg_handler handler) {
    kvs_handler = handler;

    int listen_fd = ntyco_init_server(port);
    if (listen_fd < 0) {
        return -1;
    }

    // 创建 accept 协程
    nty_coroutine *accept_co = NULL;
    if (nty_coroutine_create(&accept_co,
                             ntyco_accept_routine,
                             (void *)(intptr_t)listen_fd) != 0) {
        fprintf(stderr, "create accept coroutine failed\n");
        nty_close(listen_fd);
        return -1;
    }

    // 进入调度循环：单线程 epoll+协程，直到没有协程可跑或你设置 g_shutdown_flag 后连接都退出:contentReference[oaicite:14]{index=14}
    nty_schedule_run();

    // 调度退出之后，关闭 listen_fd
    nty_close(listen_fd);

    printf("NtyCo network exited\n");
    return 0;
}
