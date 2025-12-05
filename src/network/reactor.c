


#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/time.h>

#include <sys/uio.h> // 用于writev
#include <fcntl.h>   // 用于fcntl
#include <netinet/tcp.h>

#include "server.h"
#include "kvs_api.h"
#include "resp_protocol.h"
#include "distributed.h"


#define CONNECTION_SIZE			1024 // 1024 * 1024

#define MAX_PORTS			1

#define MAX_IOVEC_COUNT     1024   //最大iovec数量
#define MAX_SINGLE_SEND     1024 * 1024  //单次发送最大数据量 1MB

//大缓冲区管理结构
typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} large_buffer_t;

//连接扩展结构 用于管理大缓冲区
typedef struct{
    large_buffer_t recv_buf;//接收缓冲区
    large_buffer_t send_buf;//发送缓冲区
    size_t send_offset;//发送偏移量
    int processing;  //是否正在处理中
    struct iovec *iovecs;//用于分散/聚集IO
    int iovec_count;  //iovec数量

    int is_replication;//是否是复制连接
} conn_extension_t;

//全局连接扩展数组
static conn_extension_t conn_extensions[CONNECTION_SIZE] = {0};


#if ENABLE_KVSTORE

int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

typedef int (*msg_handler)(char **tokens, int count, char *response);

static msg_handler kvs_handler;


#endif

int epfd = 0;
struct conn conn_list[CONNECTION_SIZE] = {0};


//初始化大缓冲区
int init_large_buffer(large_buffer_t *buf, size_t initial_size){
    buf->data = kvs_malloc(initial_size);
    if(!buf->data) return -1;

    buf->size = 0;
    buf->capacity = initial_size;
    return 0;
}

//扩展大缓冲区
int expand_large_buffer(large_buffer_t *buf, size_t min_size){
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
void free_large_buffer(large_buffer_t *buf) {
    if (buf->data) {
        kvs_free(buf->data);
        buf->data = NULL;
    }
    buf->size = 0;
    buf->capacity = 0;
}

// 初始化连接扩展
int init_conn_extension(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    
    conn_extension_t *ext = &conn_extensions[fd];
    memset(ext, 0, sizeof(conn_extension_t));
    
    // 初始化接收缓冲区 (初始16MB)
    if (init_large_buffer(&ext->recv_buf, 16 * 1024 * 1024) < 0) return -1;
    
    // 初始化发送缓冲区 (初始16MB)
    if (init_large_buffer(&ext->send_buf, 16 * 1024 * 1024) < 0) {
        free_large_buffer(&ext->recv_buf);
        return -1;
    }
    
    // 分配iovec数组
    ext->iovecs = kvs_malloc(MAX_IOVEC_COUNT * sizeof(struct iovec));
    if (!ext->iovecs) {
        free_large_buffer(&ext->recv_buf);
        free_large_buffer(&ext->send_buf);
        return -1;
    }
    
    return 0;
}

// 释放连接扩展
void free_conn_extension(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    
    conn_extension_t *ext = &conn_extensions[fd];
    free_large_buffer(&ext->recv_buf);
    free_large_buffer(&ext->send_buf);
    
    if (ext->iovecs) {
        kvs_free(ext->iovecs);
        ext->iovecs = NULL;
    }
    
    ext->send_offset = 0;
    ext->processing = 0;
    ext->iovec_count = 0;
}

// 设置非阻塞IO
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 设置TCP_NODELAY选项减少小包延迟
int set_tcp_nodelay(int fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

// 设置TCP缓冲区大小
int set_tcp_buffer_size(int fd, int recv_size, int send_size) {
    if (recv_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recv_size, sizeof(recv_size));
    }
    if (send_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size));
    }
    return 0;
}

int set_event(int fd, int event, int flag) {

	struct epoll_event ev;
	ev.events = event;
	ev.data.fd = fd;

	if (flag) {  // non-zero add
		epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	} else {  // zero mod
		epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
	}

}

