


#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include "kvstore.h"
#include <fcntl.h> 
#include "resp_protocol.h"


#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2

extern int g_shutdown_flag; // 声明全局关闭标志

struct conn_info{
	int fd;
    int event;
    char buffer[4096];       // 增大缓冲区
    int buf_len;             // 当前缓冲区数据长度

    char response[KVS_MAX_RESPONSE_SIZE]; // 响应缓冲区
    int resp_len;            // 响应长度
};

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
	int flags = fcntl(sockfd, F_GETFL, 0);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	listen(sockfd, 10);
	
	return sockfd;
}



#define ENTRIES_LENGTH		1024
#define BUFFER_LENGTH		1024

int set_event_recv(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
	if (!sqe) return -1;

	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_READ,
	};
	
	io_uring_prep_recv(sqe, sockfd, buf, len, flags);
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}


int set_event_send(struct io_uring *ring, int sockfd, void *buf, size_t len, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_WRITE,
	};
	
	io_uring_prep_send(sqe, sockfd, buf, len, flags);
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}



int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {

	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	struct conn_info accept_info = {
		.fd = sockfd,
		.event = EVENT_ACCEPT,
	};
	
	io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
	memcpy(&sqe->user_data, &accept_info, sizeof(struct conn_info));

}


typedef int (*msg_handler)(char *msg, int length, char *response);
static msg_handler kvs_handler;




int proactor_start(unsigned short port, msg_handler handler) {

	int sockfd = p_init_server(port);
	kvs_handler = handler;


	struct io_uring_params params;
	memset(&params, 0, sizeof(params));
	struct io_uring ring;
	io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params);

	
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

		io_uring_submit(&ring);

#if 0
		struct io_uring_cqe *cqe;
		io_uring_wait_cqe(&ring, &cqe);
#else
		struct io_uring_cqe *cqe;
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms超时
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
		if (ret < 0 || !cqe) continue;  // 超时或错误
#endif
		struct conn_info *info = (struct conn_info *)&cqe->user_data;

		if (g_shutdown_flag) break;

		if(info->event == EVENT_ACCEPT){
			//接收新连接
			set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
			
			int connfd = cqe->res;
			if(connfd < 0) continue;

			//初始化新连接上下文
			struct conn_info *conn_info = malloc(sizeof(struct conn_info));
			memset(conn_info, 0, sizeof(struct conn_info));

			//开始读取数据
			set_event_recv(&ring, connfd, conn_info->buffer, sizeof(conn_info->buffer), 0);
			//memcpy(&conn_info.user_data, conn_info, sizeof(struct conn_info));
		}else if(info->event == EVENT_READ){
			int ret = cqe->res;
			if(ret <= 0){
				//连接关闭或错误
				close(info->fd);
				free(info);
			}else{
				info->resp_len = kvs_handler(info->buffer, ret, info->response);
				set_event_send(&ring, info->fd, info->response, info->resp_len, 0);
			}
		}else if(info->event == EVENT_WRITE){
			// 发送完成后继续读取
            set_event_recv(&ring, info->fd, info->buffer + info->buf_len, sizeof(info->buffer) - info->buf_len - 1, 0);
		}

		io_uring_cqe_seen(&ring, cqe);

	}


	close(sockfd);
	io_uring_queue_exit(&ring);
	printf("Proactor network exited\n");

	return 0;

}


