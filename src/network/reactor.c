


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

#include "net_session.h" //通用会话层


#define CONNECTION_SIZE			1024 // 1024 * 1024

#define MAX_PORTS			1

#define MAX_IOVEC_COUNT     1024   //最大iovec数量
#define MAX_SINGLE_SEND     1024 * 1024  //单次发送最大数据量 1MB

int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

#if ENABLE_KVSTORE
typedef int (*msg_handler)(char **tokens, int count, char *response);

static msg_handler kvs_handler;
#endif

//连接扩展结构 用于管理大缓冲区
typedef struct{
    net_session_t session; //通用会话层 封装recv/send缓冲区和RESP解析器
    size_t send_offset;//发送偏移量

    struct iovec *iovecs;//用于分散/聚集IO
    int iovec_count;  //iovec数量

    int is_replication;//是否是复制连接
} conn_extension_t;

int epfd = 0;
struct conn conn_list[CONNECTION_SIZE] = {0};

//全局连接扩展数组
static conn_extension_t conn_extensions[CONNECTION_SIZE] = {0};

extern kvs_config_t g_cfg;

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

// 初始化连接扩展
int init_conn_extension(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    
    conn_extension_t *ext = &conn_extensions[fd];
    memset(ext, 0, sizeof(conn_extension_t));
    
    // 初始化 net_session：这里直接给 16MB 初始缓冲，和原来的 large_buffer 一致
    if (net_session_init(&ext->session, 8 * 1024 * 1024, 8 * 1024 * 1024) < 0) {
        return -1;
    }
    
    // 分配iovec数组
    ext->iovecs = kvs_malloc(MAX_IOVEC_COUNT * sizeof(struct iovec));
    if (!ext->iovecs) {
        net_session_free(&ext->session);
        return -1;
    }

    ext->iovec_count = 0;
    ext->send_offset = 0;
    ext->is_replication = 0;
    
    return 0;
}

// 释放连接扩展
void free_conn_extension(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    
    conn_extension_t *ext = &conn_extensions[fd];
   
    net_session_free(&ext->session);
    
    if (ext->iovecs) {
        kvs_free(ext->iovecs);
        ext->iovecs = NULL;
    }
    
    ext->send_offset = 0;
    ext->iovec_count = 0;
    ext->is_replication = 0;
}

//注册事件 + 初始化每一个连接状态
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
    set_tcp_buffer_size(fd, 8 * 1024 * 1024, 8 * 1024 * 1024); // 8MB 缓冲区

	return set_event(fd, event | EPOLLET, 1);

}

