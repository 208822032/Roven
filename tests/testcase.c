

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>  // 包含 fcntl() 函数的声明
#include <hiredis/hiredis.h>

#include <netinet/tcp.h>
#define MAX_MSG_LENGTH		1024
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

void cleanup_connction(int connfd);

int send_msg(int connfd, char *msg, int length) {
#if 0
	int res = send(connfd, msg, length, 0);
    printf("数据发送: %d, 数据长度: %d\n", res, length);
	if (res < 0) {
		perror("send");
		exit(1);
	}
	return res;
#else
    int total_sent = 0;
    int remaining = length;

    // 可选：把 connfd 设为非阻塞
    // int flags = fcntl(connfd, F_GETFL, 0);
    // fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
    
    while (total_sent < length) {
        int res = send(connfd, msg + total_sent, remaining, 0);
        
        if (res < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 如果是非阻塞socket且缓冲区满，可以稍等或处理其他任务
                usleep(1000); // 等待1ms
                continue;
            }
            perror("send");
            exit(1);
        }
        
        if (res == 0) {
            // 连接已关闭
            printf("连接已关闭\n");
            break;
        }
        
        total_sent += res;
        remaining -= res;
        
        //printf("本次发送: %d, 累计发送: %d, 剩余: %d\n", res, total_sent, remaining);
    }
    
    //printf("数据发送完成: %d, 总长度: %d\n", total_sent, length);
    return total_sent;
#endif
}

int recv_msg(int connfd, char *msg, int length) {

    int res = recv(connfd, msg, length, 0);
    if (res < 0) {
        perror("recv");
        exit(1);
    }
    return res;

}

// 提前声明，供 recv_full_resp / recv_pipeline_resps 使用
static int parse_resp_object(const char *buf, int len, int *consumed);

// 阻塞读取，直到完整读到一条简单 RESP 或 bulk string
// 这里只用于主从同步测试读 "$6\r\nvalue1\r\n" 这样的响应
static int recv_full_resp(int connfd, char *buf, int buflen) {
    int total = 0;

    if (buflen <= 0) return -1;

    while (1) {
        int n = recv(connfd, buf + total, buflen - total - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recv_full_resp recv");
            return -1;
        } else if (n == 0) {
            // 连接关闭
            break;
        }
        total += n;
        buf[total] = '\0';

        // 尝试解析一条 RESP 对象
        int consumed = 0;
        int ret = parse_resp_object(buf, total, &consumed);
        if (ret < 0) {
            fprintf(stderr, "recv_full_resp: parse_resp_object error\n");
            return -1;
        } else if (ret == 1) {
            // 已经读到一条完整的 RESP，对多余数据不关心
            buf[consumed] = '\0';
            return consumed;
        }

        // ret == 0 表示数据还不够，继续 recv
        if (total >= buflen - 1) {
            // 缓冲区满还没解析出一条，认为失败
            fprintf(stderr, "recv_full_resp: buffer too small or protocol error\n");
            return -1;
        }
    }

    return -1;
}

// 精确解析 pipeline 响应：按 RESP 语义解析 expected_cmds 个对象
// out_raw / out_raw_cap 可选，仅在需要做字节级比对时使用
static int recv_pipeline_resps(int connfd,
                               int expected_cmds,
                               char *out_raw,
                               int  out_raw_cap)
{
    char buf[4096];
    int  buf_len    = 0;
    int  resp_count = 0;
    int  raw_len    = 0;

    if (expected_cmds <= 0) return -1;

    while (resp_count < expected_cmds) {
        // 1. 先尽量多收一点数据
        if (buf_len < (int)sizeof(buf) - 1) {
            int n = recv(connfd, buf + buf_len,
                         (int)sizeof(buf) - 1 - buf_len, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("recv_pipeline_resps recv");
                return -1;
            } else if (n == 0) {
                fprintf(stderr,
                        "recv_pipeline_resps: connection closed, resp_count=%d, expected=%d\n",
                        resp_count, expected_cmds);
                return -1;
            }
            buf_len += n;

            if (out_raw && raw_len < out_raw_cap - n - 1) {
                memcpy(out_raw + raw_len, buf + buf_len - n, n);
                raw_len += n;
            }
        }

        // 2. 在 buf 中尽量多地解析 RESP 对象
        int offset = 0;
        while (offset < buf_len && resp_count < expected_cmds) {
            int consumed = 0;
            int ret = parse_resp_object(buf + offset,
                                        buf_len - offset,
                                        &consumed);
            if (ret == 1) {
                resp_count++;
                offset += consumed;
            } else if (ret == 0) {
                // 数据不够一个完整对象，跳出等待更多数据
                break;
            } else {
                fprintf(stderr,
                        "recv_pipeline_resps: parse_resp_object error (ret=%d)\n",
                        ret);
                return -1;
            }
        }

        if (offset > 0 && offset < buf_len) {
            memmove(buf, buf + offset, buf_len - offset);
        }
        buf_len -= offset;
    }

    if (out_raw) {
        if (raw_len >= out_raw_cap) raw_len = out_raw_cap - 1;
        out_raw[raw_len] = '\0';
    }

    return 0;
}

// 生成针对 count 个 MSA_key_%06d 的 GET pipeline
static char* generate_get_pipeline(int count) {
    if (count <= 0) return NULL;

    size_t total_len = 0;
    for (int i = 0; i < count; ++i) {
        total_len += snprintf(NULL, 0,
            "*2\r\n$3\r\nGET\r\n$14\r\nMSA_key_%06d\r\n", i);
    }

    char *pipeline = (char*)malloc(total_len + 1);
    if (!pipeline) return NULL;

    size_t written = 0;
    for (int i = 0; i < count; ++i) {
        written += sprintf(pipeline + written,
            "*2\r\n$3\r\nGET\r\n$14\r\nMSA_key_%06d\r\n", i);
    }
    pipeline[written] = '\0';
    return pipeline;
}

// 生成针对 count 个 MSH_key_%06d 的 HGET pipeline
static char* generate_hash_get_pipeline(int count) {
    if (count <= 0) return NULL;

    size_t total_len = 0;
    for (int i = 0; i < count; ++i) {
        total_len += snprintf(NULL, 0,
            "*2\r\n$4\r\nHGET\r\n$14\r\nMSH_key_%06d\r\n", i);
    }

    char *pipeline = (char*)malloc(total_len + 1);
    if (!pipeline) return NULL;

    size_t written = 0;
    for (int i = 0; i < count; ++i) {
        written += sprintf(pipeline + written,
            "*2\r\n$4\r\nHGET\r\n$14\r\nMSH_key_%06d\r\n", i);
    }
    pipeline[written] = '\0';
    return pipeline;
}

// 生成针对 count 个 MSR_key_%06d 的 RGET pipeline
static char* generate_rbtree_get_pipeline(int count) {
    if (count <= 0) return NULL;

    size_t total_len = 0;
    for (int i = 0; i < count; ++i) {
        total_len += snprintf(NULL, 0,
            "*2\r\n$4\r\nRGET\r\n$14\r\nMSR_key_%06d\r\n", i);
    }

    char *pipeline = (char*)malloc(total_len + 1);
    if (!pipeline) return NULL;

    size_t written = 0;
    for (int i = 0; i < count; ++i) {
        written += sprintf(pipeline + written,
            "*2\r\n$4\r\nRGET\r\n$14\r\nMSR_key_%06d\r\n", i);
    }
    pipeline[written] = '\0';
    return pipeline;
}

// 生成针对 count 个 MSS_key_%06d 的 SGET pipeline
static char* generate_skiptable_get_pipeline(int count) {
    if (count <= 0) return NULL;

    size_t total_len = 0;
    for (int i = 0; i < count; ++i) {
        total_len += snprintf(NULL, 0,
            "*2\r\n$4\r\nSGET\r\n$14\r\nMSS_key_%06d\r\n", i);
    }

    char *pipeline = (char*)malloc(total_len + 1);
    if (!pipeline) return NULL;

    size_t written = 0;
    for (int i = 0; i < count; ++i) {
        written += sprintf(pipeline + written,
            "*2\r\n$4\r\nSGET\r\n$14\r\nMSS_key_%06d\r\n", i);
    }
    pipeline[written] = '\0';
    return pipeline;
}

