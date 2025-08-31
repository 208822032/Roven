


#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include "kvstore.h"
#include <fcntl.h> 
#include <netinet/tcp.h>
#include "resp_protocol.h"


#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2

extern int g_shutdown_flag; // 声明全局关闭标志

// 大缓冲区管理结构（与reactor.c保持一致）
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} large_buffer_t;

typedef struct conn_info{
	int fd;
    int event;
    // char buffer[4096];       // 增大缓冲区
    // int buf_len;             // 当前缓冲区数据长度
	// char response[KVS_MAX_RESPONSE_SIZE]; // 响应缓冲区
    // int resp_len;            // 响应长度

	// //接收缓冲区
	// char *recv_buf;
	// size_t recv_size;
	// size_t recv_capacity;

	// //发送缓冲区
	// char *send_buf;
	// size_t send_size;
	// size_t send_capacity;
	//size_t send_offset;//已发送数据的偏移量

	large_buffer_t recv_buf;   // 接收缓冲区
    large_buffer_t send_buf;   // 发送缓冲区
    size_t send_offset;        // 发送偏移量
	struct iovec *iovecs;      // 用于分散/聚集IO
    int iovec_count;           // iovec数量

	//RESP解析器
	resp_parser_t parser;
	int parser_initialized;

	int processing;//当前处理状态
} conn_extension_t;

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
    
    // 分配iovec数组
    ext->iovecs = kvs_malloc(1024 * sizeof(struct iovec));
    if (!ext->iovecs) {
        p_free_large_buffer(&ext->recv_buf);
        p_free_large_buffer(&ext->send_buf);
        return -1;
    }
    
    // 初始化RESP解析器
    resp_parser_init(&ext->parser);
    ext->parser_initialized = 1;
    
    // 设置TCP优化选项
    p_set_nonblocking(fd);
    p_set_tcp_nodelay(fd);
    p_set_tcp_buffer_size(fd, 1024 * 1024, 1024 * 1024); // 1MB缓冲区
    
    return 0;
}

// 释放连接扩展
static void p_free_conn_extension(conn_extension_t *ext) {
    if (!ext) return;
    
    p_free_large_buffer(&ext->recv_buf);
    p_free_large_buffer(&ext->send_buf);
    
    if (ext->iovecs) {
        kvs_free(ext->iovecs);
    }
    
    if (ext->parser_initialized) {
        resp_parser_free(&ext->parser);
    }
    
    kvs_free(ext);
}

//处理接收到的数据
static int p_process_received_data(conn_extension_t *ext) {
    if (ext->processing) {
        return 0;
    }
    
    ext->processing = 1;
    
    int parsed_total = 0;
    int parsed = 0;
    int command_count = 0;
    const int max_commands_per_batch = 10000;
    
    while (parsed_total < ext->recv_buf.size && command_count < max_commands_per_batch) {
        // 解析数据
        parsed = resp_parser(&ext->parser, 
                            ext->recv_buf.data + parsed_total, 
                            ext->recv_buf.size - parsed_total);
        
        if (parsed > 0) {
            // 成功解析一个完整命令
            if (ext->parser.state == RESP_PARSE_COMPLETE) {
                char response_buf[KVS_MAX_RESPONSE_SIZE];
                int resp_len = kvs_filter_protocol(ext->parser.tokens, 
                                                  ext->parser.arg_index, 
                                                  response_buf);
                
                // 检查发送缓冲区是否有足够空间
                if (ext->send_buf.size + resp_len > ext->send_buf.capacity) {
                    if (p_expand_large_buffer(&ext->send_buf, ext->send_buf.size + resp_len) < 0) {
                        ext->processing = 0;
                        return -1;
                    }
                }
                
                // 将响应添加到发送缓冲区
                memcpy(ext->send_buf.data + ext->send_buf.size, response_buf, resp_len);
                ext->send_buf.size += resp_len;
                command_count++;
                
                // 重置解析器状态，准备解析下一个命令
                resp_parser_reset(&ext->parser);
            }
            
            parsed_total += parsed;
        } else if (parsed == 0) {
            // 需要更多数据
            break;
        } else {
            // 解析错误，发送错误响应
            const char *error_msg = "-ERROR Protocol error\r\n";
            int error_len = strlen(error_msg);
            
            if (ext->send_buf.size + error_len > ext->send_buf.capacity) {
                if (p_expand_large_buffer(&ext->send_buf, ext->send_buf.size + error_len) < 0) {
                    ext->processing = 0;
                    return -1;
                }
            }
            
            memcpy(ext->send_buf.data + ext->send_buf.size, error_msg, error_len);
            ext->send_buf.size += error_len;
            
            // 尝试恢复：查找下一个RESP起始符
            const char *next_start = memchr(ext->recv_buf.data + parsed_total + 1, '*', 
                                          ext->recv_buf.size - parsed_total - 1);
            if (next_start) {
                parsed_total = next_start - ext->recv_buf.data;
                resp_parser_reset(&ext->parser);
            } else {
                parsed_total = ext->recv_buf.size;
                break;
            }
        }
    }
    
    // 移动未处理的数据到缓冲区开头
    if (parsed_total > 0) {
        size_t remaining = ext->recv_buf.size - parsed_total;
        if (remaining > 0) {
            memmove(ext->recv_buf.data, ext->recv_buf.data + parsed_total, remaining);
        }
        ext->recv_buf.size = remaining;
    }
    
    ext->processing = 0;
    return command_count;
}

