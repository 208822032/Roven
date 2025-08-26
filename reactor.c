


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

typedef int (*msg_handler)(char *msg, int length, char *response);

static msg_handler kvs_handler;

int kvs_request(struct conn *c) {

	c->wlength = kvs_handler(c->rbuffer, c->rlength, c->wbuffer);

}

#endif

int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);



int epfd = 0;
struct timeval begin;



struct conn conn_list[CONNECTION_SIZE] = {0};
// fd


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

	//初始化接收缓冲区
	memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].rlength = 0;
	conn_list[fd].rbuffer[0] = '\0';

	memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].wlength = 0;
	conn_list[fd].wbuffer[0] = '\0';

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
#else
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
#endif

int send_cb(int fd) {
	
	if (fd < 0 || fd >= CONNECTION_SIZE) return -1;

	if(conn_list[fd].wlength > 0){
		// char msg[2048] = {0};
		// strncpy(msg, conn_list[fd].wbuffer, conn_list[fd].wlength);
		// printf("发送数据: %s\n", msg);
		int sent = send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
		
		if (sent < 0) {
            event_unregister(fd);
            return -1;
        }

		// 移动未发送的数据
        if (sent < conn_list[fd].wlength) {
            memmove(conn_list[fd].wbuffer, 
                    conn_list[fd].wbuffer + sent, 
                    conn_list[fd].wlength - sent);
        }
        conn_list[fd].wlength -= sent;
	}

	// 如果还有数据要发送，保持EPOLLOUT事件
    if (conn_list[fd].wlength > 0) {
        set_event(fd, EPOLLOUT, 0);
    } else {
        set_event(fd, EPOLLIN, 0);
    }

	return 0;
}



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

void reactor_wait(void) {
    // 简单实现：等待1秒或直到关闭标志被设置
    struct timespec req = {0, 10000000}; // 10ms
    while (!g_shutdown_flag) {
        nanosleep(&req, NULL);
    }
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
		int nready = epoll_wait(epfd, events, 1024, -1);
		if (nready < 0 && errno != EINTR) {
            perror("epoll_wait");
            break;
        }

		if(g_shutdown_flag) break;

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