// 从 connfd 尽量读取完整的 pipeline 响应，直到超时或对端关闭
static int recv_all_with_timeout(int connfd, char **out_buf, size_t *out_len) {
    size_t cap = 1024 * 1024; // 1MB 起步
    char *buf = (char*)malloc(cap);
    if (!buf) return -1;

    size_t total = 0;

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    while (1) {
        if (cap - total < 4096) {
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
        }

        int n = recv(connfd, buf + total, (int)(cap - total - 1), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break; // 超时或被打断，认为收完
            } else {
                perror("recv_all_with_timeout recv");
                free(buf);
                return -1;
            }
        } else if (n == 0) {
            break; // 对端关闭
        } else {
            total += n;
        }
    }

    buf[total] = '\0';
    *out_buf = buf;
    *out_len = total;
    return 0;
}




void testcase(int connfd, char *msg, char *pattern, char *casename) {

	if (!msg || !pattern || !casename) return ;

	send_msg(connfd, msg, strlen(msg));

	char result[MAX_MSG_LENGTH] = {0};
	recv_msg(connfd, result, MAX_MSG_LENGTH);

	if (strcmp(result, pattern) == 0) {
		printf("==> PASS ->  %s\n", casename);
	} else {
		printf("==> FAILED -> %s, '%s' != '%s' \n", casename, result, pattern);
		exit(1);
	}

}



int connect_tcpserver(const char *ip, unsigned short port) {

	int connfd = socket(AF_INET, SOCK_STREAM, 0);


	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(struct sockaddr_in));

	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = inet_addr(ip);
	server_addr.sin_port = htons(port);

	if (0 !=  connect(connfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in))) {
		perror("connect");
		return -1;
	}
	
	return connfd;
	
}