int event_register(int fd, int event) {

	if (fd < 0 || fd >= CONNECTION_SIZE) return -1;

	memset(&conn_list[fd], 0, sizeof(struct conn));
	conn_list[fd].fd = fd;
	conn_list[fd].r_action.recv_callback = recv_cb;
	conn_list[fd].send_callback = send_cb;

    // 初始化连接扩展
    if (init_conn_extension(fd) < 0) {
        return -1;
    }

    // 设置非阻塞和TCP优化
    set_nonblocking(fd);
    set_tcp_nodelay(fd);
    set_tcp_buffer_size(fd, 8 * 1024 * 1024, 8 * 1024 * 1024); // 1MB缓冲区

	// 初始化RESP解析器
    resp_parser_init(&conn_list[fd].parser);
    conn_list[fd].parser_initialized = 1;

	return set_event(fd, event | EPOLLET, 1);

}

void event_unregister(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;

    if (conn_list[fd].parser_initialized) {
        resp_parser_free(&conn_list[fd].parser);
        conn_list[fd].parser_initialized = 0;
    }

    free_conn_extension(fd);
    //close(fd);

    //关闭文件描述符
    if(conn_list[fd].fd > 0){
        close(conn_list[fd].fd);
        conn_list[fd].fd = -1;
    }

    //重置连接结构
    // memset(&conn_list[fd], 0, sizeof(struct conn));
    
}


// listenfd(sockfd) --> EPOLLIN --> accept_cb
int accept_cb(int fd) {

	struct sockaddr_in  clientaddr;
	socklen_t len = sizeof(clientaddr);

	int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);
	if (clientfd < 0) {
		printf("accept errno: %d --> %s\n", errno, strerror(errno));
		return -1;
	}

    
	
	event_register(clientfd, EPOLLIN);  // | EPOLLET


	return 0;
}

static long long g_total_cmd[CONNECTION_SIZE];