// 准备iovec结构用于分散写入
static int prepare_iovecs(conn_extension_t *ext) {
    if (ext->send_buf.size <= 0 || ext->send_offset >= ext->send_buf.size) {
        ext->iovec_count = 0;
        return 0;
    }
    
    // 将发送缓冲区分割成多个iovec，每个最多4KB
    size_t remaining = ext->send_buf.size - ext->send_offset;
    ext->iovec_count = 0;
    
    while (remaining > 0 && ext->iovec_count < 1024) {
        size_t chunk_size = remaining > 4096 ? 4096 : remaining;
        
        ext->iovecs[ext->iovec_count].iov_base = ext->send_buf.data + ext->send_offset;
        ext->iovecs[ext->iovec_count].iov_len = chunk_size;
        
        ext->send_offset += chunk_size;
        remaining -= chunk_size;
        ext->iovec_count++;
    }
    
    return ext->iovec_count;
}

void proactor_wait(void) {
    // 简单实现：等待1秒或直到关闭标志被设置
    struct timespec req = {0, 10000000}; // 10ms
    while (!g_shutdown_flag) {
        nanosleep(&req, NULL);
    }
}


extern int kvs_protocol(char *msg, int length, char *response);


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
	
	io_uring_prep_recv(sqe, ctx->fd, 
                      ctx->recv_buf.data + ctx->recv_buf.size, 
                      ctx->recv_buf.capacity - ctx->recv_buf.size - 1, 0);
	//memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));
	io_uring_sqe_set_data(sqe, ctx);

}


//int set_event_send(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags)
int set_event_send(struct io_uring *ring, conn_extension_t *ctx) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if(!sqe) return -1;

	if(ctx->send_offset >= ctx->send_buf.size) return 0;//没有数据需要发送

	// 准备iovec
    if (prepare_iovecs(ctx) <= 0) {
        return 0;
    }

	// size_t remaining = ctx->send_size - ctx->send_offset;
	// size_t to_send = remaining > 4096 ? 4096 : remaining;

	// 使用writev进行分散写入
    io_uring_prep_writev(sqe, ctx->fd, ctx->iovecs, ctx->iovec_count, 0);
    io_uring_sqe_set_data(sqe, ctx);
	
	// io_uring_prep_send(sqe, ctx->fd, ctx->send_buf + ctx->send_offset, to_send, 0);
	// io_uring_sqe_set_data(sqe, ctx);
	//memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

	return 0;
}



int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if(!sqe) return -1;

	// struct conn_info accept_info = {
	// 	.fd = sockfd,
	// 	.event = EVENT_ACCEPT,
	// };
	
	// io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
	//memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

	//创建一个临时上下文用于接受连接
	conn_extension_t *accept_ctx = kvs_malloc(sizeof(conn_extension_t));
	if (!accept_ctx) return -1;

	memset(accept_ctx, 0, sizeof(conn_extension_t));
	accept_ctx->fd = sockfd;
	accept_ctx->event = EVENT_ACCEPT;

	io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
    io_uring_sqe_set_data(sqe, accept_ctx);

	return 0;

}


//typedef int (*msg_handler)(char *msg, int length, char *response);
typedef int (*msg_handler)(char *msg, int length, char *response);
static msg_handler kvs_handler;




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

	
#if 0
	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	accept(sockfd, (struct sockaddr*)&clientaddr, &len);
#else

	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
	
#endif

	// char buffer[BUFFER_LENGTH] = {0};
	// char response[BUFFER_LENGTH] = {0};


	// int nready;
	while (!g_shutdown_flag) {

		if (io_uring_submit(&ring) < 0) {
            perror("io_uring_submit");
            break;
        }

#if 0
		struct io_uring_cqe *cqe;
		io_uring_wait_cqe(&ring, &cqe);
#else
		struct io_uring_cqe *cqe;
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms超时
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
		if (ret < 0 || !cqe) continue;  // 超时或错误
#endif
		//struct conn_info *info = (struct conn_info *)&cqe->user_data;

		conn_extension_t *ctx = (conn_extension_t *)io_uring_cqe_get_data(cqe);

		if (g_shutdown_flag) {
			io_uring_cqe_seen(&ring, cqe);
			break;
		}

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
			}else{
				//info->resp_len = kvs_handler(info->buffer, ret, info->response);
				// set_event_send(&ring, info->fd, info->response, info->resp_len, 0);

				//更新接收数大小
				ctx->recv_buf.size += ret;
				 
				//处理接收到的数据
				p_process_received_data(ctx);

				//如果有数据要发送设置发送事件
				if(ctx->send_buf.size > 0){
					ctx->event = EVENT_WRITE;
					set_event_send(&ring, ctx);
				}else{
					//继续读取
					set_event_recv(&ring, ctx);
				}
			}
		}else if(ctx->event == EVENT_WRITE){
			int ret = cqe->res;
			if(ret <= 0){
				//发送错误 关闭连接
				p_free_conn_extension(ctx);
				kvs_free(ctx);
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


	close(sockfd);
	io_uring_queue_exit(&ring);
	printf("Proactor network exited\n");

	return 0;

}