//范围查找和排序测试用例
void array_pipeline_testcase(int connfd) {
    //printf("Testing pipeline functionality...\n");

    // 构建包含多个命令的pipeline请求
    const char *pipeline_cmd = 
        "*3\r\n$3\r\nSET\r\n$8\r\nAP_Key01\r\n$6\r\nValue1\r\n"
        "*2\r\n$3\r\nGET\r\n$8\r\nAP_Key01\r\n"
        "*3\r\n$3\r\nMOD\r\n$8\r\nAP_Key01\r\n$9\r\nNewValue1\r\n"
        "*2\r\n$3\r\nGET\r\n$8\r\nAP_Key01\r\n"
        "*2\r\n$3\r\nDEL\r\n$8\r\nAP_Key01\r\n"
        "*2\r\n$3\r\nGET\r\n$8\r\nAP_Key01\r\n"
        "*3\r\n$3\r\nSET\r\n$8\r\nAP_Key02\r\n$6\r\nValue2\r\n"
        "*3\r\n$3\r\nSET\r\n$8\r\nAP_Key03\r\n$6\r\nValue3\r\n"
		"*3\r\n$3\r\nSET\r\n$8\r\nAP_Key04\r\n$6\r\nValue4\r\n"
        "*2\r\n$4\r\nSORT\r\n$4\r\nhigh\r\n"
		"*2\r\n$4\r\nSORT\r\n$3\r\nlow\r\n"
		"*3\r\n$5\r\nRANGE\r\n$8\r\nAP_Key02\r\n$8\r\nAP_Key04\r\n"
		"*2\r\n$3\r\nDEL\r\n$8\r\nAP_Key02\r\n"
		"*2\r\n$3\r\nDEL\r\n$8\r\nAP_Key03\r\n"
		"*2\r\n$3\r\nDEL\r\n$8\r\nAP_Key04\r\n";


    // 发送pipeline请求
    send_msg(connfd, (char *)pipeline_cmd, strlen(pipeline_cmd));

    // 使用 RESP 精确解析 15 个响应
    char response[4096] = {0};
    int commands_per_pipeline = 15;
    if (recv_pipeline_resps(connfd,
                            commands_per_pipeline,
                            response, sizeof(response)) != 0) {
        printf("==> FAILED -> array pipeline RESP parse error\n");
        exit(1);
    }

    // 验证响应内容
    const char *expected_response = 
        "+OK\r\n"
        "$6\r\nValue1\r\n"
        "+OK\r\n"
        "$9\r\nNewValue1\r\n"
        "+OK\r\n"
        "+NO EXIST\r\n"
        "+OK\r\n"
        "+OK\r\n"
		"+OK\r\n"
        "*3\r\n$15\r\nAP_Key04:Value4\r\n$15\r\nAP_Key03:Value3\r\n$15\r\nAP_Key02:Value2\r\n"
		"*3\r\n$15\r\nAP_Key02:Value2\r\n$15\r\nAP_Key03:Value3\r\n$15\r\nAP_Key04:Value4\r\n"
		"*3\r\n$15\r\nAP_Key02:Value2\r\n$15\r\nAP_Key03:Value3\r\n$15\r\nAP_Key04:Value4\r\n"
		"+OK\r\n"
        "+OK\r\n"
		"+OK\r\n";
    
	if (strcmp(response, expected_response) == 0) {
        //printf("==> PASS -> Pipeline test\n");
    } else {
        printf("==> FAILED -> Pipeline test\n");
        printf("Expected:\n%s\n", expected_response);
        printf("Received:\n%s\n", response);
    }
}
void array_pipeline_qps_testcase(int connfd){
	int count = 100000;
    int commands_per_test = 15; // 每次测试中的命令数量
    int i = 0;

    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    
    for(i = 0;i < count;i ++){

        array_pipeline_testcase(connfd);

    }

    struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms
    int total_commands = count * commands_per_test;

    float commands_per_second = (total_commands  * 1000.0) / time_used;

    printf("array pipeline testcase --> time_used: %d ms, commands: %d, qps: %.2f\n", 
           time_used, total_commands, commands_per_second);
}
void rbtree_pipeline_testcase(int connfd) {
    //printf("Testing pipeline functionality...\n");

    // 构建包含多个命令的pipeline请求
    const char *pipeline_cmd = 
        "*3\r\n$4\r\nRSET\r\n$8\r\nRP_Key01\r\n$6\r\nValue1\r\n"
        "*2\r\n$4\r\nRGET\r\n$8\r\nRP_Key01\r\n"
        "*3\r\n$4\r\nRMOD\r\n$8\r\nRP_Key01\r\n$9\r\nNewValue1\r\n"
        "*2\r\n$4\r\nRGET\r\n$8\r\nRP_Key01\r\n"
        "*2\r\n$4\r\nRDEL\r\n$8\r\nRP_Key01\r\n"
        "*2\r\n$4\r\nRGET\r\n$8\r\nRP_Key01\r\n"
        "*3\r\n$4\r\nRSET\r\n$8\r\nRP_Key02\r\n$6\r\nValue2\r\n"
        "*3\r\n$4\r\nRSET\r\n$8\r\nRP_Key03\r\n$6\r\nValue3\r\n"
		"*3\r\n$4\r\nRSET\r\n$8\r\nRP_Key04\r\n$6\r\nValue4\r\n"
        "*2\r\n$5\r\nRSORT\r\n$4\r\nhigh\r\n"
		"*2\r\n$5\r\nRSORT\r\n$3\r\nlow\r\n"
		"*3\r\n$6\r\nRRANGE\r\n$8\r\nRP_Key02\r\n$8\r\nRP_Key04\r\n"
		"*2\r\n$4\r\nRDEL\r\n$8\r\nRP_Key02\r\n"
		"*2\r\n$4\r\nRDEL\r\n$8\r\nRP_Key03\r\n"
		"*2\r\n$4\r\nRDEL\r\n$8\r\nRP_Key04\r\n";

    // 发送pipeline请求
    send_msg(connfd, (char *)pipeline_cmd, strlen(pipeline_cmd));

    char response[4096] = {0};
    int commands_per_pipeline = 15;
    if (recv_pipeline_resps(connfd,
                            commands_per_pipeline,
                            response, sizeof(response)) != 0) {
        printf("==> FAILED -> rbtree pipeline RESP parse error\n");
        exit(1);
    }

    // 验证响应内容
    const char *expected_response = 
        "+OK\r\n"
        "$6\r\nValue1\r\n"
        "+OK\r\n"
        "$9\r\nNewValue1\r\n"
        "+OK\r\n"
        "+NO EXIST\r\n"
        "+OK\r\n"
        "+OK\r\n"
		"+OK\r\n"
        "*3\r\n$15\r\nRP_Key04:Value4\r\n$15\r\nRP_Key03:Value3\r\n$15\r\nRP_Key02:Value2\r\n"
		"*3\r\n$15\r\nRP_Key02:Value2\r\n$15\r\nRP_Key03:Value3\r\n$15\r\nRP_Key04:Value4\r\n"
		"*3\r\n$15\r\nRP_Key02:Value2\r\n$15\r\nRP_Key03:Value3\r\n$15\r\nRP_Key04:Value4\r\n"
		"+OK\r\n"
        "+OK\r\n"
		"+OK\r\n";

    if (strcmp(response, expected_response) == 0) {
        //printf("==> PASS -> Pipeline test\n");
    } else {
        printf("==> FAILED -> Pipeline test\n");
        printf("Expected:\n%s\n", expected_response);
        printf("Received:\n%s\n", response);
    }

}
void rbtreee_pipeline_qps_testcase(int connfd){
	int count = 100000;
    int commands_per_test = 15; // 每次测试中的命令数量
    int i = 0;

    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    
    for(i = 0;i < count;i ++){

        rbtree_pipeline_testcase(connfd);

    }

    struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms
    int total_commands = count * commands_per_test;

    float commands_per_second = (total_commands  * 1000.0) / time_used;

    printf("rbtree pipeline testcase --> time_used: %d ms, commands: %d, qps: %.2f\n", 
           time_used, total_commands, commands_per_second);

}
void hash_pipeline_testcase(int connfd) {
    //printf("Testing pipeline functionality...\n");

    // 构建包含多个命令的pipeline请求
    const char *pipeline_cmd = 
        "*3\r\n$4\r\nHSET\r\n$8\r\nHP_Key01\r\n$6\r\nValue1\r\n"
        "*2\r\n$4\r\nHGET\r\n$8\r\nHP_Key01\r\n"
        "*3\r\n$4\r\nHMOD\r\n$8\r\nHP_Key01\r\n$9\r\nNewValue1\r\n"
        "*2\r\n$4\r\nHGET\r\n$8\r\nHP_Key01\r\n"
        "*2\r\n$4\r\nHDEL\r\n$8\r\nHP_Key01\r\n"
        "*2\r\n$4\r\nHGET\r\n$8\r\nHP_Key01\r\n"
        "*3\r\n$4\r\nHSET\r\n$8\r\nHP_Key02\r\n$6\r\nValue2\r\n"
        "*3\r\n$4\r\nHSET\r\n$8\r\nHP_Key03\r\n$6\r\nValue3\r\n"
        "*3\r\n$4\r\nHSET\r\n$8\r\nHP_Key04\r\n$6\r\nValue4\r\n"
        "*2\r\n$5\r\nHSORT\r\n$4\r\nhigh\r\n"
        "*2\r\n$5\r\nHSORT\r\n$3\r\nlow\r\n"
        "*3\r\n$6\r\nHRANGE\r\n$8\r\nHP_Key02\r\n$8\r\nHP_Key04\r\n"
        "*2\r\n$4\r\nHDEL\r\n$8\r\nHP_Key02\r\n"
        "*2\r\n$4\r\nHDEL\r\n$8\r\nHP_Key03\r\n"
        "*2\r\n$4\r\nHDEL\r\n$8\r\nHP_Key04\r\n";


    // 发送pipeline请求
    send_msg(connfd, (char *)pipeline_cmd, strlen(pipeline_cmd));

    char response[4096] = {0};
    int commands_per_pipeline = 15;
    if (recv_pipeline_resps(connfd,
                            commands_per_pipeline,
                            response, sizeof(response)) != 0) {
        printf("==> FAILED -> hash pipeline RESP parse error\n");
        exit(1);
    }

    // 验证响应内容
   const char *expected_response = 
        "+OK\r\n"
        "$6\r\nValue1\r\n"
        "+OK\r\n"
        "$9\r\nNewValue1\r\n"
        "+OK\r\n"
        "+NO EXIST\r\n"
        "+OK\r\n"
        "+OK\r\n"
        "+OK\r\n"
        "*3\r\n$15\r\nHP_Key04:Value4\r\n$15\r\nHP_Key03:Value3\r\n$15\r\nHP_Key02:Value2\r\n"
        "*3\r\n$15\r\nHP_Key02:Value2\r\n$15\r\nHP_Key03:Value3\r\n$15\r\nHP_Key04:Value4\r\n"
        "*3\r\n$15\r\nHP_Key02:Value2\r\n$15\r\nHP_Key03:Value3\r\n$15\r\nHP_Key04:Value4\r\n"
        "+OK\r\n"
        "+OK\r\n"
        "+OK\r\n";

    if (strcmp(response, expected_response) == 0) {
        //printf("==> PASS -> Pipeline test\n");
    } else {
        printf("==> FAILED -> Pipeline test\n");
        printf("Expected:\n%s\n", expected_response);
        printf("Received:\n%s\n", response);
    }

}
void hash_pipeline_qps_testcase(int connfd){
	int count = 100000;
    int commands_per_test = 15; // 每次测试中的命令数量
    int i = 0;

    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    
    for(i = 0;i < count;i ++){

        hash_pipeline_testcase(connfd);

    }

    struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms
    int total_commands = count * commands_per_test;

    float commands_per_second = (total_commands  * 1000.0) / time_used;

    printf("hash pipeline testcase --> time_used: %d ms, commands: %d, qps: %.2f\n", 
           time_used, total_commands, commands_per_second);
}
void skiptable_pipeline_testcase(int connfd) {
    //printf("Testing pipeline functionality...\n");

    // 构建包含多个命令的pipeline请求
    const char *pipeline_cmd = 
        "*3\r\n$4\r\nSSET\r\n$8\r\nSP_Key01\r\n$6\r\nValue1\r\n"
        "*2\r\n$4\r\nSGET\r\n$8\r\nSP_Key01\r\n"
        "*3\r\n$4\r\nSMOD\r\n$8\r\nSP_Key01\r\n$9\r\nNewValue1\r\n"
        "*2\r\n$4\r\nSGET\r\n$8\r\nSP_Key01\r\n"
        "*2\r\n$4\r\nSDEL\r\n$8\r\nSP_Key01\r\n"
        "*2\r\n$4\r\nSGET\r\n$8\r\nSP_Key01\r\n"
        "*3\r\n$4\r\nSSET\r\n$8\r\nSP_Key02\r\n$6\r\nValue2\r\n"
        "*3\r\n$4\r\nSSET\r\n$8\r\nSP_Key03\r\n$6\r\nValue3\r\n"
		"*3\r\n$4\r\nSSET\r\n$8\r\nSP_Key04\r\n$6\r\nValue4\r\n"
        "*2\r\n$5\r\nSSORT\r\n$4\r\nhigh\r\n"
		"*2\r\n$5\r\nSSORT\r\n$3\r\nlow\r\n"
		"*3\r\n$6\r\nSRANGE\r\n$8\r\nSP_Key02\r\n$8\r\nSP_Key04\r\n"
		"*2\r\n$4\r\nSDEL\r\n$8\r\nSP_Key02\r\n"
		"*2\r\n$4\r\nSDEL\r\n$8\r\nSP_Key03\r\n"
		"*2\r\n$4\r\nSDEL\r\n$8\r\nSP_Key04\r\n";


    // 发送pipeline请求
    send_msg(connfd, (char *)pipeline_cmd, strlen(pipeline_cmd));

    char response[4096] = {0};
    int commands_per_pipeline = 15;
    if (recv_pipeline_resps(connfd,
                            commands_per_pipeline,
                            response, sizeof(response)) != 0) {
        printf("==> FAILED -> skiptable pipeline RESP parse error\n");
        exit(1);
    }

    // 验证响应内容
    const char *expected_response = 
        "+OK\r\n"
        "$6\r\nValue1\r\n"
        "+OK\r\n"
        "$9\r\nNewValue1\r\n"
        "+OK\r\n"
        "+NO EXIST\r\n"
        "+OK\r\n"
        "+OK\r\n"
		"+OK\r\n"
        "*3\r\n$15\r\nSP_Key04:Value4\r\n$15\r\nSP_Key03:Value3\r\n$15\r\nSP_Key02:Value2\r\n"
		"*3\r\n$15\r\nSP_Key02:Value2\r\n$15\r\nSP_Key03:Value3\r\n$15\r\nSP_Key04:Value4\r\n"
		"*3\r\n$15\r\nSP_Key02:Value2\r\n$15\r\nSP_Key03:Value3\r\n$15\r\nSP_Key04:Value4\r\n"
		"+OK\r\n"
        "+OK\r\n"
		"+OK\r\n";

    if (strcmp(response, expected_response) == 0) {
        //printf("==> PASS -> Pipeline test\n");
    } else {
        printf("==> FAILED -> Pipeline test\n");
        printf("Expected:\n%s\n", expected_response);
        printf("Received:\n%s\n", response);
    }
}
void skiptable_pipeline_qps_testcase(int connfd){
	int count = 100000;
    int commands_per_test = 15; // 每次测试中的命令数量
    int i = 0;

    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    
    for(i = 0;i < count;i ++){

        skiptable_pipeline_testcase(connfd);

    }

    struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms
    int total_commands = count * commands_per_test;

    float commands_per_second = (total_commands  * 1000.0) / time_used;

    printf("skiptable pipeline testcase --> time_used: %d ms, commands: %d, qps: %.2f\n", 
           time_used, total_commands, commands_per_second);
}




