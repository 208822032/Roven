


#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h> 
#include <netinet/tcp.h>

#include "kvs_api.h"
#include "kvs_log.h"
#include "resp_protocol.h"
#include "distributed.h"
#include "net_session.h" //通用会话层


#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2

extern int g_shutdown_flag; // 声明全局关闭标志

typedef int (*msg_handler)(char **tokens, int count, char *response);
static msg_handler kvs_handler;

typedef struct conn_info{
	int fd;

	net_session_t session; //通用会话层 封装recv/send缓冲区和RESP解析器
    size_t send_offset;        // 发送偏移量

    //当前是否有还在发送过程中的 READ/WRITE SQE
    int read_in_flight;
    int write_in_flight;

    int is_replication;//是否是复制连接
} conn_extension_t;

typedef struct{
    int op; // EVENT_ACCEPT / EVENT_READ / EVENT_WRITE
    conn_extension_t* ctx;// 对于 ACCEPT，ctx 只存 fd；对于 READ/WRITE，是连接上下文
} io_uring_op_t;

extern kvs_config_t g_cfg;

//工具函数
static int p_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 设置TCP_NODELAY选项减少小包延迟
static int p_set_tcp_nodelay(int fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

// 设置TCP缓冲区大小
static int p_set_tcp_buffer_size(int fd, int recv_size, int send_size) {
    if (recv_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recv_size, sizeof(recv_size));
    }
    if (send_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size));
    }
    return 0;
}


//初始化连接上下文
static int p_init_conn_extension(conn_extension_t *ext, int fd) {
    memset(ext, 0, sizeof(conn_extension_t));
    ext->fd = fd;
    ext->send_offset = 0;
    ext->is_replication = 0;
    ext->read_in_flight = 0;
    ext->write_in_flight = 0;
    
    //初始化net_session
    if(net_session_init(&ext->session,
                        1024 * 1024,
                        1024 * 1024) < 0){
        return -1;
    }

    //TCP优化
    p_set_nonblocking(fd);
    p_set_tcp_nodelay(fd);
    p_set_tcp_buffer_size(fd, 1024 * 1024, 1024 * 1024); // 1MB缓冲区
    
    return 0;
}

// 释放连接扩展
static void p_free_conn_extension(conn_extension_t *ext) {
    if (!ext) return;
    
    net_session_free(&ext->session);
      
    if (ext->fd >= 0) {
        close(ext->fd);
        ext->fd = -1;
    }
    
    kvs_free(ext);
}

