


#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h> 
#include <netinet/tcp.h>
#include "kvs_api.h"
#include "resp_protocol.h"


#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2

extern int g_shutdown_flag; // 声明全局关闭标志
//typedef int (*msg_handler)(char *msg, int length, char *response);
//typedef int (*msg_handler)(char *msg, int length, char *response);
typedef int (*msg_handler)(char **tokens, int count, char *response);
static msg_handler kvs_handler;

// 大缓冲区管理结构（与reactor.c保持一致）
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} large_buffer_t;

typedef struct conn_info{
	int fd;
    int event;

	large_buffer_t recv_buf;   // 接收缓冲区
    large_buffer_t send_buf;   // 发送缓冲区
    size_t send_offset;        // 发送偏移量

	//RESP解析器
	resp_parser_t parser;
	int parser_initialized;

	int processing;//当前处理状态
    int first_packet_checked; //是否做过对齐检查

    //当前是否有还在发送过程中的 READ/WRITE SQE
    int read_in_flight;
    int write_in_flight;
} conn_extension_t;

typedef struct{
    int op; // EVENT_ACCEPT / EVENT_READ / EVENT_WRITE
    conn_extension_t* ctx;// 对于 ACCEPT，ctx 只存 fd；对于 READ/WRITE，是连接上下文
} io_uring_op_t;

// READ 和 WRITE 解耦：不再用 ctx->event 互斥 READ/WRITE；
// 始终保持 Read SQE 在队列里（只要连接没关闭）；
// WRITE 作为附加操作，通过 write_in_flight 控制“同一时刻最多一个 WRITE SQE”；



// 初始化大缓冲区
static int p_init_large_buffer(large_buffer_t *buf, size_t initial_size) {
    buf->data = kvs_malloc(initial_size);
    if (!buf->data) return -1;
    
    buf->size = 0;
    buf->capacity = initial_size;
    return 0;
}

// 扩展大缓冲区
static int p_expand_large_buffer(large_buffer_t *buf, size_t min_size) {
    size_t new_capacity = buf->capacity * 2;
    while (new_capacity < min_size) {
        new_capacity *= 2;
    }
    
    char *new_data = kvs_realloc(buf->data, new_capacity);
    if (!new_data) return -1;
    
    buf->data = new_data;
    buf->capacity = new_capacity;
    return 0;
}

