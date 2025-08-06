


#include "kvstore.h"
#include "nty_coroutine.h"

#include <arpa/inet.h>


typedef int (*msg_handler)(char *msg, int length, char *response);
static msg_handler kvs_handler;
extern int g_shutdown_flag;


void server_reader(void *arg) {
	int fd = *(int *)arg;
	int ret = 0;

	printf("开始接收数据\n");
	while (1) {
		
		char buf[1024] = {0};
		ret = recv(fd, buf, 1024, 0);
		if (ret > 0) {
			
			char response[1024] = {0};
			int slength = kvs_handler(buf, ret, response);

			// 检查是否收到QUIT命令
            if (g_shutdown_flag) {
                close(fd);
                break;
            }

			ret = send(fd, response, slength, 0);
			if (ret == -1) {
				close(fd);
				break;
			}
		} else if (ret == 0) {	
			close(fd);
			break;
		}

	}
	printf("数据接收完毕退出\n");
}



void server(void *arg) {

	unsigned short port = *(unsigned short *)arg;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return ;

	// 设置socket为非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in local, remote;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr.s_addr = INADDR_ANY;
	bind(fd, (struct sockaddr*)&local, sizeof(struct sockaddr_in));

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
        
        if (cli_fd > 0) {
            nty_coroutine *read_co;
            nty_coroutine_create(&read_co, server_reader, &cli_fd);
        } else {
            // 没有新连接时短暂休眠
            //usleep(10000);  // 10ms
        }
    }
#endif
	close(fd);
	
}

int ntyco_start(unsigned short port, msg_handler handler) {

	//int port = atoi(argv[1]);
	kvs_handler = handler;

	nty_coroutine *co = NULL;
	nty_coroutine_create(&co, server, &port);

	// 修改调度循环，添加关闭标志检测
    while (!g_shutdown_flag) {
        nty_schedule_run();  // 非阻塞执行
        usleep(10000);       // 10ms休眠
    }

}