static int proactor_session_handler(resp_parser_t* parser,
                                     char* response_buf,
                                     size_t response_buf_cap,
                                     void* user_data)
{
    conn_extension_t* ext = (conn_extension_t* )user_data;

    if(!parser || parser->arg_index <= 0 || !parser->tokens[0]){
        const char* err = "-ERR invalid command\r\n";
        size_t len = strlen(err);
        if(len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    char* cmd_name = parser->tokens[0];

    // 单独处理 REPLSYNC
    if (g_cfg.sync_mode == KVS_SYNC_EBPF && strcasecmp(cmd_name, "REPLSYNC") == 0) {
        extern distributed_config_t global_dist_config;

        const char *ok_resp  = "+OK\r\n";
        const char *err_resp = "-ERR REPLSYNC not allowed in current role\r\n";

        if (global_dist_config.role == ROLE_SLAVE) {
            ext->is_replication = 1;

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
    if (ext->is_replication) {
        set_sync_context(2);
    } else {
        set_sync_context(0);
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

// === net_session 封装 ===

static int p_process_received_data(conn_extension_t *ext) {
    return net_session_process_resp(ext->fd,
                                    &ext->session,
                                    proactor_session_handler,
                                    ext);
}


void proactor_wait(void) {
    // 简单实现：等待1秒或直到关闭标志被设置
    struct timespec req = {0, 10000000}; // 10ms
    while (!g_shutdown_flag) {
        nanosleep(&req, NULL);
    }
}

#define ENTRIES_LENGTH		1024

int set_event_recv(struct io_uring *ring, conn_extension_t *ctx) {
    if(ctx->read_in_flight) return 0; // 已经有一个 READ 在飞

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe) return -1;

    netbuf_t* recv_buf = &ctx->session.recv_buf;

	// 确保有足够空间接收数据
    size_t min_space = 4096;
    if (recv_buf->size + 4096 > recv_buf->capacity) {
        if(netbuf_reserve(recv_buf, recv_buf->size + min_space) < 0){
            return -1;
        }
    }

    size_t remaining = recv_buf->capacity - recv_buf->size;

    io_uring_op_t* op = kvs_malloc(sizeof(io_uring_op_t));
    if(!op) return -1;

    op->op  = EVENT_READ;
    op->ctx = ctx;
	
	io_uring_prep_recv(sqe, ctx->fd, 
                      recv_buf->data + recv_buf->size, 
                      recv_buf->capacity - recv_buf->size - 1, 0);

    io_uring_sqe_set_data(sqe, op);

    ctx->read_in_flight = 1;

    return 0;
}


int set_event_send(struct io_uring *ring, conn_extension_t *ctx) {
    if (ctx->write_in_flight) return 0; // 已经有一个 WRITE 在飞

    netbuf_t* send_buf = &ctx->session.send_buf;

    if (ctx->send_offset >= send_buf->size) return 0; // 无数据可发

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if(!sqe) return -1;

	size_t remaining = send_buf->size - ctx->send_offset;

    io_uring_op_t *op = kvs_malloc(sizeof(io_uring_op_t));
    if (!op) return -1;

    op->op  = EVENT_WRITE;
    op->ctx = ctx;

    io_uring_prep_send(sqe,
                       ctx->fd,
                       send_buf->data + ctx->send_offset,
                       remaining,
                       0);// io_uring_sqe_set_data(sqe, ctx);
    io_uring_sqe_set_data(sqe, op);
	
    ctx->write_in_flight = 1;
	return 0;
}

int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if(!sqe) return -1;

    io_uring_op_t* op = kvs_malloc(sizeof(io_uring_op_t));
    if(!op) return -1;

    op->op  = EVENT_ACCEPT;
    op->ctx = NULL; // 对于 ACCEPT，ctx 暂时不用

    io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
    io_uring_sqe_set_data(sqe, op);
	return 0;

}

static void post_recv_if_needed(struct io_uring *ring, conn_extension_t *ctx) {
    // 连接还活着且当前没有 READ，就再投一个
    if (!ctx->read_in_flight) {
        set_event_recv(ring, ctx);
    }
}

static void post_send_if_needed(struct io_uring *ring, conn_extension_t *ctx) {
    netbuf_t* send_buf = &ctx->session.send_buf;
    if (ctx->send_offset < send_buf->size && !ctx->write_in_flight) {
        set_event_send(ring, ctx);
    }
}

//服务器监听

int p_init_server(unsigned short port) {	

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);	
	if (sockfd < 0) {
    KVS_LOG_ERROR_ERRNO("network", "socket failed");
        return -1;
    }

	// 设置SO_REUSEADDR和SO_REUSEPORT选项
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        KVS_LOG_ERROR_ERRNO("network", "setsockopt SO_REUSEADDR failed");
        close(sockfd);
        return -1;
    }
    
    #ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        KVS_LOG_ERROR_ERRNO("network", "setsockopt SO_REUSEPORT failed");
        close(sockfd);
        return -1;
    }
    #endif

	struct sockaddr_in serveraddr;	
	memset(&serveraddr, 0, sizeof(struct sockaddr_in));	
	serveraddr.sin_family = AF_INET;	
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);	
	serveraddr.sin_port = htons(port);	

	if (-1 == bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {		
        KVS_LOG_ERROR_ERRNO("network", "bind failed on port %u", port);
        close(sockfd);
		return -1;	
	}	

	// 设置监听socket为非阻塞
    p_set_nonblocking(sockfd);

	if(listen(sockfd, 10)){
        KVS_LOG_ERROR_ERRNO("network", "listen failed on port %u", port);
        close(sockfd);
        return -1;
    }
	
	return sockfd;
}

int proactor_start(unsigned short port, msg_handler handler) {

	int sockfd = p_init_server(port);
	if (sockfd < 0) {
        return -1;
    }
	kvs_handler = handler;


	struct io_uring_params params;
	memset(&params, 0, sizeof(params));
	struct io_uring ring;
	if (io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params) < 0) {
    KVS_LOG_ERROR_ERRNO("network", "io_uring_queue_init_params failed");
        close(sockfd);
        return -1;
    }

	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
	
	// int nready;
	while (!g_shutdown_flag) {

		if (io_uring_submit(&ring) < 0) {
            KVS_LOG_ERROR_ERRNO("network", "io_uring_submit failed");
            break;
        }

		struct io_uring_cqe *cqe;
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms超时
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
		if (ret < 0 || !cqe) continue;  // 超时或错误

		// conn_extension_t *ctx = (conn_extension_t *)io_uring_cqe_get_data(cqe);
        io_uring_op_t *op = (io_uring_op_t *)io_uring_cqe_get_data(cqe);

		if (g_shutdown_flag) {
			io_uring_cqe_seen(&ring, cqe);
            if(op) kvs_free(op);
			break;
		}

        if (!op) {
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

    if (op->op == EVENT_ACCEPT) {
            int connfd = cqe->res;
            if (connfd > 0) {
                conn_extension_t *new_ctx = kvs_malloc(sizeof(conn_extension_t));
                if (new_ctx && p_init_conn_extension(new_ctx, connfd) == 0) {
                    // 新连接：立即投一个 READ
                    set_event_recv(&ring, new_ctx);
                } else {
                    if (new_ctx) kvs_free(new_ctx);
                    close(connfd);
                }
            }
            // 继续监听下一个连接
            set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);

        } else if (op->op == EVENT_READ) {
            conn_extension_t *ctx = op->ctx;
            int n = cqe->res;
            ctx->read_in_flight = 0;

            if (n <= 0) {
                // client 关闭或错误
                p_free_conn_extension(ctx);
            } else {
                netbuf_t* recv_buf = &ctx->session.recv_buf;
                recv_buf->size += n;

                int cmd_cnt = p_process_received_data(ctx);
                if (cmd_cnt < 0) {
                    p_free_conn_extension(ctx);
                } else {
                    // 始终保持至少一个 READ 在队列中
                    post_recv_if_needed(&ring, ctx);
                    // 若有数据要发，附带一个 WRITE
                    post_send_if_needed(&ring, ctx);
                }
            }

        } else if (op->op == EVENT_WRITE) {

            conn_extension_t *ctx = op->ctx;
            int n = cqe->res;
            ctx->write_in_flight = 0;

            if (n <= 0) {
                p_free_conn_extension(ctx);
            } else {
                ctx->send_offset += n;

                netbuf_t* send_buf = &ctx->session.send_buf;
                if (ctx->send_offset >= send_buf->size) {
                    // 全部发送完毕，重置发送缓冲
                    ctx->send_offset   = 0;
                    send_buf->size = 0;
                }
                // 如果还有未发送的内容，再投一个 WRITE
                post_send_if_needed(&ring, ctx);
                // 同时确保 READ 也一直在排队
                post_recv_if_needed(&ring, ctx);
            }
        }

        io_uring_cqe_seen(&ring, cqe);
        kvs_free(op);
    }


	close(sockfd);
	io_uring_queue_exit(&ring);
    KVS_LOG_INFO("network", "Proactor network exited");

	return 0;

}


