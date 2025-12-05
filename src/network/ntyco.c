#include "nty_coroutine.h"

#include <arpa/inet.h>
#include "kvs_api.h"
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

    int first_packet_checked;//是否做过对齐检查

    int is_replication;//是否是复制连接
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

	// 初始化接收缓冲区（初始1MB）
    if (n_init_large_buffer(&ctx->recv_buf, 1024 * 1024) < 0) return -1;
    
    // 初始化发送缓冲区（初始1MB）
    if (n_init_large_buffer(&ctx->send_buf, 1024 * 1024) < 0) {
        n_free_large_buffer(&ctx->recv_buf);
        return -1;
    }

    ctx->first_packet_checked = 0;   // 新增初始化
    ctx->processing = 0;
    
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

#if 0
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
#else
int ntyco_process_received_data(int fd, conn_context_t *ctx) {
    if (!ctx) return 0;

    if (ctx->processing) {
        return 0; // 已经在处理中，避免重入
    }
    ctx->processing = 1;

    resp_parser_t *parser = &ctx->parser;

    // 如果上次刚好停在 COMPLETE 状态，说明上一条命令已处理完，重置状态
    if (parser->state == RESP_PARSE_COMPLETE) {
        resp_parser_reset(parser);
    }

    // 只在第一次做简单对齐检查：保证命令从 '*' 开始
    if (!ctx->first_packet_checked &&
        ctx->recv_buf.size > 0 &&
        parser->state == RESP_PARSE_INIT) {

        size_t i = 0;
        while (i < ctx->recv_buf.size &&
               (ctx->recv_buf.data[i] == '\r' ||
                ctx->recv_buf.data[i] == '\n' ||
                ctx->recv_buf.data[i] == ' '  ||
                ctx->recv_buf.data[i] == '\t')) {
            i++;
        }

        if (i >= ctx->recv_buf.size) {
            // 全是空白，等更多数据
            ctx->processing = 0;
            return 0;
        }

        if (ctx->recv_buf.data[i] != '*') {
            printf("ntyco protocol alignment error: first non-space char is '%c' (0x%02X), fd=%d\n",
                   ctx->recv_buf.data[i],
                   (unsigned char)ctx->recv_buf.data[i],
                   fd);
            ctx->processing = 0;
            return -1;
        }

        if (i > 0) {
            memmove(ctx->recv_buf.data,
                    ctx->recv_buf.data + i,
                    ctx->recv_buf.size - i);
            ctx->recv_buf.size -= i;
        }
        ctx->first_packet_checked = 1;
    }

    size_t parsed_total  = 0;  // 本轮共消费的字节数
    int    command_count = 0;  // 本轮解析出的命令数

    while (parsed_total < ctx->recv_buf.size) {
        size_t      cur_len = ctx->recv_buf.size - parsed_total;
        const char *cur_buf = ctx->recv_buf.data + parsed_total;

        // 1. 用无状态 reader 探测：当前 offset 是否有完整命令
        int cmd_len = resp_peek_command(ctx->recv_buf.data,
                                        ctx->recv_buf.size,
                                        parsed_total);
        if (cmd_len < 0) {
            printf("ntyco resp_peek_command error for fd %d\n", fd);
            ctx->processing = 0;
            return -1;
        } else if (cmd_len == 0) {
            // 半包：当前 offset 这条命令还不完整，等待更多数据
            break;
        }

        // 2. 确认 [parsed_total, parsed_total+cmd_len) 是一整条命令
        if (parser->state != RESP_PARSE_INIT &&
            parser->state != RESP_PARSE_COMPLETE) {
            // 理论上不该发生，保险起见 reset 一下
            resp_parser_reset(parser);
        }

        int parsed = resp_parser(parser,
                                 ctx->recv_buf.data + parsed_total,
                                 (size_t)cmd_len);
        if (parsed < 0) {
            printf("ntyco protocol parse error for fd %d\n", fd);
            printf("ntyco protocol parse error for fd %d, state=%d\n",
                   fd, parser->state);
            ctx->processing = 0;
            return -1;
        }

        if (parsed != cmd_len || parser->state != RESP_PARSE_COMPLETE) {
            printf("ntyco resp_parser mismatch: cmd_len=%d, parsed=%d, state=%d\n",
                   cmd_len, parsed, parser->state);
            ctx->processing = 0;
            return -1;
        }

        // 3. 一条完整命令解析成功，调用 kvs_handler 生成响应
		char* response_buf = (char* )kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
        if(!response_buf){
            ctx->processing = 0;
            return -1;
        }

        int  resp_len = kvs_handler(parser->tokens,
                                    parser->arg_index,
                                    response_buf);
        if(resp_len <= 0 || resp_len > (int)KVS_MAX_RESPONSE_LENGTH){
            kvs_free(response_buf);
            ctx->processing = 0;
            return -1;
        }

        // 4. 把响应 append 到 send_buf
        if (ctx->send_buf.size + (size_t)resp_len > ctx->send_buf.capacity) {
            size_t needed       = ctx->send_buf.size + (size_t)resp_len;
            size_t new_capacity = ctx->send_buf.capacity;
            while (new_capacity < needed) {
                new_capacity *= 2;
            }
            if (new_capacity > 512 * 1024 * 1024) {
                printf("ntyco send buffer too large for fd %d\n", fd);
                kvs_free(response_buf);
                ctx->processing = 0;
                return -1;
            }
            if (n_expand_large_buffer(&ctx->send_buf, new_capacity) < 0) {
                kvs_free(response_buf);
                ctx->processing = 0;
                return -1;
            }
        }

        memcpy(ctx->send_buf.data + ctx->send_buf.size,
               response_buf, (size_t)resp_len);
        ctx->send_buf.size += (size_t)resp_len;
        kvs_free(response_buf);
        command_count++;

        // 5. 准备解析下一条命令
        parsed_total += (size_t)cmd_len;
        resp_parser_reset(parser);
    }

    // 6. 将完全解析的数据裁剪掉，只剩下未处理的半包
    if (parsed_total > 0) {
        size_t remaining = ctx->recv_buf.size - parsed_total;
        if (remaining > 0) {
            memmove(ctx->recv_buf.data,
                    ctx->recv_buf.data + parsed_total,
                    remaining);
        }
        ctx->recv_buf.size = remaining;
    }

    ctx->processing = 0;
    return command_count;
}

