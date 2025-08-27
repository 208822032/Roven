


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

#include "kvstore.h"
#include "server.h"
#include "resp_protocol.h"


#define CONNECTION_SIZE			1024 // 1024 * 1024

#define MAX_PORTS			20

#if ENABLE_KVSTORE

int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

typedef int (*msg_handler)(char *msg, int length, char *response);

static msg_handler kvs_handler;

int kvs_request(struct conn *c) {

	c->wlength = kvs_handler(c->rbuffer, c->rlength, c->wbuffer);

}

#endif

int epfd = 0;
struct conn conn_list[CONNECTION_SIZE] = {0};

// 在适当位置添加调试输出
void debug_print_buffer(const char *buffer, size_t length, const char *tag) {
    printf("[DEBUG %s] Buffer content (first 200 bytes):\n", tag);
    for (size_t i = 0; i < (length < 200 ? length : 200); i++) {
        if (isprint(buffer[i])) {
            putchar(buffer[i]);
        } else {
            printf("\\x%02X", (unsigned char)buffer[i]);
        }
    }
    printf("\n[DEBUG %s] Total length: %zu\n", tag, length);
}


//初始化动态缓冲区
int init_dynamic_buffer(struct conn *c){
    c->rbuffer = kvs_malloc(BUFFER_LENGTH);
    if(!c->rbuffer) return -1;
    c->rbuffer_size = BUFFER_LENGTH;
    c->rlength = 0;

    c->wbuffer = kvs_malloc(BUFFER_LENGTH);
    if(!c->wbuffer){
        kvs_free(c->rbuffer);
        return -1;
    }
    c->wbuffer_size = BUFFER_LENGTH;
    c->wlength = 0;

    return 0;
}

//扩展接收缓冲区
int expand_receive_buffer(struct conn *c, size_t min_size){
    size_t new_size = c->rbuffer_size * 2;
    while(new_size < min_size){
        new_size *= 2;
    }

    char *new_buf = kvs_realloc(c->rbuffer, new_size);
    if(!new_buf) return -1;

    c->rbuffer = new_buf;
    c->rbuffer_size = new_size;
    return 0;
}

//扩展发送缓冲区
int expand_send_buffer(struct conn *c, size_t min_size){
    size_t new_size = c->wbuffer_size * 2;
    while(new_size < min_size){
        new_size *= 2;
    }

    char *new_buf = kvs_realloc(c->wbuffer, new_size);
    if(!new_buf) return -1;

    c->wbuffer = new_buf;
    c->wbuffer_size = new_size;
    return 0;
}

//释放动态缓冲区
void free_dynamic_buffer(struct conn *c){
    if(c->rbuffer){
        kvs_free(c->rbuffer);
        c->rbuffer = NULL;
    }

    if(c->wbuffer){
        c->wbuffer = NULL;
    }
    c->rbuffer_size = 0;
    c->wbuffer_size = 0;
    c->rlength = 0;
    c->wlength = 0;
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

#if 0
	//初始化接收缓冲区
	memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].rlength = 0;
	conn_list[fd].rbuffer[0] = '\0';

    memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].wlength = 0;
	conn_list[fd].wbuffer[0] = '\0';
#else 
    //初始化动态缓冲区
    if(init_dynamic_buffer(&conn_list[fd]) < 0) return -1;

#endif

	// 初始化RESP解析器
    resp_parser_init(&conn_list[fd].parser);
    conn_list[fd].parser_initialized = 1;

	return set_event(fd, event, 1);
}