// 释放大缓冲区
static void p_free_large_buffer(large_buffer_t *buf) {
    if (buf->data) {
        kvs_free(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

// 设置非阻塞IO
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
// 初始化连接扩展
static int p_init_conn_extension(conn_extension_t *ext, int fd) {
    memset(ext, 0, sizeof(conn_extension_t));
    ext->fd = fd;
    
    // 初始化接收缓冲区 (初始16KB)
    if (p_init_large_buffer(&ext->recv_buf, 16 * 1024) < 0) return -1;
    
    // 初始化发送缓冲区 (初始16KB)
    if (p_init_large_buffer(&ext->send_buf, 16 * 1024) < 0) {
        p_free_large_buffer(&ext->recv_buf);
        return -1;
    }
    
    // 初始化RESP解析器
    resp_parser_init(&ext->parser);
    ext->parser_initialized = 1;
    
    // 设置TCP优化选项
    p_set_nonblocking(fd);
    p_set_tcp_nodelay(fd);
    p_set_tcp_buffer_size(fd, 1024 * 1024, 1024 * 1024); // 1MB缓冲区

    ext->read_in_flight = 0;
    ext->write_in_flight = 0;
    
    return 0;
}

// 释放连接扩展
static void p_free_conn_extension(conn_extension_t *ext) {
    if (!ext) return;
    
    p_free_large_buffer(&ext->recv_buf);
    p_free_large_buffer(&ext->send_buf);
    
    
    if (ext->parser_initialized) {
        resp_parser_free(&ext->parser);
        ext->parser_initialized = 0;
    }
    
    kvs_free(ext);
}

static int p_process_received_data(conn_extension_t *ext) {
    if (ext->processing) {
        return 0;
    }
    ext->processing = 1;

    resp_parser_t *parser = &ext->parser;

    // 如果上一次刚好 COMPLETE，说明上一条命令处理完，重置状态
    if (parser->state == RESP_PARSE_COMPLETE) {
        resp_parser_reset(parser);
    }

    // 只在第一次对齐：保证第一个非空白是 '*'
    if (!ext->first_packet_checked &&
        ext->recv_buf.size > 0 &&
        parser->state == RESP_PARSE_INIT) {

        size_t i = 0;
        while (i < ext->recv_buf.size &&
               (ext->recv_buf.data[i] == '\r' ||
                ext->recv_buf.data[i] == '\n' ||
                ext->recv_buf.data[i] == ' '  ||
                ext->recv_buf.data[i] == '\t')) {
            i++;
        }

        if (i >= ext->recv_buf.size) {
            // 全是空白，等更多数据
            ext->processing = 0;
            return 0;
        }

        if (ext->recv_buf.data[i] != '*') {
            printf("Proactor protocol alignment error: first non-space char is '%c' (0x%02X), fd=%d\n",
                   ext->recv_buf.data[i],
                   (unsigned char)ext->recv_buf.data[i],
                   ext->fd);
            ext->processing = 0;
            // 上层会关闭 ctx
            return -1;
        }

        if (i > 0) {
            memmove(ext->recv_buf.data,
                    ext->recv_buf.data + i,
                    ext->recv_buf.size - i);
            ext->recv_buf.size -= i;
        }
        ext->first_packet_checked = 1;
    }

    size_t parsed_total  = 0;  // 本轮在 recv_buf 中消费的总字节数
    int    command_count = 0;  // 本轮解析出的命令数
    // const int max_commands_per_batch = 1000000;

    while (parsed_total < ext->recv_buf.size /*&&
           command_count < max_commands_per_batch*/) {

        size_t      cur_len = ext->recv_buf.size - parsed_total;
        const char *cur_buf = ext->recv_buf.data + parsed_total;

        // 1) 用无状态 reader 探测当前 offset 是否有完整命令
        int cmd_len = resp_peek_command(ext->recv_buf.data,
                                        ext->recv_buf.size,
                                        parsed_total);
        if (cmd_len < 0) {
            printf("proactor resp_peek_command error for fd %d\n", ext->fd);
            ext->processing = 0;
            return -1;
        } else if (cmd_len == 0) {
            // 半包：等待更多数据
            break;
        }

        // 2) 确认 [parsed_total, parsed_total+cmd_len) 就是一条完整命令
        if (parser->state != RESP_PARSE_INIT &&
            parser->state != RESP_PARSE_COMPLETE) {
            resp_parser_reset(parser);
        }

        int parsed = resp_parser(parser,
                                 ext->recv_buf.data + parsed_total,
                                 (size_t)cmd_len);
        if (parsed < 0) {
            printf("proactor protocol parse error for fd %d\n", ext->fd);
            printf("proactor protocol parse error for fd %d, state=%d\n",
                   ext->fd, parser->state);
            ext->processing = 0;
            return -1;
        }
        if (parsed != cmd_len || parser->state != RESP_PARSE_COMPLETE) {
            printf("proactor resp_parser mismatch: cmd_len=%d, parsed=%d, state=%d\n",
                   cmd_len, parsed, parser->state);
            ext->processing = 0;
            return -1;
        }

        // 3) 一条完整命令解析成功，调用 kvs_handler 生成响应
        char* response_buf = (char* )kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
        if(!response_buf){
            ext->processing = 0;
            return -1;
        }

        int  resp_len = kvs_handler(parser->tokens,
                                    parser->arg_index,
                                    response_buf);
        if(resp_len <= 0 || resp_len > (int)KVS_MAX_RESPONSE_LENGTH){
            kvs_free(response_buf);
            ext->processing = 0;
            return -1;
        }

        // 4) 把响应 append 到 send_buf，按需扩容
        if (ext->send_buf.size + (size_t)resp_len > ext->send_buf.capacity) {
            size_t needed      = ext->send_buf.size + (size_t)resp_len;
            size_t new_capacity = ext->send_buf.capacity;
            while (new_capacity < needed) {
                new_capacity *= 2;
            }
            if (new_capacity > 512 * 1024 * 1024) {
                printf("Proactor send buffer too large for fd %d\n", ext->fd);
                kvs_free(response_buf);
                ext->processing = 0;
                return -1;
            }
            if (p_expand_large_buffer(&ext->send_buf, new_capacity) < 0) {
                kvs_free(response_buf);
                ext->processing = 0;
                return -1;
            }
        }

        memcpy(ext->send_buf.data + ext->send_buf.size,
               response_buf, (size_t)resp_len);
        ext->send_buf.size += (size_t)resp_len;
        kvs_free(response_buf);
        command_count++;

        parsed_total += (size_t)cmd_len;
        resp_parser_reset(parser);
    }

    // 5) 裁掉已经完全解析的前缀，保留半包
    if (parsed_total > 0) {
        size_t remaining = ext->recv_buf.size - parsed_total;
        if (remaining > 0) {
            memmove(ext->recv_buf.data,
                    ext->recv_buf.data + parsed_total,
                    remaining);
        }
        ext->recv_buf.size = remaining;

    }

    ext->processing = 0;
    return command_count;
}



void proactor_wait(void) {
    // 简单实现：等待1秒或直到关闭标志被设置
    struct timespec req = {0, 10000000}; // 10ms
    while (!g_shutdown_flag) {
        nanosleep(&req, NULL);
    }
}


//extern int kvs_protocol(char *msg, int length, char *response);


int p_init_server(unsigned short port) {	

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);	
	if (sockfd < 0) {
        perror("socket");
        return -1;
    }

	// 设置SO_REUSEADDR和SO_REUSEPORT选项
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sockfd);
        return -1;
    }
    
    #ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEPORT");
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
		perror("bind");		
		return -1;	
	}	

	// 设置监听socket为非阻塞
    p_set_nonblocking(sockfd);

	// 设置监听socket为非阻塞
	int flags = fcntl(sockfd, F_GETFL, 0);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	listen(sockfd, 10);
	
	return sockfd;
}





