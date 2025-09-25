#include "kvstore.h"
#include "nty_coroutine.h"

#include <arpa/inet.h>
#include "resp_protocol.h"
#include <fcntl.h>
#include <arpa/inet.h>

static nty_coroutine *co = NULL;


//typedef int (*msg_handler)(char *msg, int length, char *response);
//typedef int (*msg_handler)(char *msg, int length, char *response);
typedef int (*msg_handler)(char **tokens, int count, char *response);
static msg_handler kvs_handler;
extern int g_shutdown_flag;

//大缓冲区管理结构
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} large_buffer_t;

//管理每个连接的解析状态
// 连接上下文结构，用于管理每个连接的解析状态
typedef struct {
    resp_parser_t parser;
    // char *recv_buf;          // 接收缓冲区
    // size_t recv_size;        // 当前接收数据大小
    // size_t recv_capacity;    // 缓冲区容量
    // char *send_buf;          // 发送缓冲区
    // size_t send_size;        // 待发送数据大小
    // size_t send_capacity;    // 发送缓冲区容量
	large_buffer_t recv_buf; //接收缓冲区
	large_buffer_t send_buf; //发送缓冲区
	int processing;          //是否正在处理中
} conn_context_t;

// 初始化大缓冲区
int n_init_large_buffer(large_buffer_t *buf, size_t initial_size) {
    buf->data = kvs_malloc(initial_size);
    if (!buf->data) return -1;
    
    buf->size = 0;
    buf->capacity = initial_size;
    return 0;
}

// 扩展大缓冲区
int n_expand_large_buffer(large_buffer_t *buf, size_t min_size) {
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
void n_free_large_buffer(large_buffer_t *buf) {
    if (buf->data) {
        kvs_free(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

// 初始化连接上下文
int n_init_conn_context(conn_context_t *ctx) {
    memset(ctx, 0, sizeof(conn_context_t));
    resp_parser_init(&ctx->parser);
    
    // 初始化接收缓冲区（初始16KB）
    // ctx->recv_capacity = 16 * 1024;
    // ctx->recv_buf = kvs_malloc(ctx->recv_capacity);
    // if (!ctx->recv_buf) return -1;
    
    // // 初始化发送缓冲区（初始16KB）
    // ctx->send_capacity = 16 * 1024;
    // ctx->send_buf = kvs_malloc(ctx->send_capacity);
    // if (!ctx->send_buf) {
    //     kvs_free(ctx->recv_buf);
    //     return -1;
    // }

	// 初始化接收缓冲区（初始1MB）
    if (n_init_large_buffer(&ctx->recv_buf, 1024 * 1024) < 0) return -1;
    
    // 初始化发送缓冲区（初始1MB）
    if (n_init_large_buffer(&ctx->send_buf, 1024 * 1024) < 0) {
        n_free_large_buffer(&ctx->recv_buf);
        return -1;
    }
    
    return 0;
}

// 释放连接上下文
void n_free_conn_context(conn_context_t *ctx) {
    // if (ctx->recv_buf) kvs_free(ctx->recv_buf);
    // if (ctx->send_buf) kvs_free(ctx->send_buf);
	n_free_large_buffer(&ctx->recv_buf);
    n_free_large_buffer(&ctx->send_buf);
    //resp_parser_free(&ctx->parser);
    // if (ctx->parser_initialized) {
        resp_parser_free(&ctx->parser);
    //     ctx->parser_initialized = 0;
    // }
    memset(ctx, 0, sizeof(conn_context_t));
}

// 处理接收到的数据
int ntyco_process_received_data(int fd, conn_context_t *ctx) {
	if (ctx->processing) {
        return 0; // 已经在处理中，避免重入
    }
    
    ctx->processing = 1;

    int parsed_total = 0;
    int parsed = 0;
    int command_count = 0;
	const int max_commands_per_batch = 10000; // 每批处理最多10000个命令
    
    while (parsed_total < ctx->recv_buf.size && command_count < max_commands_per_batch) {
        // 解析RESP协议
       parsed = resp_parser(&ctx->parser, 
                            ctx->recv_buf.data + parsed_total, 
                            ctx->recv_buf.size - parsed_total);
        
        if (parsed > 0) {
            // 成功解析一个完整命令
            if (ctx->parser.state == RESP_PARSE_COMPLETE) {
                char response_buf[KVS_MAX_RESPONSE_SIZE];
                int resp_len = kvs_handler(ctx->parser.tokens, 
                                          ctx->parser.arg_index, 
                                          response_buf);
                
                // 检查发送缓冲区是否有足够空间
                if (ctx->send_buf.size + resp_len > ctx->send_buf.capacity) {
					if (n_expand_large_buffer(&ctx->send_buf, ctx->send_buf.size + resp_len) < 0) {
						ctx->processing = 0;
						return -1;
					}
				}
                
                // 将响应添加到发送缓冲区
				memcpy(ctx->send_buf.data + ctx->send_buf.size, response_buf, resp_len);
                ctx->send_buf.size += resp_len;
                command_count++;
                
                // 重置解析器状态，准备解析下一个命令
                resp_parser_reset(&ctx->parser);
            }
            
            parsed_total += parsed;
        } else if (parsed == 0) {
            // 需要更多数据
            break;
        } else {
            // 解析错误
            printf("Protocol parse error for fd %d\n", fd);
            
            // 尝试恢复：查找下一个RESP起始符
            const char *next_start = memchr(ctx->recv_buf.data + parsed_total + 1, '*', ctx->recv_buf.size - parsed_total - 1);
            if (next_start) {
                parsed_total = next_start - ctx->recv_buf.data;
                resp_parser_reset(&ctx->parser);
            } else {
                // 没有找到更多RESP数据，清空缓冲区
                parsed_total = ctx->recv_buf.size;
                resp_parser_reset(&ctx->parser);
                break;
            }
        }
    }
    
   // 移动未处理的数据到缓冲区开头
    if (parsed_total > 0) {
        size_t remaining_data = ctx->recv_buf.size - parsed_total;
        if (remaining_data > 0) {
            memmove(ctx->recv_buf.data, 
                    ctx->recv_buf.data + parsed_total, 
                    remaining_data);
        }
        ctx->recv_buf.size = remaining_data;
    }
    
	ctx->processing = 0;
    return command_count;
}

void server_reader(void *arg) {
	int fd = *(int *)arg;
	//int ret = 0;
	conn_context_t ctx;

	//char buffer[4096] = {0};//改变缓冲区大小
	//初始化连接上下文
	if(n_init_conn_context(&ctx) < 0){
		close(fd);
		return;
	}

	// 设置TCP优化选项
    int yes = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) < 0) {
        perror("setsockopt TCP_NODELAY");
    }
    
    // 设置TCP缓冲区大小（发送和接收）
    int buf_size = 1024 * 1024; // 1MB
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size)) < 0) {
        perror("setsockopt SO_SNDBUF");
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size)) < 0) {
        perror("setsockopt SO_RCVBUF");
    }
    
    // 获取实际的缓冲区大小
    socklen_t len = sizeof(buf_size);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, &len) == 0) {
        //printf("Socket %d send buffer size: %d\n", fd, buf_size);
    }
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, &len) == 0) {
        //printf("Socket %d recv buffer size: %d\n", fd, buf_size);
    }
    
    // 设置接收超时（5秒）
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
    }

	while (1) {

		// 检查关闭标志
        if (g_shutdown_flag) {
            break;
        }

		//检查是否需要扩展接收缓冲区
		// if(ctx.recv_size + 4096 > ctx.recv_capacity){
		// 	if(expand_buffer(&ctx.recv_buf, &ctx.recv_capacity, ctx.recv_size + 4096) < 0) break;
		// }
		if (ctx.recv_buf.size + 4096 > ctx.recv_buf.capacity) {
            if (n_expand_large_buffer(&ctx.recv_buf, ctx.recv_buf.size + 4096) < 0) {
                break;
            }
        }

		// 计算剩余空间
        size_t remaining = ctx.recv_buf.capacity - ctx.recv_buf.size;
        if (remaining <= 0) {
            break;
        }
		
		//读取数据
		//ret = recv(fd, buffer, sizeof(buffer) - 1, 0);
		int ret = recv(fd, ctx.recv_buf.data + ctx.recv_buf.size, remaining, 0);

		if (ret > 0) {
            ctx.recv_buf.size += ret;
            
            // 处理接收到的数据
            ntyco_process_received_data(fd, &ctx);
            
            // 发送响应数据
            if (ctx.send_buf.size > 0) {
					size_t sent_total = 0;
					
					while (sent_total < ctx.send_buf.size) {
						ret = send(fd, ctx.send_buf.data + sent_total, ctx.send_buf.size - sent_total, 0);
						
						if (ret <= 0) {
							if (errno == EAGAIN) {
								// 非阻塞模式下重试
								nty_coroutine_yield(co);
								continue;
							}
							break;
						}
						
						sent_total += ret;
					}
					
					// 移动未发送的数据
					if (sent_total > 0) {
						if (sent_total < ctx.send_buf.size) {
							memmove(ctx.send_buf.data, 
									ctx.send_buf.data + sent_total, 
									ctx.send_buf.size - sent_total);
						}
						ctx.send_buf.size -= sent_total;
					}
				}
		} else if (ret == 0) {
			// 连接关闭
			break;
		} else if (errno != EAGAIN) {
			// 错误
			break;
		}

			// 让出协程，避免长时间占用
			//nty_coroutine_yield(co);
		}
	
	// 清理资源
    n_free_conn_context(&ctx);
    close(fd);

}