int process_received_data(int fd) {
    struct conn* c = &conn_list[fd];
    conn_extension_t* ext = &conn_extensions[fd];

    if (ext->processing) {
        return 0; // 已经在处理中，避免重入
    }

    ext->processing = 1;

    //如果上次刚好停在COMPLETE状态 说明上一条命令已经处理完
    if(c->parser.state == RESP_PARSE_COMPLETE){
        resp_parser_reset(&c->parser);
    }

    //只在第一次进行简单的对齐检查
    if(!c->first_packet_checked &&
        ext->recv_buf.size > 0 &&
        c->parser.state == RESP_PARSE_INIT) {
        size_t i = 0;
        while(i < ext->recv_buf.size &&
              (ext->recv_buf.data[i] == '\r' ||
               ext->recv_buf.data[i] == '\n' ||
               ext->recv_buf.data[i] == ' '  ||
               ext->recv_buf.data[i] == '\t')) {
            i++;
        }

        if(i >= ext->recv_buf.size) {
            // 全是空白，等更多数据
            ext->processing = 0;
            return 0;
        }

        if(ext->recv_buf.data[i] != '*') {
            // 不是合法 RESP 数组起始，认为协议错误，直接断开
            printf("Protocol alignment error: first non-space char is '%c' (0x%02X), fd=%d\n",
                   ext->recv_buf.data[i],
                   (unsigned char)ext->recv_buf.data[i],
                   fd);
            ext->processing = 0;
            event_unregister(fd);
            return -1;
        }

        if(i > 0){
            memmove(ext->recv_buf.data,
                    ext->recv_buf.data + i,
                    ext->recv_buf.size - i);
            ext->recv_buf.size -= i;
        }
        c->first_packet_checked = 1;
    }

    //主解析循环 从recv_buf中解析命令
    size_t parsed_total = 0;//本轮共消费的字节数
    int    command_count = 0;//本轮解析出的字节数

    while(parsed_total < ext->recv_buf.size){
        size_t      cur_len = ext->recv_buf.size - parsed_total;
        const char* cur_buf = ext->recv_buf.data + parsed_total;

        // 1. 用无状态 reader 探测：当前 offset 是否有完整命令
        int cmd_len = resp_peek_command(ext->recv_buf.data,
                                        ext->recv_buf.size,
                                        parsed_total);

        if (cmd_len < 0) {
            // 协议错误
            printf("resp_peek_command error for fd %d\n", fd);
            ext->processing = 0;
            event_unregister(fd);
            return -1;
        } else if (cmd_len == 0) {
            // 半包，当前 offset 这条命令还不完整，等待更多数据
            break;
        }                                

        // 2. 此处保证 [parsed_total, parsed_total+cmd_len) 就是一整条命令
        //    用 stateful resp_parser 在这段上解析一次
        if (c->parser.state != RESP_PARSE_INIT &&
            c->parser.state != RESP_PARSE_COMPLETE) {
            // 理论上不该发生，保险起见 reset 一下
            resp_parser_reset(&c->parser);
        }

        int parsed = resp_parser(&c->parser,
                                 ext->recv_buf.data + parsed_total,
                                 (size_t)cmd_len);

        if (parsed < 0) {
            printf("Protocol parse error for fd %d\n", fd);
            printf("Protocol parse error for fd %d, state=%d\n", fd, c->parser.state);
            ext->processing = 0;
            event_unregister(fd);
            return -1;
        }
        
        if (parsed != cmd_len || c->parser.state != RESP_PARSE_COMPLETE) {
            // 正常情况下，cmd_len 保证了完整命令，此时 parser 一定 COMPLETE
            printf("resp_parser mismatch: cmd_len=%d, parsed=%d, state=%d\n",
                   cmd_len, parsed, c->parser.state);
            ext->processing = 0;
            event_unregister(fd);
            return -1;
        }
        
        //获得一条完整命令
        char* cmd_name = c->parser.tokens[0];
        if(!cmd_name){
            printf("Empty command name for fd %d\n", fd);
            ext->processing = 0;
            event_unregister(fd);
            return -1;
        }
        
        //单独处理REPLSYNC命令
        if(strcasecmp(cmd_name, "REPLSYNC") == 0){
            //获取全局角色信息
            extern distributed_config_t global_dist_config;

            const char* ok_resp = "+OK\r\n";
            const char* err_resp = "-ERR REPLSYNC not allowed in current role\r\n";

            if(global_dist_config.role == ROLE_SLAVE){
                //将该连接标记为复制连接
                ext->is_replication = 1;

                //回复+OK
                size_t ok_len = strlen(ok_resp);
                if(ext->send_buf.size + ok_len > ext->send_buf.capacity){
                    if(expand_large_buffer(&ext->send_buf, ext->send_buf.size + ok_len) < 0){
                        ext->processing = 0;
                        event_unregister(fd);
                        return -1;
                    }
                }
                memcpy(ext->send_buf.data + ext->send_buf.size, ok_resp, ok_len);
                ext->send_buf.size += ok_len;
            }else{
                //只允许从节点接收 REPLSYNC
                size_t err_len = strlen(err_resp);
                if(ext->send_buf.size + err_len > ext->send_buf.capacity){
                    if(expand_large_buffer(&ext->send_buf, ext->send_buf.size + err_len) < 0){
                        ext->processing = 0;
                        event_unregister(fd);
                        return -1;
                    }
                }
                memcpy(ext->send_buf.data + ext->send_buf.size, err_resp, err_len);
                ext->send_buf.size += err_len;
            }

        //该命令处理结束
        parsed_total += (size_t)cmd_len;
        resp_parser_reset(&c->parser);
        continue;
        }
        
        
        // 除开上述命令之外的命令
        int old_ctx = get_sync_context();
        if(ext->is_replication){
            set_sync_context(2);//复制连接上下文
        }else{
            set_sync_context(0);//普通连接上下文
        }

        char* response_buf = (char* )kvs_malloc(KVS_MAX_RESPONSE_LENGTH);
        if(!response_buf){
            ext->processing = 0;
            event_unregister(fd);
            return -1;
        }
        int resp_len = kvs_handler(c->parser.tokens,
                            c->parser.arg_index,
                    response_buf);
        if(resp_len <= 0 || resp_len > (int)KVS_MAX_RESPONSE_LENGTH){
            kvs_free(response_buf);
            ext->processing = 0;
            event_unregister(fd);
            return -1;
        }

        set_sync_context(old_ctx);

        // 4. 把响应 append 到 send_buf
        if (ext->send_buf.size + (size_t)resp_len > ext->send_buf.capacity) {
            size_t needed      = ext->send_buf.size + (size_t)resp_len;
            size_t new_capacity = ext->send_buf.capacity;
            while (new_capacity < needed) {
                new_capacity *= 2;
            }
            if (new_capacity > 512 * 1024 * 1024) {
                printf("Send buffer too large for fd %d\n", fd);
                ext->processing = 0;
                kvs_free(response_buf);
                return -1;
            }
            if (expand_large_buffer(&ext->send_buf, new_capacity) < 0) {
                ext->processing = 0;
                kvs_free(response_buf);
                return -1;
            }
        }
        memcpy(ext->send_buf.data + ext->send_buf.size,
               response_buf, (size_t)resp_len);
        ext->send_buf.size += (size_t)resp_len;
        kvs_free(response_buf);
        command_count++;
        // 5. 准备解析下一条命令
        parsed_total += (size_t)cmd_len;
        resp_parser_reset(&c->parser);


    }

    //将完全解析的数据裁剪掉 只剩下未处理的半包
    if(parsed_total > 0){
        size_t remaining = ext->recv_buf.size - parsed_total;
        if(remaining > 0){
            memmove(ext->recv_buf.data,
                    ext->recv_buf.data + parsed_total,
                    remaining);
        }
        ext->recv_buf.size = remaining;

    }
    ext->processing = 0;

    //如果有数据要发送，注册写事件
    if(ext->send_buf.size > 0){
        set_event(fd, EPOLLOUT | EPOLLIN, 0);
    }else{
        set_event(fd, EPOLLIN, 0);
    }

    return command_count;
}