#define ENTRIES_LENGTH		1024

int set_event_recv(struct io_uring *ring, conn_extension_t *ctx) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe) return -1;

	// struct conn_info accept_info = {
	// 	.fd = sockfd,
	// 	.event = EVENT_READ,
	// };

	// 确保有足够空间接收数据
    if (ctx->recv_buf.size + 4096 > ctx->recv_buf.capacity) {
        if (p_expand_large_buffer(&ctx->recv_buf, ctx->recv_buf.size + 4096) < 0) {
            return -1;
        }
    }

    io_uring_op_t* op = kvs_malloc(sizeof(io_uring_op_t));
    if(!op) return -1;

    op->op  = EVENT_READ;
    op->ctx = ctx;
	
	io_uring_prep_recv(sqe, ctx->fd, 
                      ctx->recv_buf.data + ctx->recv_buf.size, 
                      ctx->recv_buf.capacity - ctx->recv_buf.size - 1, 0);
	//memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));
	// io_uring_sqe_set_data(sqe, ctx);
    io_uring_sqe_set_data(sqe, op);

    ctx->read_in_flight = 1;

}


//int set_event_send(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags)
int set_event_send(struct io_uring *ring, conn_extension_t *ctx) {
    if (ctx->write_in_flight) return 0; // 已经有一个 WRITE 在飞
    if (ctx->send_offset >= ctx->send_buf.size) return 0; // 无数据可发

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if(!sqe) return -1;

	if(ctx->send_offset >= ctx->send_buf.size) return 0;//没有数据需要发送

	size_t remaining = ctx->send_buf.size - ctx->send_offset;

    io_uring_op_t *op = kvs_malloc(sizeof(io_uring_op_t));
    if (!op) return -1;

    op->op  = EVENT_WRITE;
    op->ctx = ctx;

    io_uring_prep_send(sqe,
                       ctx->fd,
                       ctx->send_buf.data + ctx->send_offset,
                       remaining,
                       0);// io_uring_sqe_set_data(sqe, ctx);
    io_uring_sqe_set_data(sqe, op);
	
	// io_uring_prep_send(sqe, ctx->fd, ctx->send_buf + ctx->send_offset, to_send, 0);
	// io_uring_sqe_set_data(sqe, ctx);
	//memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));
    ctx->write_in_flight = 1;
	return 0;
}