void server(void *arg) {

	unsigned short port = *(unsigned short *)arg;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;

	// 设置socket为非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	//设置SO_REUSEADDR和SO_REUSEPORT选项
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    #ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    #endif

	struct sockaddr_in local, remote;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = INADDR_ANY;
	
	if (bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in)) < 0) {
        close(fd);
        return;
    }

	listen(fd, 20);
	printf("listen port : %d\n", port);

#if 0
	while (1) {
		socklen_t len = sizeof(struct sockaddr_in);
		int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);

		nty_coroutine *read_co;
		nty_coroutine_create(&read_co, server_reader, &cli_fd);
	}
#else
	while (!g_shutdown_flag) {  // 添加关闭标志检查
        socklen_t len = sizeof(struct sockaddr_in);
        int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
        
		if (g_shutdown_flag) break;

        if (cli_fd > 0) {
			// 设置客户端socket为非阻塞
            flags = fcntl(cli_fd, F_GETFL, 0);
            fcntl(cli_fd, F_SETFL, flags | O_NONBLOCK);

			// 设置TCP优化选项
            int yes = 1;
            setsockopt(cli_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

            nty_coroutine *read_co;
            nty_coroutine_create(&read_co, server_reader, &cli_fd);
        } else {
            // 没有新连接时短暂休眠
            usleep(10000);  // 10ms
        }
    }
#endif
	close(fd);
	
}



int ntyco_start(unsigned short port, msg_handler handler) {

	//int port = atoi(argv[1]);
	kvs_handler = handler;
	//nty_coroutine *co = NULL;
	nty_coroutine_create(&co, server, &port);

	// 修改调度循环，添加关闭标志检测
    while (!g_shutdown_flag) {
        nty_schedule_run();  // 非阻塞执行
        usleep(10000);       // 10ms休眠
    }

}




