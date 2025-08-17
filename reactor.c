


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

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)



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

	if (flag) {  // non-zero add

		struct epoll_event ev;
		ev.events = event;
		ev.data.fd = fd;
		epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

	} else {  // zero mod

		struct epoll_event ev;
		ev.events = event;
		ev.data.fd = fd;
		epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
		
	}
	

}


int event_register(int fd, int event) {

	if (fd < 0) return -1;

	memset(&conn_list[fd], 0, sizeof(struct conn));
	conn_list[fd].fd = fd;
	conn_list[fd].r_action.recv_callback = recv_cb;
	conn_list[fd].send_callback = send_cb;

	memset(conn_list[fd].rbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].rlength = 0;

	memset(conn_list[fd].wbuffer, 0, BUFFER_LENGTH);
	conn_list[fd].wlength = 0;

	// 初始化RESP解析器
    resp_parser_init(&conn_list[fd].parser);
    conn_list[fd].parser_initialized = 1;

	set_event(fd, event, 1);
}

void event_unregister(int fd) {
    if (fd >= 0 && fd < CONNECTION_SIZE && conn_list[fd].parser_initialized) {
        resp_parser_free(&conn_list[fd].parser);
        conn_list[fd].parser_initialized = 0;
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

	if ((clientfd % 1000) == 0) {

		struct timeval current;
		gettimeofday(&current, NULL);

		int time_used = TIME_SUB_MS(current, begin);
		memcpy(&begin, &current, sizeof(struct timeval));
		

		//printf("accept finshed: %d, time_used: %d\n", clientfd, time_used);

	}

	return 0;
}


int recv_cb(int fd) {

	int count = recv(fd, conn_list[fd].rbuffer + conn_list[fd].rlength, BUFFER_LENGTH - conn_list[fd].rlength - 1, 0);
	if (count <= 0) { // disconnect
		//printf("client disconnect: %d\n", fd);
		event_unregister(fd);
		close(fd);
		epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); // unfinished
		return 0;
	} 

	
	conn_list[fd].rlength = count;
	conn_list[fd].rbuffer[conn_list[fd].rlength] = '\0';
	//printf("RECV: %s\n", conn_list[fd].rbuffer);

	conn_list[fd].wlength = kvs_handler(conn_list[fd].rbuffer, conn_list[fd].rlength, conn_list[fd].wbuffer);
    set_event(fd, EPOLLOUT, 0);
	
	return count;
}


int send_cb(int fd) {

	if (conn_list[fd].wlength != 0) {
		send(fd, conn_list[fd].wbuffer, conn_list[fd].wlength, 0);
		conn_list[fd].wlength = 0;  // 清空发送缓冲区
	}
	
	set_event(fd, EPOLLIN, 0);

	return 0;
}



int r_init_server(unsigned short port) {

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in servaddr;
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
	servaddr.sin_port = htons(port); // 0-1023, 

	if (-1 == bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr))) {
		printf("bind failed: %s\n", strerror(errno));
	}

	listen(sockfd, 10);
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