int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if(!sqe) return -1;

	//创建一个临时上下文用于接受连接
	// conn_extension_t *accept_ctx = kvs_malloc(sizeof(conn_extension_t));
	// if (!accept_ctx) return -1;

	// memset(accept_ctx, 0, sizeof(conn_extension_t));
	// accept_ctx->fd = sockfd;
	// accept_ctx->event = EVENT_ACCEPT;

	// io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
    // io_uring_sqe_set_data(sqe, accept_ctx);

    io_uring_op_t* op = kvs_malloc(sizeof(io_uring_op_t));
    if(!op) return -1;

    op->op  = EVENT_ACCEPT;
    op->ctx = NULL; // 对于 ACCEPT，ctx 暂时不用

    io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
    // io_uring_sqe_set_data(sqe, accept_ctx);
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
    // 有待发送数据且当前没有 WRITE，就再投一个
    if (ctx->send_offset < ctx->send_buf.size && !ctx->write_in_flight) {
        set_event_send(ring, ctx);
    }
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
        perror("io_uring_queue_init_params");
        close(sockfd);
        return -1;
    }

	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
	
	// int nready;
	while (!g_shutdown_flag) {

		if (io_uring_submit(&ring) < 0) {
            perror("io_uring_submit");
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

        #if 0
		if(ctx->event == EVENT_ACCEPT){
			//接收新连接
			//set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
			
			int connfd = cqe->res;
			if(connfd > 0){
				// 创建新连接上下文
                conn_extension_t *new_ctx = kvs_malloc(sizeof(conn_extension_t));
				if(new_ctx && p_init_conn_extension(new_ctx, connfd) == 0){
					new_ctx->event = EVENT_READ;
					set_event_recv(&ring, new_ctx);
				}else{
					if (new_ctx) kvs_free(new_ctx);
					close(connfd);
				}
			}
	

			//初始化新连接上下文
			// struct conn_info *conn_info = malloc(sizeof(struct conn_info));
			// memset(conn_info, 0, sizeof(struct conn_info));

			//开始读取数据
			//set_event_recv(&ring, connfd, conn_info->buffer, sizeof(conn_info->buffer), 0);
			//memcpy(&conn_info.user_data, conn_info, sizeof(struct conn_info));

			// 重新设置接受事件
            kvs_free(ctx);
            set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);

		}else if(ctx->event == EVENT_READ){
			int ret = cqe->res;
			if(ret <= 0){
				// 连接关闭或错误
                p_free_conn_extension(ctx);
                // 确保解析器状态重置
                // if (ctx->parser_initialized) {
                //     resp_parser_reset(&ctx->parser);
                // }
                // kvs_free(ctx);
			}else{
				//info->resp_len = kvs_handler(info->buffer, ret, info->response);
				// set_event_send(&ring, info->fd, info->response, info->resp_len, 0);

				//更新接收数大小
				ctx->recv_buf.size += ret;
				 
				//处理接收到的数据
				int cmd_cnt = p_process_received_data(ctx);
                if(cmd_cnt < 0){
                    // 处理错误，关闭连接
                    p_free_conn_extension(ctx);
                    continue;
                }else{
                    //如果有数据要发送设置发送事件
                    if(ctx->send_buf.size > 0){
                        ctx->event = EVENT_WRITE;
                        set_event_send(&ring, ctx);
                    }else{
                        //继续读取
                        set_event_recv(&ring, ctx);
                    }
                }
			}
		}else if(ctx->event == EVENT_WRITE){
			int ret = cqe->res;
			if(ret <= 0){
				//发送错误 关闭连接
				p_free_conn_extension(ctx);
				// kvs_free(ctx);
			}else{
				//更新已发送偏移量
				ctx->send_offset += ret;

				if(ctx->send_offset >= ctx->send_buf.size){
					//全部发送完毕 重置发送缓冲区
					ctx->send_offset = 0;
					ctx->send_buf.size = 0;

					//继续读取
					ctx->event = EVENT_READ;
					set_event_recv(&ring, ctx);
				}else{
					//还有数据要发送 继续发送
					set_event_send(&ring, ctx);
				}
			}

			// // 发送完成后继续读取
            // set_event_recv(&ring, info->fd, info->buffer + info->buf_len, sizeof(info->buffer) - info->buf_len - 1, 0);
		}

		io_uring_cqe_seen(&ring, cqe);

	}
    #else
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
                ctx->recv_buf.size += n;

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
                if (ctx->send_offset >= ctx->send_buf.size) {
                    // 全部发送完毕，重置发送缓冲
                    ctx->send_offset   = 0;
                    ctx->send_buf.size = 0;
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
    #endif


	close(sockfd);
	io_uring_queue_exit(&ring);
	printf("Proactor network exited\n");

	return 0;

}