/*
 1. 简单字符串：以"+"开头，以"\r\n"结尾。例如："+OK\r\n"
 2. 错误：以"-"开头，以"\r\n"结尾。例如："-Error message\r\n"
 3. 整数：以":"开头，以"\r\n"结尾。例如：":1000\r\n"
 4. 批量字符串：以"$"开头，后面跟字符串长度，然后是"\r\n"，然后是字符串，
    最后是"\r\n"。如果字符串为空，则为"$-1\r\n"。例如："$5\r\nhello\r\n"
 5. 数组：以"*"开头，后面跟数组元素个数，然后是"\r\n"，然后每个元素按照自己的类型编码。
    例如：数组["SET", "key", "value"]编码为："*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n"
*/
void array_resp_testcase(int connfd){

	testcase(connfd, "*3\r\n$3\r\nSET\r\n$8\r\narr_key1\r\n$5\r\napple\r\n", "+OK\r\n", "SET-arr_key1");
	testcase(connfd, "*2\r\n$3\r\nGET\r\n$8\r\narr_key1\r\n", "$5\r\napple\r\n", "GET-arr_key1");
	testcase(connfd, "*2\r\n$5\r\nEXIST\r\n$8\r\narr_key1\r\n", "+EXIST\r\n","EXIST-arr_key1");
	testcase(connfd, "*3\r\n$3\r\nMOD\r\n$8\r\narr_key1\r\n$5\r\nAPPLE\r\n", "+OK\r\n","MOD-arr_key1");
	testcase(connfd, "*2\r\n$3\r\nGET\r\n$8\r\narr_key1\r\n", "$5\r\nAPPLE\r\n","GET-arr_key1");
	testcase(connfd, "*2\r\n$3\r\nDEL\r\n$8\r\narr_key1\r\n", "+OK\r\n","DEL-arr_key1");
	testcase(connfd, "*2\r\n$5\r\nEXIST\r\n$8\r\narr_key1\r\n", "+NO EXIST\r\n","EXIST-arr_key1");
	testcase(connfd, "*3\r\n$3\r\nSET\r\n$8\r\narr_key2\r\n$6\r\nbanana\r\n", "+OK\r\n", "SET-arr_key2");
	testcase(connfd, "*3\r\n$3\r\nSET\r\n$8\r\narr_key3\r\n$6\r\ncherry\r\n", "+OK\r\n", "SET-arr_key3");
	testcase(connfd, "*3\r\n$3\r\nSET\r\n$8\r\narr_key4\r\n$4\r\ndate\r\n", "+OK\r\n", "SET-arr_key4");
    testcase(connfd, "*1\r\n$4\r\nSORT\r\n", "*3\r\n$15\r\narr_key2:banana\r\n$15\r\narr_key3:cherry\r\n$13\r\narr_key4:date\r\n", "array_SORT");
    testcase(connfd, "*3\r\n$5\r\nRANGE\r\n$8\r\narr_key2\r\n$8\r\narr_key4\r\n", "*3\r\n$15\r\narr_key2:banana\r\n$15\r\narr_key3:cherry\r\n$13\r\narr_key4:date\r\n", "array_RANGE");

    // // 清理本测试用到的所有 key
    // testcase(connfd, "*2\r\n$3\r\nDEL\r\n$8\r\narr_key1\r\n", "+OK\r\n", "CLEAN-arr_key1");
    testcase(connfd, "*2\r\n$3\r\nDEL\r\n$8\r\narr_key2\r\n", "+OK\r\n", "CLEAN-arr_key2");
    testcase(connfd, "*2\r\n$3\r\nDEL\r\n$8\r\narr_key3\r\n", "+OK\r\n", "CLEAN-arr_key3");
    testcase(connfd, "*2\r\n$3\r\nDEL\r\n$8\r\narr_key4\r\n", "+OK\r\n", "CLEAN-arr_key4");
	
}
void rbtree_resp_testcase(int connfd){

	testcase(connfd, "*3\r\n$4\r\nRSET\r\n$9\r\ntree_key1\r\n$8\r\nelephant\r\n", "+OK\r\n", "RSET-tree_key1");
	testcase(connfd, "*2\r\n$4\r\nRGET\r\n$9\r\ntree_key1\r\n", "$8\r\nelephant\r\n", "RGET-tree_key1");
	testcase(connfd, "*2\r\n$6\r\nREXIST\r\n$9\r\ntree_key1\r\n", "+EXIST\r\n","EXIST-tree_key1");
	testcase(connfd, "*3\r\n$4\r\nRMOD\r\n$9\r\ntree_key1\r\n$8\r\nELEPHANT\r\n", "+OK\r\n","RMOD-tree_key1");
	testcase(connfd, "*2\r\n$4\r\nRGET\r\n$9\r\ntree_key1\r\n", "$8\r\nELEPHANT\r\n","GET-tree_key1");
	testcase(connfd, "*2\r\n$4\r\nRDEL\r\n$9\r\ntree_key1\r\n", "+OK\r\n","DEL-tree_key1");
	testcase(connfd, "*2\r\n$6\r\nREXIST\r\n$9\r\ntree_key1\r\n", "+NO EXIST\r\n","EXIST-tree_key1");
	testcase(connfd, "*3\r\n$4\r\nRSET\r\n$9\r\ntree_key2\r\n$3\r\nfox\r\n", "+OK\r\n", "SET-arr_key2");
	testcase(connfd, "*3\r\n$4\r\nRSET\r\n$9\r\ntree_key3\r\n$7\r\ngiraffe\r\n", "+OK\r\n", "SET-arr_key3");
	testcase(connfd, "*3\r\n$4\r\nRSET\r\n$9\r\ntree_key4\r\n$5\r\nhippo\r\n", "+OK\r\n", "SET-arr_key4");
	testcase(connfd, "*1\r\n$5\r\nRSORT\r\n", "*3\r\n$13\r\ntree_key2:fox\r\n$17\r\ntree_key3:giraffe\r\n$15\r\ntree_key4:hippo\r\n", "rbtree_SORT");
	testcase(connfd, "*3\r\n$6\r\nRRANGE\r\n$9\r\ntree_key2\r\n$9\r\ntree_key4\r\n", "*3\r\n$13\r\ntree_key2:fox\r\n$17\r\ntree_key3:giraffe\r\n$15\r\ntree_key4:hippo\r\n", "rbtree_RANGE");

    // // 清理本测试用到的所有 key
    // testcase(connfd, "*2\r\n$4\r\nRDEL\r\n$9\r\ntree_key1\r\n", "+OK\r\n", "CLEAN-tree_key1");
    testcase(connfd, "*2\r\n$4\r\nRDEL\r\n$9\r\ntree_key2\r\n", "+OK\r\n", "CLEAN-tree_key2");
    testcase(connfd, "*2\r\n$4\r\nRDEL\r\n$9\r\ntree_key3\r\n", "+OK\r\n", "CLEAN-tree_key3");
    testcase(connfd, "*2\r\n$4\r\nRDEL\r\n$9\r\ntree_key4\r\n", "+OK\r\n", "CLEAN-tree_key4");
	
}
void hash_resp_testcase(int connfd){

	testcase(connfd, "*3\r\n$4\r\nHSET\r\n$9\r\nhash_key1\r\n$6\r\niguana\r\n", "+OK\r\n", "HSET-hash_key1");
	testcase(connfd, "*2\r\n$4\r\nHGET\r\n$9\r\nhash_key1\r\n", "$6\r\niguana\r\n", "HGET-hash_key1");
	testcase(connfd, "*2\r\n$6\r\nHEXIST\r\n$9\r\nhash_key1\r\n", "+EXIST\r\n","EXIST-hash_key1");
	testcase(connfd, "*3\r\n$4\r\nHMOD\r\n$9\r\nhash_key1\r\n$6\r\nIGUANA\r\n", "+OK\r\n","HMOD-hash_key1");
	testcase(connfd, "*2\r\n$4\r\nHGET\r\n$9\r\nhash_key1\r\n", "$6\r\nIGUANA\r\n","GET-hash_key1");
	testcase(connfd, "*2\r\n$4\r\nHDEL\r\n$9\r\nhash_key1\r\n", "+OK\r\n","DEL-hash_key1");
	testcase(connfd, "*2\r\n$6\r\nHEXIST\r\n$9\r\nhash_key1\r\n", "+NO EXIST\r\n","EXIST-hash_key1");
	testcase(connfd, "*3\r\n$4\r\nHSET\r\n$9\r\nhash_key2\r\n$6\r\njaguar\r\n", "+OK\r\n", "SET-hash_key2");
	testcase(connfd, "*3\r\n$4\r\nHSET\r\n$9\r\nhash_key3\r\n$5\r\nkoala\r\n", "+OK\r\n", "SET-hash_key3");
	testcase(connfd, "*3\r\n$4\r\nHSET\r\n$9\r\nhash_key4\r\n$5\r\nlemur\r\n", "+OK\r\n", "SET-hash_key4");
	testcase(connfd, "*1\r\n$5\r\nHSORT\r\n", "*3\r\n$16\r\nhash_key2:jaguar\r\n$15\r\nhash_key3:koala\r\n$15\r\nhash_key4:lemur\r\n", "hash_SORT");
	testcase(connfd, "*3\r\n$6\r\nHRANGE\r\n$9\r\nhash_key2\r\n$9\r\nhash_key4\r\n", "*3\r\n$16\r\nhash_key2:jaguar\r\n$15\r\nhash_key3:koala\r\n$15\r\nhash_key4:lemur\r\n", "hash_RANGE");

    // // 清理本测试用到的所有 key
    // testcase(connfd, "*2\r\n$4\r\nHDEL\r\n$9\r\nhash_key1\r\n", "+OK\r\n", "CLEAN-hash_key1");
    testcase(connfd, "*2\r\n$4\r\nHDEL\r\n$9\r\nhash_key2\r\n", "+OK\r\n", "CLEAN-hash_key2");
    testcase(connfd, "*2\r\n$4\r\nHDEL\r\n$9\r\nhash_key3\r\n", "+OK\r\n", "CLEAN-hash_key3");
    testcase(connfd, "*2\r\n$4\r\nHDEL\r\n$9\r\nhash_key4\r\n", "+OK\r\n", "CLEAN-hash_key4");
	
}
void skiptable_resp_testcase(int connfd){

	testcase(connfd, "*3\r\n$4\r\nSSET\r\n$9\r\nskip_key1\r\n$6\r\nmonkey\r\n", "+OK\r\n", "SSET-skip_key1");
	testcase(connfd, "*2\r\n$4\r\nSGET\r\n$9\r\nskip_key1\r\n", "$6\r\nmonkey\r\n", "DGET-skip_key1");
	testcase(connfd, "*2\r\n$6\r\nSEXIST\r\n$9\r\nskip_key1\r\n", "+EXIST\r\n","EXIST-skip_key1");
	testcase(connfd, "*3\r\n$4\r\nSMOD\r\n$9\r\nskip_key1\r\n$6\r\nMONKEY\r\n", "+OK\r\n","HMOD-skip_key1");
	testcase(connfd, "*2\r\n$4\r\nSGET\r\n$9\r\nskip_key1\r\n", "$6\r\nMONKEY\r\n","GET-skip_key1");
	testcase(connfd, "*2\r\n$4\r\nSDEL\r\n$9\r\nskip_key1\r\n", "+OK\r\n","DEL-skip_key1");
	testcase(connfd, "*2\r\n$6\r\nSEXIST\r\n$9\r\nskip_key1\r\n", "+NO EXIST\r\n","EXIST-skip_key1");
	testcase(connfd, "*3\r\n$4\r\nSSET\r\n$9\r\nskip_key2\r\n$7\r\nnarwhal\r\n", "+OK\r\n", "SET-skip_key2");
	testcase(connfd, "*3\r\n$4\r\nSSET\r\n$9\r\nskip_key3\r\n$7\r\noctopus\r\n", "+OK\r\n", "SET-skip_key3");
	testcase(connfd, "*3\r\n$4\r\nSSET\r\n$9\r\nskip_key4\r\n$7\r\npenguin\r\n", "+OK\r\n", "SET-skip_key4");
	testcase(connfd, "*1\r\n$5\r\nSSORT\r\n", "*3\r\n$17\r\nskip_key2:narwhal\r\n$17\r\nskip_key3:octopus\r\n$17\r\nskip_key4:penguin\r\n", "skiptable_SORT");
	testcase(connfd, "*3\r\n$6\r\nSRANGE\r\n$9\r\nskip_key2\r\n$9\r\nskip_key4\r\n", "*3\r\n$17\r\nskip_key2:narwhal\r\n$17\r\nskip_key3:octopus\r\n$17\r\nskip_key4:penguin\r\n", "skiptable_RANGE");

    // 清理本测试用到的所有 key
    // testcase(connfd, "*2\r\n$4\r\nSDEL\r\n$9\r\nskip_key1\r\n", "+OK\r\n", "CLEAN-skip_key1");
    testcase(connfd, "*2\r\n$4\r\nSDEL\r\n$9\r\nskip_key2\r\n", "+OK\r\n", "CLEAN-skip_key2");
    testcase(connfd, "*2\r\n$4\r\nSDEL\r\n$9\r\nskip_key3\r\n", "+OK\r\n", "CLEAN-skip_key3");
    testcase(connfd, "*2\r\n$4\r\nSDEL\r\n$9\r\nskip_key4\r\n", "+OK\r\n", "CLEAN-skip_key4");
	
}
 