void event_unregister(int fd) {
    if (fd >= 0 && fd < CONNECTION_SIZE) {
        if (conn_list[fd].parser_initialized) {
            resp_parser_free(&conn_list[fd].parser);
            conn_list[fd].parser_initialized = 0;
        }
        free_dynamic_buffer(&conn_list[fd]);
        close(fd);
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

#if 0
int recv_cb(int fd) {
	if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    
    // 计算剩余空间
    int remaining = BUFFER_LENGTH - conn_list[fd].rlength - 1;
    if (remaining <= 0) {
        // 缓冲区已满，关闭连接
        event_unregister(fd);
        return -1;
    }
    
    // 接收数据
    int count = recv(fd, conn_list[fd].rbuffer + conn_list[fd].rlength, remaining, 0);
    if (count <= 0) {
        // 连接关闭或错误
        event_unregister(fd);
        return 0;
    }
    
    conn_list[fd].rlength += count;
    conn_list[fd].rbuffer[conn_list[fd].rlength] = '\0';
    
    // 处理接收到的数据
    int parsed_total = 0;
    int parsed = 0;
    
    do {
        // 解析RESP协议
        parsed = resp_parser(&conn_list[fd].parser, 
                            conn_list[fd].rbuffer + parsed_total, 
                            conn_list[fd].rlength - parsed_total);
        
        if (parsed > 0) {
            // 成功解析一个完整命令
            char response_buf[KVS_MAX_RESPONSE_SIZE];
            int resp_len = kvs_filter_protocol(conn_list[fd].parser.tokens, 
                                      conn_list[fd].parser.arg_index, 
                                      response_buf);
            
            // 将响应添加到发送缓冲区
            if (conn_list[fd].wlength + resp_len < BUFFER_LENGTH) {
                memcpy(conn_list[fd].wbuffer + conn_list[fd].wlength, 
                       response_buf, resp_len);
                conn_list[fd].wlength += resp_len;
            } else {
                // 发送缓冲区满，关闭连接
                event_unregister(fd);
                return -1;
            }
            
            // 释放解析器资源并重置
            resp_parser_free(&conn_list[fd].parser);
            resp_parser_init(&conn_list[fd].parser);
            
            parsed_total += parsed;
        } else if (parsed == 0) {
            // 需要更多数据
            break;
        } else {
            // 解析错误
            const char *error_msg = "-ERROR Protocol error\r\n";
            int error_len = strlen(error_msg);
            if (conn_list[fd].wlength + error_len < BUFFER_LENGTH) {
                memcpy(conn_list[fd].wbuffer + conn_list[fd].wlength, 
                       error_msg, error_len);
                conn_list[fd].wlength += error_len;
            }
            break;
        }
    } while (parsed_total < conn_list[fd].rlength);
    
    // 移动未处理的数据到缓冲区开头
    if (parsed_total > 0) {
        int remaining_data = conn_list[fd].rlength - parsed_total;
        if (remaining_data > 0) {
            memmove(conn_list[fd].rbuffer, 
                    conn_list[fd].rbuffer + parsed_total, 
                    remaining_data);
        }
        conn_list[fd].rlength = remaining_data;
    }
    
    // 如果有数据要发送，注册写事件
    if (conn_list[fd].wlength > 0) {
        set_event(fd, EPOLLOUT, 0);
    }
    
    return count;
}
#elif 0
int recv_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    
    // 计算剩余空间
    int remaining = BUFFER_LENGTH - conn_list[fd].rlength - 1;
    if (remaining <= 0) {
        // 缓冲区已满，关闭连接
        event_unregister(fd);
        return -1;
    }
    
    // 接收数据
    int count = recv(fd, conn_list[fd].rbuffer + conn_list[fd].rlength, remaining, 0);
    if (count <= 0) {
        // 连接关闭或错误
        event_unregister(fd);
        return 0;
    }
    
    conn_list[fd].rlength += count;
    conn_list[fd].rbuffer[conn_list[fd].rlength] = '\0';
    
    // 处理接收到的数据
    int parsed_total = 0;
    int parsed = 0;
    
    do {
        // 解析RESP协议
        parsed = resp_parser(&conn_list[fd].parser, 
                            conn_list[fd].rbuffer + parsed_total, 
                            conn_list[fd].rlength - parsed_total);
        
        printf("recv count: %d, parsed value: %d\n", count, parsed);
        
        if (parsed > 0) {
            // 成功解析一个完整命令
            if (conn_list[fd].parser.state == RESP_PARSE_COMPLETE) {
                char response_buf[KVS_MAX_RESPONSE_SIZE];
                int resp_len = kvs_filter_protocol(conn_list[fd].parser.tokens, 
                                                  conn_list[fd].parser.arg_index, 
                                                  response_buf);
                
                // 将响应添加到发送缓冲区
                if (conn_list[fd].wlength + resp_len < BUFFER_LENGTH) {
                    memcpy(conn_list[fd].wbuffer + conn_list[fd].wlength, 
                           response_buf, resp_len);
                    conn_list[fd].wlength += resp_len;
                } else {
                    // 发送缓冲区满，关闭连接
                    event_unregister(fd);
                    return -1;
                }
                
                // 重置解析器状态，准备解析下一个命令
                resp_parser_reset(&conn_list[fd].parser);
            }
            
            parsed_total += parsed;
        } else if (parsed == 0) {
            // 需要更多数据
            break;
        } else {
            // 解析错误
            printf("解析错误\n");
            
            // 尝试从错误中恢复：查找下一个RESP起始符
            const char *next_start = memchr(conn_list[fd].rbuffer + parsed_total + 1, '*', 
                                          conn_list[fd].rlength - parsed_total - 1);
            if (next_start) {
                // 找到下一个RESP起始符，跳过错误数据
                parsed_total = next_start - conn_list[fd].rbuffer;
                resp_parser_reset(&conn_list[fd].parser);
            } else {
                // 没有找到更多RESP数据，清空缓冲区
                parsed_total = conn_list[fd].rlength;
                resp_parser_reset(&conn_list[fd].parser);
                break;
            }
        }
    } while (parsed_total < conn_list[fd].rlength);
    
    // 移动未处理的数据到缓冲区开头
    if (parsed_total > 0) {
        int remaining_data = conn_list[fd].rlength - parsed_total;
        if (remaining_data > 0) {
            memmove(conn_list[fd].rbuffer, 
                    conn_list[fd].rbuffer + parsed_total, 
                    remaining_data);
        }
        conn_list[fd].rlength = remaining_data;
        conn_list[fd].rbuffer[conn_list[fd].rlength] = '\0';
    }
    
    // 如果有数据要发送，注册写事件
    if (conn_list[fd].wlength > 0) {
        set_event(fd, EPOLLOUT, 0);
    }
    
    return count;
}
#else
int recv_cb(int fd){
    if(fd < 0 || fd >= CONNECTION_SIZE) return -1;

    struct conn *c = &conn_list[fd];

    //确保有足够空间接收数据
    if(c->rlength + 4096 > c->rbuffer_size){
        if(expand_receive_buffer(c, c->rlength + 4096) < 0){
            event_unregister(fd);
            return -1;
        }
    }

    //计算剩余空间
    size_t remaining = c->rbuffer_size - c->rlength - 1;
    if(remaining <= 0){
        event_unregister(fd);
        return -1;
    }


    // 接收数据
    int count = recv(fd, c->rbuffer + c->rlength, remaining, 0);//非阻塞接收
    if (count <= 0) {
        if (count == 0) {
            printf("Connection closed by client for fd %d\n", fd);
        } else {
            printf("recv error for fd %d: %s\n", fd, strerror(errno));
        }
        event_unregister(fd);
        return 0;
    }

    c->rlength += count;
    c->rbuffer[c->rlength] = '\0';

    
    // 处理接收到的数据
    int parsed_total = 0;
    int parsed = 0;
    int command_count = 0; // 计数器用于解析的命令数量
    
    while (parsed_total < conn_list[fd].rlength) {
        // 解析RESP协议
        parsed = resp_parser(&conn_list[fd].parser, 
                            conn_list[fd].rbuffer + parsed_total, 
                            conn_list[fd].rlength - parsed_total);
            
        if (parsed > 0) {
            // 成功解析一个完整命令
            if (conn_list[fd].parser.state == RESP_PARSE_COMPLETE) {
                char response_buf[KVS_MAX_RESPONSE_SIZE];
                int resp_len = kvs_filter_protocol(conn_list[fd].parser.tokens,  conn_list[fd].parser.arg_index,   response_buf);
 
                // 将响应添加到发送缓冲区
                if (conn_list[fd].wlength + resp_len < BUFFER_LENGTH) {
                    memcpy(conn_list[fd].wbuffer + conn_list[fd].wlength,  response_buf, resp_len);
                    conn_list[fd].wlength += resp_len;
                    command_count++;
                } else {
                    // 发送缓冲区满，关闭连接
                    event_unregister(fd);
                    return -1;
                }
                
                // 重置解析器状态，准备解析下一个命令
                resp_parser_reset(&conn_list[fd].parser);
            }
            
            parsed_total += parsed;
        } else if (parsed == 0) {
            // 需要更多数据
            break;
        } else {
            // 解析错误
            printf("解析错误\n");
            
            // 尝试从错误中恢复：查找下一个RESP起始符
            const char *next_start = memchr(conn_list[fd].rbuffer + parsed_total + 1, '*', conn_list[fd].rlength - parsed_total - 1);
            if (next_start) {
                // 找到下一个RESP起始符，跳过错误数据
                parsed_total = next_start - conn_list[fd].rbuffer;
                resp_parser_reset(&conn_list[fd].parser);
            } else {
                // 没有找到更多RESP数据，清空缓冲区
                parsed_total = conn_list[fd].rlength;
                resp_parser_reset(&conn_list[fd].parser);
                break;
            }
        }
    }
    
    // 移动未处理的数据到缓冲区开头
    if (parsed_total > 0) {
        int remaining_data = conn_list[fd].rlength - parsed_total;
        if (remaining_data > 0) {
            memmove(conn_list[fd].rbuffer, 
                    conn_list[fd].rbuffer + parsed_total, 
                    remaining_data);
        }
        conn_list[fd].rlength = remaining_data;
        conn_list[fd].rbuffer[conn_list[fd].rlength] = '\0';
    }
    
    // 如果有数据要发送，注册写事件
    if (conn_list[fd].wlength > 0) {
        printf("Data to send: %zu bytes for fd %d\n", c->wlength, fd);
        //set_event(fd, EPOLLOUT, 0);
        set_event(fd, EPOLLOUT | EPOLLIN, 0);
    }else{
        // 如果没有数据要发送，只监听EPOLLIN事件
        printf("No data to send for fd %d, setting EPOLLIN\n", fd);
        set_event(fd, EPOLLIN, 0);
    }

    return count;
}
#endif

#if 0
int send_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    
    struct conn *c = &conn_list[fd];
    
    if (c->wlength > 0) {
        int sent = send(fd, c->wbuffer, c->wlength, 0);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞模式下重试
                printf("Send would block, retrying later for fd %d\n", fd);
                return 0;
            }
            printf("发送失败关闭连接\n");
            event_unregister(fd);
            return -1;
        }

        printf("Sent %d bytes for fd %d\n", sent, fd);

        // 移动未发送的数据
        if (sent < c->wlength) {
            memmove(c->wbuffer, c->wbuffer + sent, c->wlength - sent);
        }
        c->wlength -= sent;
    }
    printf("数据发送成功\n");
    
    // 如果还有数据要发送，保持EPOLLOUT事件
    if (c->wlength > 0) {
        set_event(fd, EPOLLOUT, 0);
    } else {
        set_event(fd, EPOLLIN, 0);
    }
    
    return 0;
}
#else
int send_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    
    struct conn *c = &conn_list[fd];
    
    if (c->wlength > 0) {
        int total_sent = 0;
        int sent = 0;
        
        do {
            sent = send(fd, c->wbuffer + total_sent, c->wlength - total_sent, 0);
            
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 非阻塞模式下重试
                    printf("Send would block, retrying later for fd %d\n", fd);
                    break;
                }
                printf("Send error for fd %d: %s\n", fd, strerror(errno));
                event_unregister(fd);
                return -1;
            } else if (sent == 0) {
                printf("Connection closed during send for fd %d\n", fd);
                event_unregister(fd);
                return -1;
            }
            
            total_sent += sent;
            printf("Sent %d bytes for fd %d (total: %d/%zu)\n", 
                   sent, fd, total_sent, c->wlength);
            
        } while (total_sent < c->wlength);
        
        // 移动未发送的数据
        if (total_sent > 0) {
            if (total_sent < c->wlength) {
                memmove(c->wbuffer, c->wbuffer + total_sent, c->wlength - total_sent);
            }
            c->wlength -= total_sent;
        }
    }
    
    // 关键修改：无论是否还有数据要发送，都需要同时监听EPOLLIN事件
    // 因为可能需要接收更多数据来完成不完整的请求
    
    if (c->wlength > 0) {
        // 如果还有数据要发送，同时监听EPOLLOUT和EPOLLIN事件
        printf("Remaining data to send: %zu bytes for fd %d, setting EPOLLOUT|EPOLLIN\n", c->wlength, fd);
        set_event(fd, EPOLLOUT | EPOLLIN, 0);
    } else {
        // 如果没有数据要发送，只监听EPOLLIN事件
        printf("No more data to send for fd %d, setting EPOLLIN\n", fd);
        set_event(fd, EPOLLIN, 0);
    }
    
    return 0;
}
#endif



int r_init_server(unsigned short port) {

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
        perror("socket");
        return -1;
    }

	// 设置SO_REUSEADDR选项
	int reuse = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(sockfd);
        return -1;
    }

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

			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			} 

			if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                event_unregister(connfd);
                close(connfd);
            }

		}

	}

	for (int i = 0; i < CONNECTION_SIZE; i++) {
        if (conn_list[i].fd > 0) {
            event_unregister(i);
            close(conn_list[i].fd);
        }
    }
	
	close(epfd);
	return 0;
}