#endif

void server_reader(void *arg) {
    int fd = (int)(intptr_t)arg;
    conn_context_t ctx;

    if (n_init_conn_context(&ctx) < 0) {
        close(fd);
        return;
    }

    // TCP 优化
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    int buf_size = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    while (!g_shutdown_flag) {

        // 1) 确保接收缓冲区有空间
        if (ctx.recv_buf.size + 4096 > ctx.recv_buf.capacity) {
            if (n_expand_large_buffer(&ctx.recv_buf,
                                      ctx.recv_buf.size + 4096) < 0) {
                goto out;
            }
        }

        size_t remaining = ctx.recv_buf.capacity - ctx.recv_buf.size;
        if (remaining == 0) {
            goto out; // 理论上不会
        }

        // 2) 阻塞 recv（不再是非阻塞 + EAGAIN 循环）
        ssize_t n = recv(fd,
                         ctx.recv_buf.data + ctx.recv_buf.size,
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
        ctx.recv_buf.size += (size_t)n;

        // 3) 解析 + 生成响应
        int cmd_cnt = ntyco_process_received_data(fd, &ctx);
        if (cmd_cnt < 0) {
            goto out;
        }

        // 4) 一次性发送当前所有响应
        size_t sent_total = 0;
        while (sent_total < ctx.send_buf.size) {
            ssize_t wn = send(fd,
                              ctx.send_buf.data + sent_total,
                              ctx.send_buf.size - sent_total,
                              0);
            if (wn < 0) {
                if (errno == EINTR) continue;
                perror("send");
                goto out;
            } else if (wn == 0) {
                goto out;
            }
            sent_total += (size_t)wn;
        }

        if (sent_total > 0) {
            ctx.send_buf.size = 0;
        }

        // 不显式 yield，让 NtyCo 的调度在其他协程阻塞时自动切换
        // 如果你希望每轮主动让出一次 CPU，可以取消下面的注释：
        // nty_coroutine_yield(NULL);
    }

out:
    n_free_conn_context(&ctx);
    close(fd);
}



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

    if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
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
        int cli_fd = accept(fd, (struct sockaddr*)&remote, &len);
        if (cli_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // 把每个连接交给一个协程处理
        nty_coroutine *read_co;
        nty_coroutine_create(&read_co,
                             server_reader,
                             (void*)(intptr_t)cli_fd);
    }

    close(fd);
}



int ntyco_start(unsigned short port, msg_handler handler) {

	//int port = atoi(argv[1]);
	kvs_handler = handler;
	//nty_coroutine *co = NULL;
	nty_coroutine_create(&co, server, &port);

	// 修改调度循环，添加关闭标志检测
    while (!g_shutdown_flag) {
        fprintf(stderr, "[ntyco] schedule_run enter, flag=%d\n", g_shutdown_flag);
        nty_schedule_run();
        fprintf(stderr, "[ntyco] schedule_run return, flag=%d\n", g_shutdown_flag);
        usleep(10000);       // 10ms休眠
    }

}