/**
 * @brief 生成包含i条SET命令的RESP管道
 * @param count 要生成的SET命令数量
 * @param key_prefix 键名前缀
 * @param value_prefix 值前缀
 * @return RESP管道字符串，需要调用者free释放
 */
char* generate_set_pipeline(int count, const char* key_prefix, const char* value_prefix) {
    if (count <= 0 || key_prefix == NULL || value_prefix == NULL) {
        return NULL;
    }

    // 第一次遍历：计算总长度
    size_t total_len = 0;
    
    for (int i = 0; i < count; i++) {
        // 生成key和value
        char key[256], value[256];
        snprintf(key, sizeof(key), "%s%d", key_prefix, i + 1);
        snprintf(value, sizeof(value), "%s%d", value_prefix, i + 1);
        
        // 计算这条SET命令的长度
        total_len += snprintf(NULL, 0, "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n", 
                            strlen(key), key, strlen(value), value);
    }
    
    // 分配内存
    char* pipeline = (char*)malloc(total_len + 1);
    if (!pipeline) {
        return NULL;
    }
    
    // 第二次遍历：实际写入
    size_t written = 0;
    for (int i = 0; i < count; i++) {
        char key[256], value[256];
        snprintf(key, sizeof(key), "%s%d", key_prefix, i + 1);
        snprintf(value, sizeof(value), "%s%d", value_prefix, i + 1);
        
        size_t key_len = strlen(key);
        size_t value_len = strlen(value);
        
        written += sprintf(pipeline + written, "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n", 
                         key_len, key, value_len, value);
    }
    
    pipeline[written] = '\0';
    return pipeline;
}

/**
 * @brief 生成包含i条SET命令的RESP管道（带自定义格式）
 * @param count 命令数量
 * @param key_format 键名格式字符串（包含%d占位符）
 * @param value_format 值格式字符串（包含%d占位符）
 * @return RESP管道字符串
 */