int recv_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    if (conn_list[fd].fd < 0) return -1;   // 已经被 event_unregister
    
    struct conn *c = &conn_list[fd];
    conn_extension_t *ext = &conn_extensions[fd];

    int total_recv = 0;  // 添加统计
    int recv_count = 0;   // 统计recv调用次数
    
    // 边缘触发模式需要循环读取直到EAGAIN
    while (1) {
        // 确保有足够空间接收数据
        size_t min_space = 64 * 1024;
        // if (ext->recv_buf.size + 4096 > ext->recv_buf.capacity) {
        //     if (expand_large_buffer(&ext->recv_buf, ext->recv_buf.size + 4096) < 0) {
        //         event_unregister(fd);
        //         return -1;
        //     }
        // }
        if (ext->recv_buf.capacity - ext->recv_buf.size < min_space) {
            // 扩展到当前容量的2倍,最大512MB
            size_t new_capacity = ext->recv_buf.capacity * 2;
            if (new_capacity > 512 * 1024 * 1024) {
                printf("Receive buffer too large for fd %d\n", fd);
                event_unregister(fd);
                return -1;
            }
            
            if (expand_large_buffer(&ext->recv_buf, new_capacity) < 0) {
                event_unregister(fd);
                return -1;
            }
        }
        
        // 计算剩余空间
        size_t remaining = ext->recv_buf.capacity - ext->recv_buf.size;
        if (remaining <= 0) {
            event_unregister(fd);
            return -1;
        }

        // 🔥 增加每次recv的大小
        size_t recv_size = remaining > (256 * 1024) ? (256 * 1024) : remaining;
        
        // 接收数据
        int count = recv(fd, ext->recv_buf.data + ext->recv_buf.size, recv_size, 0);
        if (count <= 0) {
            if (count == 0) {
                // 连接关闭
                // printf("Connection closed by client for fd %d\n", fd);
                event_unregister(fd);
                return 0;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有更多数据可读
                break;
            } else {
                // 其他错误
                printf("recv error for fd %d: %s\n", fd, strerror(errno));
                event_unregister(fd);
                return -1;
            }
        }
        
        ext->recv_buf.size += count;
        total_recv += count;
        recv_count++;
        
        // 如果接收了大量数据，先处理一部分避免阻塞太久
        // if (ext->recv_buf.size > 1024 * 1024) { // 超过1MB时开始处理
        //     process_received_data(fd);
        // }
    }

    // 打印接收统计
    // if (total_recv > 0) {
    //     printf("fd %d: received %d bytes in %d recv() calls, buffer total %zu bytes\n", 
    //            fd, total_recv, recv_count, ext->recv_buf.size);
    // }
    
    // 等所有数据接收完毕后，一次性处理
    if (ext->recv_buf.size > 0) {
        int processed = process_received_data(fd);
        if (processed > 0) {
            // printf("fd %d: processed %d commands, send buffer %zu bytes\n", 
            //        fd, processed, ext->send_buf.size);
        }
        if (processed < 0) {
            // process_received_data 已经调用 event_unregister 关闭了fd
            return -1;
        }
    }
    
    return 0;
}