void event_unregister(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;

    free_conn_extension(fd);

    //关闭文件描述符
    if(conn_list[fd].fd > 0){
        close(conn_list[fd].fd);
        conn_list[fd].fd = -1;
    }
    
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

//net_session 的handler 
//实现 REPLSYNC + is_replication + kvs_handler 调用
static int reactor_session_handler(resp_parser_t* parser,
                                    char* response_buf,
                                    size_t response_buf_cap,
                                    void *user_data){

    conn_extension_t* ext = (conn_extension_t* )user_data;
    
    if(!parser || parser->arg_index <= 0 || !parser->tokens[0]){
        const char* err = "-ERR invalid command\r\n";
        size_t len = strlen(err);
        if(len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    char *cmd_name = parser->tokens[0];
    int resp_len = 0;

        //单独处理REPLSYNC命令
        if(g_cfg.sync_mode == KVS_SYNC_EBPF && strcasecmp(cmd_name, "REPLSYNC") == 0){
            extern distributed_config_t global_dist_config;

            const char* ok_resp = "+OK\r\n";
            const char* err_resp = "-ERR REPLSYNC not allowed in current role\r\n";

            if(global_dist_config.role == ROLE_SLAVE){
                //将该连接标记为复制连接
                ext->is_replication = 1;

                //回复+OK
                size_t ok_len = strlen(ok_resp);
                if(ok_len > response_buf_cap) ok_len = response_buf_cap;
                memcpy(response_buf, ok_resp, ok_len);
                return (int)ok_len;
            }else{
                //只允许从节点接收 REPLSYNC
                size_t err_len = strlen(err_resp);
                if(err_len > response_buf_cap) err_len = response_buf_cap;
                memcpy(response_buf, err_resp, err_len);
                return (int)err_len;
            }
        }

        //其他命令 根据is_replication选择上下文
        int old_ctx = get_sync_context();
        if(ext->is_replication){
            set_sync_context(2);//复制连接上下文
        }else{
            set_sync_context(0);//普通连接上下文
        }

        resp_len = kvs_handler(parser->tokens,
                            parser->arg_index,
                    response_buf);

        set_sync_context(old_ctx);


    if(resp_len < 0 || resp_len > (int)response_buf_cap){
        //不合法长度 返回错误
        const char* err = "-ERR internal handler error\r\n";
        size_t len = strlen(err);
        if(len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    return resp_len;
}

// 准备iovec结构用于分散写入
int prepare_iovecs(int fd) {
    conn_extension_t *ext = &conn_extensions[fd];
    netbuf_t* send_buf = &ext->session.send_buf;
    
    if (send_buf->size <= 0) {
        ext->iovec_count = 0;
        return 0;
    }
    
    // 将发送缓冲区分割成多个iovec，每个最多MAX_SINGLE_SEND字节
    size_t remaining = send_buf->size - ext->send_offset;
    ext->iovec_count = 0;
    
    while (remaining > 0 && ext->iovec_count < MAX_IOVEC_COUNT) {
        size_t chunk_size = remaining > MAX_SINGLE_SEND ? MAX_SINGLE_SEND : remaining;

        ext->iovecs[ext->iovec_count].iov_base = send_buf->data + ext->send_offset;
        ext->iovecs[ext->iovec_count].iov_len  = chunk_size;

        ext->send_offset += chunk_size;
        remaining        -= chunk_size;
        ext->iovec_count++;
    }

    return ext->iovec_count;
}


int recv_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    if (conn_list[fd].fd < 0) return -1;   // 已经被 event_unregister
    
    conn_extension_t *ext = &conn_extensions[fd];
    netbuf_t* recv_buf = &ext->session.recv_buf;

    int total_recv = 0;  // 添加统计
    int recv_count = 0;   // 统计recv调用次数
    
    while (1) {
        size_t min_space = 64 * 1024;
        if(recv_buf->capacity - recv_buf->size < min_space){
            // 扩展缓冲区
            if(netbuf_reserve(recv_buf, recv_buf->size + min_space) < 0){
                printf("Failed to reserve space in recv buffer for fd %d\n", fd);
                event_unregister(fd);
                return -1;
            }
        }
        
        // 计算剩余空间
        size_t remaining = recv_buf->capacity - recv_buf->size;
        if (remaining == 0) {
            event_unregister(fd);
            return -1;
        }

        size_t recv_size = remaining > (256 * 1024) ? (256 * 1024) : remaining;
        
        int count = recv(fd, 
                         recv_buf->data + recv_buf->size, 
                         recv_size, 
                         0);
        if (count <= 0) {
            if (count == 0) {
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
        
        recv_buf->size += count;
        total_recv += count;
        recv_count++;
        
    }

    //有数据 交给net_session 统一解析 生成响应
    if(recv_buf->size > 0){
        int processed = net_session_process_resp(
            fd,
            &ext->session,
            reactor_session_handler,
            ext
        );
        if(processed < 0){
            //出错 关闭连接
            event_unregister(fd);
            return -1;
        }

        g_total_cmd[fd] += processed;
    }

    //根据 send_buf 是否有数据 决定是否监听 EPOLLOUT
    netbuf_t* send_buf = &ext->session.send_buf;
    if(send_buf->size > 0){
        set_event(fd, EPOLLIN | EPOLLOUT, 0);
    }else{
        set_event(fd, EPOLLIN, 0);
    }
    
    return 0;
}




static long long g_total_sent_bytes[CONNECTION_SIZE] = {0};

int send_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    if (conn_list[fd].fd < 0) return -1;
    
    conn_extension_t *ext = &conn_extensions[fd];
    netbuf_t* send_buf = &ext->session.send_buf;
    
    if (send_buf->size <= 0 || ext->send_offset >= send_buf->size) {
        // 没有数据要发送或已发送完毕
        ext->send_offset = 0;
        send_buf->size = 0;
        set_event(fd, EPOLLIN, 0);
        return 0;
    }

    long long sent_this_round = 0;

    // 使用循环发送，尽可能发送所有数据
    while (ext->send_offset < send_buf->size) {
        size_t remaining = send_buf->size - ext->send_offset;
        
        ssize_t sent = send(fd, 
                           send_buf->data + ext->send_offset, 
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
    
    //全部发送完毕
    if(ext->send_offset >= send_buf->size){
        ext->send_offset = 0;
        send_buf->size = 0;
        set_event(fd, EPOLLIN, 0);
    }else{
        set_event(fd, EPOLLOUT | EPOLLIN, 0);
    }
    
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
    memset(conn_list, 0, sizeof(conn_list));

	for (int i = 0;i < MAX_PORTS;i ++) {
		int sockfd = r_init_server(port + i);
		if (sockfd < 0) continue;

		conn_list[sockfd].fd = sockfd;
		conn_list[sockfd].r_action.recv_callback = accept_cb;
		
		set_event(sockfd, EPOLLIN, 1);
	}

	struct epoll_event events[1024] = {0};
	while (!g_shutdown_flag) { // mainloop
        if(g_shutdown_flag) break;

		int nready = epoll_wait(epfd, events, 1024, -1); //-1
		if (nready < 0 && errno != EINTR) {
            perror("epoll_wait");
            break;
        }

		for (int i = 0;i < nready;i ++) {
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
            }

		}

	}

	for (int i = 0; i < CONNECTION_SIZE; i++) {
        if (conn_list[i].fd > 0) {
            event_unregister(i);
        }
    }
	
	close(epfd);
	return 0;
}