char* generate_custom_set_pipeline(int count, const char* key_format, const char* value_format) {
    if (count <= 0 || key_format == NULL || value_format == NULL) {
        return NULL;
    }

    size_t total_len = 0;
    
    // 计算总长度
    for (int i = 0; i < count; i++) {
        char key[256], value[256];
        snprintf(key, sizeof(key), key_format, i + 1);
        snprintf(value, sizeof(value), value_format, i + 1);
        
        total_len += snprintf(NULL, 0, "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n", 
                            strlen(key), key, strlen(value), value);
    }
    
    char* pipeline = (char*)malloc(total_len + 1);
    if (!pipeline) return NULL;
    
    size_t written = 0;
    for (int i = 0; i < count; i++) {
        char key[256], value[256];
        snprintf(key, sizeof(key), key_format, i + 1);
        snprintf(value, sizeof(value), value_format, i + 1);
        
        written += sprintf(pipeline + written, "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n", 
                         strlen(key), key, strlen(value), value);
    }
    
    pipeline[written] = '\0';
    return pipeline;
}
/* 辅助：读取一行，返回行长度(包含 \r\n)，不够返回 0，错误返回 -1 */
static int read_line(const char *buf, int len) {
    for (int i = 0; i < len - 1; ++i) {
        if (buf[i] == '\r' && buf[i+1] == '\n') {
            return i + 2; // 包含 \r\n
        }
    }
    return 0; // 不够一行
}

/* 辅助：从形如 ":123\r\n" / "$5\r\n" / "*10\r\n" 中解析整数 */
static int parse_integer(const char *buf, int len, long long *value, int *line_len) {
    int l = read_line(buf, len);
    if (l == 0) return 0;           // 数据不够
    if (buf[l-2] != '\r' || buf[l-1] != '\n') return -1;

    long long v = 0;
    int sign = 1;
    int i = 0;
    if (buf[0] == '-' || buf[0] == '+') {
        if (buf[0] == '-') sign = -1;
        i = 1;
    }
    for (; i < l-2; ++i) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        v = v * 10 + (buf[i] - '0');
    }
    *value   = v * sign;
    *line_len = l;
    return 1;
}

static int parse_resp_object(const char *buf, int len, int *consumed) {
    if (len <= 0) return 0;

    // 1. 跳过前导空白字符（\r, \n, 空格, \t）
    int skip = 0;
    while (skip < len) {
        char c = buf[skip];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            skip++;
        } else {
            break;
        }
    }
    if (skip >= len) return 0;  // 全是空白，数据还不够

    // 2. 真正的 RESP 类型
    char type = buf[skip];
    buf += skip + 1;   // 跳过空白和类型字节
    len -= skip + 1;

    int line_len = 0;
    long long num = 0;
    int ret = 0;
    int used = skip + 1; // 已经消耗的字节数（空白+类型）

    switch (type) {
    case '+': // 简单字符串
    case '-': // 错误
    case ':': { // 整数
        int l = read_line(buf, len);
        if (l == 0) return 0;     // 数据不够
        if (type == ':') {
            long long dummy;
            int ll;
            ret = parse_integer(buf, len, &dummy, &ll);
            if (ret <= 0) return ret;
            l = ll;
        }
        *consumed = used + l;
        return 1;
    }

    case '$': { // 批量字符串
        ret = parse_integer(buf, len, &num, &line_len);
        if (ret <= 0) return ret;
        used += line_len;
        buf  += line_len;
        len  -= line_len;

        if (num < 0) {
            *consumed = used;
            return 1;
        }
        if (len < num + 2) return 0;
        if (buf[num] != '\r' || buf[num+1] != '\n') return -1;

        used += (int)(num + 2);
        *consumed = used;
        return 1;
    }

    case '*': { // 数组
        ret = parse_integer(buf, len, &num, &line_len);
        if (ret <= 0) return ret;
        used += line_len;
        buf  += line_len;
        len  -= line_len;

        if (num < 0) {
            *consumed = used;
            return 1;
        }
        for (long long i = 0; i < num; ++i) {
            int sub_used = 0;
            ret = parse_resp_object(buf, len, &sub_used);
            if (ret <= 0) return ret;
            buf  += sub_used;
            len  -= sub_used;
            used += sub_used;
        }
        *consumed = used;
        return 1;
    }

    default:
        return -1;
    }
}


static int parse_simple_resp_line(const char *buf, int len, int *consumed) {
    if (len <= 0) return 0;

    int skip = 0;
    while (skip < len) {
        char c = buf[skip];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            skip++;
        } else {
            break;
        }
    }
    if (skip >= len) return 0;

    char type = buf[skip];
    if (type != '+' && type != '-') {
        // 对于 huge pipeline 测试，理论上只会返回简单字符串/错误
        // 若遇到其它类型，直接当作错误
        return -1;
    }

    const char *p = buf + skip + 1;   // 跳过 '+' 或 '-'
    int remain   = len - (skip + 1);

    int l = read_line(p, remain);
    if (l == 0) return 0;  // 不够一行

    // p[l-2]==\r, p[l-1]==\n 在 read_line 中已经保证
    *consumed = skip + 1 + l;
    return 1;
}

void hugepacket_testcase(int connfd, int command_count) {
    if (command_count <= 0) {
        printf("command_count must > 0\n");
        return;
    }

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    // 1. 构造 pipeline 命令
    char *pipeline_cmd = generate_set_pipeline(command_count, "key", "value");
    if (!pipeline_cmd) {
        printf("Failed to generate pipeline\n");
        return;
    }
    size_t total_len = strlen(pipeline_cmd);
    printf("Sending %d commands (%zu bytes)...\n", command_count, total_len);

    // 2. 用 send_msg 阻塞发送全部数据
    int sent = send_msg(connfd, pipeline_cmd, (int)total_len);
    if (sent != (int)total_len) {
        printf("Send incomplete: sent=%d, expected=%zu\n", sent, total_len);
        free(pipeline_cmd);
        return;
    }
    // printf("Sent %d bytes\n", sent);

    // 3. 接收响应：按 RESP 简单字符串 / 错误 一条条解析
    size_t buf_cap = 1024 * 1024;
    char *buf = (char *)malloc(buf_cap);
    if (!buf) {
        printf("Failed to allocate recv buffer\n");
        free(pipeline_cmd);
        return;
    }

    int total_received = 0;
    int resp_count     = 0;
    int eof            = 0;

    // 设置接收超时（例如 5 秒，防止永久阻塞）
    // struct timeval tv;
    // tv.tv_sec  = 5;
    // tv.tv_usec = 0;
    // setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    while (resp_count < command_count) {
        // 如果还没 EOF，先尝试多收一点数据
        if (!eof) {
            if (buf_cap - total_received < 4096) {
                buf_cap *= 2;
                char *nb = (char *)realloc(buf, buf_cap);
                if (!nb) {
                    printf("Failed to expand recv buffer\n");
                    free(buf);
                    free(pipeline_cmd);
                    return;
                }
                buf = nb;
            }

            int n = recv(connfd, buf + total_received,
                         (int)(buf_cap - total_received - 1), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINVAL || errno == EINTR) {
                    // 没有新数据了，继续用现有 buf 尝试解析
                } else {
                    printf("recv error: %s\n", strerror(errno));
                    break;
                }
            } else if (n == 0) {
                // 对端关闭，标记 EOF
                printf("Server closed connection.\n");
                eof = 1;
            } else {
                total_received += n;
            }
        }

        // 尽量多地从 buf 里解析简单 RESP 行（+ 或 -）
        int offset = 0;
        int old_resp = resp_count;
        while (offset < total_received) {
            int consumed = 0;
            int ret = parse_simple_resp_line(buf + offset,
                                             total_received - offset,
                                             &consumed);
            if (ret == 1) {
                resp_count++;
                offset += consumed;
            } else if (ret == 0) {
                // 数据不够一行，等待更多 recv
                break;
            } else {
                printf("Simple RESP parse error around offset=%d\n", offset);
                printf("Dump: ");
                for (int i = 0; i < 32 && offset + i < total_received; ++i) {
                    unsigned char c = (unsigned char)buf[offset + i];
                    if (c >= 32 && c <= 126) putchar(c);
                    else if (c == '\r') printf("\\r");
                    else if (c == '\n') printf("\\n");
                    else printf("\\x%02X", c);
                }
                printf("\n");
                free(buf);
                free(pipeline_cmd);
                return;
            }
        }

        if (offset > 0 && offset < total_received) {
            memmove(buf, buf + offset, total_received - offset);
        }
        total_received -= offset;

        // printf("buffer_remain=%d, resp_count=%d\n", total_received, resp_count);

        // 如果已经 EOF 且这轮解析没有任何进展，则退出
        if (eof && old_resp == resp_count) {
            break;
        }
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms

    if (resp_count == command_count) {
        double qps = (command_count * 1000.0) / (time_used > 0 ? time_used : 1);
        printf("==> PASS -> Huge packet test (%d commands), time_used: %d ms, qps: %.2f\n",
               command_count, time_used, qps);
    } else {
        printf("==> FAILED -> Huge packet test\n");
        printf("Expected %d responses, got %d\n", command_count, resp_count);
    }

    free(buf);
    free(pipeline_cmd);
}