// 准备iovec结构用于分散写入
int prepare_iovecs(int fd) {
    conn_extension_t *ext = &conn_extensions[fd];
    
    if (ext->send_buf.size <= 0) {
        ext->iovec_count = 0;
        return 0;
    }
    
    // 将发送缓冲区分割成多个iovec，每个最多MAX_SINGLE_SEND字节
    size_t remaining = ext->send_buf.size - ext->send_offset;
    ext->iovec_count = 0;
    
    while (remaining > 0 && ext->iovec_count < MAX_IOVEC_COUNT) {
        size_t chunk_size = remaining > MAX_SINGLE_SEND ? MAX_SINGLE_SEND : remaining;
        
        ext->iovecs[ext->iovec_count].iov_base = ext->send_buf.data + ext->send_offset;
        ext->iovecs[ext->iovec_count].iov_len = chunk_size;
        
        ext->send_offset += chunk_size;
        remaining -= chunk_size;
        ext->iovec_count++;
    }
    
    return ext->iovec_count;
}


static long long g_total_sent_bytes[CONNECTION_SIZE] = {0};

int send_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    if (conn_list[fd].fd < 0) return -1;
    
    struct conn *c = &conn_list[fd];
    conn_extension_t *ext = &conn_extensions[fd];
    
    if (ext->send_buf.size <= 0 || ext->send_offset >= ext->send_buf.size) {
        // 没有数据要发送或已发送完毕
        ext->send_offset = 0;
        ext->send_buf.size = 0;
        set_event(fd, EPOLLIN, 0);
        return 0;
    }

    long long sent_this_round = 0;
    
#if 0
    // 准备iovec
    if (prepare_iovecs(fd) <= 0) {
        return 0;
    }
    
    // 使用writev进行分散写入
    ssize_t total_sent = 0;
    for (int i = 0; i < ext->iovec_count; i++) {
        ssize_t sent = writev(fd, &ext->iovecs[i], 1);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞模式下重试
                break;
            }
            //printf("Send error for fd %d: %s\n", fd, strerror(errno));
            event_unregister(fd);
            return -1;
        } else if (sent == 0) {
            printf("Connection closed during send for fd %d\n", fd);
            event_unregister(fd);
            return -1;
        }
        
        total_sent += sent;
        
        // 如果发送的数据量小于iovec的长度，说明TCP缓冲区已满
        if (sent < (ssize_t)ext->iovecs[i].iov_len) {
            // 调整偏移量，下次从正确位置继续发送
            ext->send_offset -= (ext->iovecs[i].iov_len - sent);
            break;
        }
    }
#endif

    // 使用循环发送，尽可能发送所有数据
    while (ext->send_offset < ext->send_buf.size) {
        size_t remaining = ext->send_buf.size - ext->send_offset;
        
        ssize_t sent = send(fd, 
                           ext->send_buf.data + ext->send_offset, 
                           remaining, 
                           MSG_NOSIGNAL);  // 避免SIGPIPE信号
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // TCP缓冲区满，等待下次EPOLLOUT
                set_event(fd, EPOLLOUT | EPOLLIN, 0);
                return 0;
            }
            printf("Send error for fd %d: %s\n", fd, strerror(errno));
            event_unregister(fd);
            return -1;
        } else if (sent == 0) {
            printf("Connection closed during send for fd %d\n", fd);
            event_unregister(fd);
            return -1;
        }
        
        ext->send_offset += sent;
        sent_this_round   += sent;
        g_total_sent_bytes[fd] += sent;
    }
    
    // printf("fd %d: send_cb sent %lld bytes this round, total_sent=%lld, offset=%zu, buf_size=%zu\n",
    //        fd, sent_this_round, g_total_sent_bytes[fd], ext->send_offset, ext->send_buf.size);

    // 检查是否全部发送完毕
    if (ext->send_offset >= ext->send_buf.size) {
        // 全部发送完毕，重置缓冲区
        ext->send_offset = 0;
        ext->send_buf.size = 0;
        set_event(fd, EPOLLIN, 0);
    } else {
        // 还有数据要发送，保持EPOLLOUT事件
        set_event(fd, EPOLLOUT | EPOLLIN, 0);
    }

    // 全部发送完毕，重置缓冲区
    // ext->send_offset = 0;
    // ext->send_buf.size = 0;

    // // 只监听读事件
    // set_event(fd, EPOLLIN, 0);
    
    return 0;
}


int r_init_server(unsigned short port) {

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



	struct sockaddr_in servaddr;
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
	servaddr.sin_port = htons(port); // 0-1023, 

	if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr)) < 0) {
		printf("bind failed: %s\n", strerror(errno));
		close(sockfd);
        return -1;
	}

	if (listen(sockfd, 1024) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
	//printf("listen finshed: %d\n", sockfd); // 3 

    // 设置非阻塞
    set_nonblocking(sockfd);

	return sockfd;

}

int reactor_start(unsigned short port, msg_handler handler) {

	//unsigned short port = 2000;
	kvs_handler = handler;

	epfd = epoll_create(1);
	if (epfd < 0) {
        perror("epoll_create1");
        return -1;
    }

    // 初始化连接扩展数组
    memset(conn_extensions, 0, sizeof(conn_extensions));


	int i;
	for (i = 0;i < MAX_PORTS;i ++) {
		int sockfd = r_init_server(port + i);
		if (sockfd < 0) continue;

		conn_list[sockfd].fd = sockfd;
		conn_list[sockfd].r_action.recv_callback = accept_cb;
		
		set_event(sockfd, EPOLLIN, 1);
	}

	struct epoll_event events[1024] = {0};
	while (!g_shutdown_flag) { // mainloop
        if(g_shutdown_flag) break;

		int nready = epoll_wait(epfd, events, 1024, -1);
		if (nready < 0 && errno != EINTR) {
            perror("epoll_wait");
            break;
        }

		int i;
		for (i = 0;i < nready;i ++) {
			int connfd = events[i].data.fd;
            if (conn_list[connfd].fd < 0) continue;  // 已关闭，忽略本次事件

			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			} 

			if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                event_unregister(connfd);
                // close(connfd);
            }

		}

	}

	for (int i = 0; i < CONNECTION_SIZE; i++) {
        if (conn_list[i].fd > 0) {
            event_unregister(i);
            // close(conn_list[i].fd);
        }
    }
	
	close(epfd);
	return 0;
}