//在测试用例间添加连接清理
void cleanup_connction(int connfd){

    //清空可能的残留响应
    char dummy_buf[1024];
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    while(recv(connfd, dummy_buf, sizeof(dummy_buf)-1, 0) > 0){
        //继续读取直到没有数据
    }

    //恢复默认超时
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

}

// master-slave sync tests: write on master, read on slave
// master-slave sync tests: write on master, read on slave
static void master_slave_array_test(const char *master_ip, int master_port,
                                    const char *slave_ip, int slave_port) {
    int master_fd = connect_tcpserver(master_ip, master_port);
    int slave_fd  = connect_tcpserver(slave_ip,  slave_port);
    if (master_fd < 0 || slave_fd < 0) {
        printf("connect master/slave failed\n");
        return;
    }

    int count = 100000;              // key 数量

    char cmd[256];
    char buf[256];

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    // 1. 在 master 上写 count 个 key，初始值 value1
    for (int i = 0; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$3\r\nSET\r\n$14\r\nMSA_key_%06d\r\n$6\r\nvalue1\r\n",
                 i);
        send_msg(master_fd, cmd, strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf)); // expect +OK
    }

    // 2. 等待主从同步
    sleep(5);
    // 3. 第一次：验证初始写入同步是否正确
    char *get_pipeline = generate_get_pipeline(count);
    if (!get_pipeline) {
        printf("generate_get_pipeline failed\n");
        return;
    }

    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    char *master_resp = NULL;
    size_t master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master pipeline resp failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    char *slave_resp = NULL;
    size_t slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave pipeline resp failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

    // 对 array 模式暂时仍按原始 RESP 比较
    int equal = 0;
    if (master_len == slave_len &&
        memcmp(master_resp, slave_resp, master_len) == 0) {
        equal = 1;
    }

    printf("array master-slave pipeline [SET] compare: keys %d, master_len %zu, "
           "slave_len %zu, %s\n",
           count, master_len, slave_len,
           equal ? "SYNC OK" : "SYNC MISMATCH");

    if (!equal) {
        int dump = (int)((master_len < 256) ? master_len : 256);
        printf("[SET] master_resp (first %d bytes):\n%.*s\n", dump, dump, master_resp);
        dump = (int)((slave_len < 256) ? slave_len : 256);
        printf("[SET] slave_resp (first %d bytes):\n%.*s\n", dump, dump, slave_resp);
    }

    free(master_resp);
    free(slave_resp);

    // 4. 在 master 上对一半 key 做 MOD，另一半做 DEL
    int half = count / 2;
    for (int i = 0; i < half; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$3\r\nMOD\r\n$14\r\nMSA_key_%06d\r\n$6\r\nvalue2\r\n",
                 i);
        send_msg(master_fd, cmd, strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }
    for (int i = half; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*2\r\n$3\r\nDEL\r\n$14\r\nMSA_key_%06d\r\n", i);
        send_msg(master_fd, cmd, strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }

    // 5. 再次等待主从同步
    sleep(2);

    // 6. 第二次：验证 MOD/DEL 后的数据是否同步
    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    master_resp = NULL;
    master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master pipeline resp after MOD/DEL failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    slave_resp = NULL;
    slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave pipeline resp after MOD/DEL failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms

    equal = 0;
    if (master_len == slave_len &&
        memcmp(master_resp, slave_resp, master_len) == 0) {
        equal = 1;
    }

    printf("array master-slave pipeline [MOD/DEL] compare: keys %d, time_used %d ms, "
           "master_len %zu, slave_len %zu, %s\n",
           count, time_used, master_len, slave_len,
           equal ? "SYNC OK" : "SYNC MISMATCH");

    if (!equal) {
        int dump = (int)((master_len < 256) ? master_len : 256);
        printf("[MOD/DEL] master_resp (first %d bytes):\n%.*s\n", dump, dump, master_resp);
        dump = (int)((slave_len < 256) ? slave_len : 256);
        printf("[MOD/DEL] slave_resp (first %d bytes):\n%.*s\n", dump, dump, slave_resp);
    }

    free(get_pipeline);
    free(master_resp);
    free(slave_resp);
}

// 解析简单流水线 GET/RGET/HGET/SGET 响应，将每个 key 对应的 bulk string
// value 按顺序提取出来放到 values[count][buf] 里；返回解析出的条目数。
// 这里利用前面已经实现的 RESP 解析函数 parse_resp_object。
static int parse_values_from_resp(const char *buf, size_t len,
                                  char values[][32], int max_values) {
    int parsed = 0;
    int offset = 0;

    while (offset < (int)len && parsed < max_values) {
        int consumed = 0;
        int ret = parse_resp_object(buf + offset, (int)len - offset, &consumed);
        if (ret <= 0) break;

        // 简单处理：只关心形如 $6\r\nvalueX\r\n 的 bulk string
        if (buf[offset] == '$') {
            // 找到第一个 CRLF，取得长度
            int line_len = read_line(buf + offset + 1, (int)len - offset - 1);
            if (line_len <= 0) break;
            long long bulk_len = 0;
            int dummy = 0;
            if (parse_integer(buf + offset + 1, (int)len - offset - 1,
                              &bulk_len, &dummy) != 1) {
                break;
            }
            const char *data = buf + offset + 1 + dummy;
            if (bulk_len > 0 &&
                data + bulk_len + 2 <= buf + len &&
                bulk_len <  (long long)sizeof(values[0])) {
                memcpy(values[parsed], data, (size_t)bulk_len);
                values[parsed][bulk_len] = '\0';
                parsed++;
            }
        }

        offset += consumed;
    }

    return parsed;
}

// 比较两端 RESP 流解析出的 value 序列是否一致（忽略原始字节长度差异）
static int compare_resp_value_sequences(const char *master_buf, size_t master_len,
                                        const char *slave_buf, size_t slave_len,
                                        const char *name, const char *phase,
                                        int keys, int time_used) {
    char master_values[100000][32];
    char slave_values[100000][32];

    int m_cnt = parse_values_from_resp(master_buf, master_len,
                                       master_values, 100000);
    int s_cnt = parse_values_from_resp(slave_buf, slave_len,
                                       slave_values, 100000);

    int equal = 1;
    if (m_cnt != s_cnt) {
        equal = 0;
    } else {
        for (int i = 0; i < m_cnt; ++i) {
            if (strcmp(master_values[i], slave_values[i]) != 0) {
                equal = 0;
                break;
            }
        }
    }

    if (time_used >= 0) {
        printf("%s master-slave pipeline [%s] compare: keys %d, time_used %d ms, "
               "master_len %zu, slave_len %zu, %s\n",
               name, phase, keys, time_used, master_len, slave_len,
               equal ? "SYNC OK" : "SYNC MISMATCH");
    } else {
        printf("%s master-slave pipeline [%s] compare: keys %d, master_len %zu, "
               "slave_len %zu, %s\n",
               name, phase, keys, master_len, slave_len,
               equal ? "SYNC OK" : "SYNC MISMATCH");
    }

    if (!equal) {
        int dump = (int)((master_len < 256) ? master_len : 256);
        printf("[%s] %s master_resp (first %d bytes):\n%.*s\n",
               phase, name, dump, dump, master_buf);
        dump = (int)((slave_len < 256) ? slave_len : 256);
        printf("[%s] %s slave_resp (first %d bytes):\n%.*s\n",
               phase, name, dump, dump, slave_buf);
    }

    return equal;
}

static void master_slave_rbtree_test(const char *master_ip, int master_port,
                                     const char *slave_ip, int slave_port) {
    int master_fd = connect_tcpserver(master_ip, master_port);
    int slave_fd  = connect_tcpserver(slave_ip,  slave_port);
    if (master_fd < 0 || slave_fd < 0) {
        printf("connect master/slave failed\n");
        return;
    }

    int count = 100000;
    char cmd[256];
    char buf[256];

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    /* 第一阶段：RSET 同步校验 */
    for (int i = 0; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$4\r\nRSET\r\n$14\r\nMSR_key_%06d\r\n$6\r\nvalue1\r\n",
                 i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));    // 期望 +OK
    }

    sleep(5);

    char *get_pipeline = generate_rbtree_get_pipeline(count);
    if (!get_pipeline) {
        printf("generate_rbtree_get_pipeline failed\n");
        return;
    }

    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    char *master_resp = NULL;
    size_t master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master rbtree pipeline [SET] resp failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    char *slave_resp = NULL;
    size_t slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave rbtree pipeline [SET] resp failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

    // rbtree SET 阶段：按 value 序列比较主从
    compare_resp_value_sequences(master_resp, master_len,
                                 slave_resp, slave_len,
                                 "rbtree", "SET", count, -1);

    free(master_resp);
    free(slave_resp);

    /* 第二阶段：前半 RMOD value2，后半 RDEL，校验 MOD/DEL 同步 */
    int half = count / 2;
    for (int i = 0; i < half; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$4\r\nRMOD\r\n$14\r\nMSR_key_%06d\r\n$6\r\nvalue2\r\n",
                 i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }
    for (int i = half; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*2\r\n$4\r\nRDEL\r\n$14\r\nMSR_key_%06d\r\n", i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }

    sleep(2);

    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    master_resp = NULL;
    master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master rbtree pipeline [MOD/DEL] resp failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    slave_resp = NULL;
    slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave rbtree pipeline [MOD/DEL] resp failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    // rbtree MOD/DEL 阶段：按 value 序列比较主从
    compare_resp_value_sequences(master_resp, master_len,
                                 slave_resp, slave_len,
                                 "rbtree", "MOD/DEL", count, time_used);

    free(get_pipeline);
    free(master_resp);
    free(slave_resp);
}

static void master_slave_hash_test(const char *master_ip, int master_port,
                                   const char *slave_ip, int slave_port) {
    int master_fd = connect_tcpserver(master_ip, master_port);
    int slave_fd  = connect_tcpserver(slave_ip,  slave_port);
    if (master_fd < 0 || slave_fd < 0) {
        printf("connect master/slave failed\n");
        return;
    }

    int count = 100000;
    char cmd[256];
    char buf[256];

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    /* 第一阶段：HSET 同步校验 */
    for (int i = 0; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$4\r\nHSET\r\n$14\r\nMSH_key_%06d\r\n$6\r\nvalue1\r\n",
                 i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }

    sleep(5);

    char *get_pipeline = generate_hash_get_pipeline(count);
    if (!get_pipeline) {
        printf("generate_hash_get_pipeline failed\n");
        return;
    }

    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    char *master_resp = NULL;
    size_t master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master hash pipeline [SET] resp failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    char *slave_resp = NULL;
    size_t slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave hash pipeline [SET] resp failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

    // hash SET 阶段：按 value 序列比较主从
    compare_resp_value_sequences(master_resp, master_len,
                                 slave_resp, slave_len,
                                 "hash", "SET", count, -1);

    free(master_resp);
    free(slave_resp);

    /* 第二阶段：前半 HMOD value2，后半 HDEL，校验 MOD/DEL 同步 */
    int half = count / 2;
    for (int i = 0; i < half; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$4\r\nHMOD\r\n$14\r\nMSH_key_%06d\r\n$6\r\nvalue2\r\n",
                 i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }
    for (int i = half; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*2\r\n$4\r\nHDEL\r\n$14\r\nMSH_key_%06d\r\n", i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }

    sleep(2);

    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    master_resp = NULL;
    master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master hash pipeline [MOD/DEL] resp failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    slave_resp = NULL;
    slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave hash pipeline [MOD/DEL] resp failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);

    // hash MOD/DEL 阶段：按 value 序列比较主从
    compare_resp_value_sequences(master_resp, master_len,
                                 slave_resp, slave_len,
                                 "hash", "MOD/DEL", count, time_used);

    free(get_pipeline);
    free(master_resp);
    free(slave_resp);
}

static void master_slave_skiptable_test(const char *master_ip, int master_port,
                                        const char *slave_ip, int slave_port) {
    int master_fd = connect_tcpserver(master_ip, master_port);
    int slave_fd  = connect_tcpserver(slave_ip,  slave_port);
    if (master_fd < 0 || slave_fd < 0) {
        printf("connect master/slave failed\n");
        return;
    }

    int count = 100000;
    char cmd[256];
    char buf[256];

    struct timeval tv_begin, tv_end;
    gettimeofday(&tv_begin, NULL);

    /* 第一阶段：SSET 同步校验 */
    for (int i = 0; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$4\r\nSSET\r\n$14\r\nMSS_key_%06d\r\n$6\r\nvalue1\r\n",
                 i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }

    sleep(5);

    char *get_pipeline = generate_skiptable_get_pipeline(count);
    if (!get_pipeline) {
        printf("generate_skiptable_get_pipeline failed\n");
        return;
    }

    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    char *master_resp = NULL;
    size_t master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master skiptable pipeline [SET] resp failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    char *slave_resp = NULL;
    size_t slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave skiptable pipeline [SET] resp failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

        compare_resp_value_sequences(master_resp, master_len,
                         slave_resp, slave_len,
                         "skiptable", "SET", count, -1);

    free(master_resp);
    free(slave_resp);

    /* 第二阶段：前半 SMOD value2，后半 SDEL，校验 MOD/DEL 同步 */
    int half = count / 2;
    for (int i = 0; i < half; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*3\r\n$4\r\nSMOD\r\n$14\r\nMSS_key_%06d\r\n$6\r\nvalue2\r\n",
                 i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }
    for (int i = half; i < count; ++i) {
        snprintf(cmd, sizeof(cmd),
                 "*2\r\n$4\r\nSDEL\r\n$14\r\nMSS_key_%06d\r\n", i);
        send_msg(master_fd, cmd, (int)strlen(cmd));
        recv_full_resp(master_fd, buf, sizeof(buf));
    }

    sleep(2);

    send_msg(master_fd, get_pipeline, (int)strlen(get_pipeline));
    master_resp = NULL;
    master_len = 0;
    if (recv_all_with_timeout(master_fd, &master_resp, &master_len) != 0) {
        printf("recv master skiptable pipeline [MOD/DEL] resp failed\n");
        free(get_pipeline);
        return;
    }

    send_msg(slave_fd, get_pipeline, (int)strlen(get_pipeline));
    slave_resp = NULL;
    slave_len = 0;
    if (recv_all_with_timeout(slave_fd, &slave_resp, &slave_len) != 0) {
        printf("recv slave skiptable pipeline [MOD/DEL] resp failed\n");
        free(get_pipeline);
        free(master_resp);
        return;
    }

    gettimeofday(&tv_end, NULL);
    int time_used = TIME_SUB_MS(tv_end, tv_begin);

        compare_resp_value_sequences(master_resp, master_len,
                         slave_resp, slave_len,
                         "skiptable", "MOD/DEL", count, time_used);

    free(get_pipeline);
    free(master_resp);
    free(slave_resp);
}


// testcase master_ip master_port mode [slave_ip slave_port]
int main(int argc, char *argv[]) {

	if (argc < 4) {
		printf("usage: %s ip port mode [slave_ip slave_port]\n", argv[0]);
		return -1;
	}

	char *ip = argv[1];
	int port = atoi(argv[2]);
	int mode = atoi(argv[3]);

    // master-slave sync modes: 6~9，需要提供从节点 ip/port
	if (mode >= 6 && mode <= 9) {
		if (argc < 6) {
			printf("master-slave mode need slave_ip slave_port\n");
			return -1;
		}
		char *slave_ip = argv[4];
		int slave_port = atoi(argv[5]);

        if (mode == 6) {
            master_slave_array_test(ip, port, slave_ip, slave_port);
        } else if (mode == 7) {
            master_slave_rbtree_test(ip, port, slave_ip, slave_port);
        } else if (mode == 8) {
            master_slave_hash_test(ip, port, slave_ip, slave_port);
        } else if (mode == 9) {
            master_slave_skiptable_test(ip, port, slave_ip, slave_port);
        }
		return 0;
	}

	int connfd = connect_tcpserver(ip, port);

    if (mode == 1) {
        cleanup_connction(connfd);
        array_resp_testcase(connfd);
    } else if (mode == 2) {
        cleanup_connction(connfd);
        rbtree_resp_testcase(connfd);
    } else if (mode == 3) {
        cleanup_connction(connfd);
        hash_resp_testcase(connfd);
    } else if (mode == 4) {
        cleanup_connction(connfd);
        skiptable_resp_testcase(connfd);
    }  else if (mode == 5) {
        cleanup_connction(connfd);
        hugepacket_testcase(connfd, 1000000);
    }
    
	return 0;
	
}



