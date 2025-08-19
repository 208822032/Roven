

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/time.h>


#define MAX_MSG_LENGTH		1024
#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)


int send_msg(int connfd, char *msg, int length) {

	int res = send(connfd, msg, length, 0);
	if (res < 0) {
		perror("send");
		exit(1);
	}
	return res;
}

int recv_msg(int connfd, char *msg, int length) {

	int res = recv(connfd, msg, length, 0);
	if (res < 0) {
		perror("recv");
		exit(1);
	}
	return res;

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
void array_test_sort_range(int connfd) {
    // 准备测试数据
    testcase(connfd, "SET A_Apple Red", "OK\r\n", "SET-A_Apple");
    testcase(connfd, "SET B_Banana Yellow", "OK\r\n", "SET-B_Banana");
    testcase(connfd, "SET C_Cherry Red", "OK\r\n", "SET-C_Cherry");
    testcase(connfd, "SET D_Grape Purple", "OK\r\n", "SET-D_Grape");
    testcase(connfd, "SET E_Orange Orange", "OK\r\n", "SET-E_Orange");
    
    // 测试排序
    testcase(connfd, "SORT", "A_Apple:Red\r\nB_Banana:Yellow\r\nC_Cherry:Red\r\nD_Grape:Purple\r\nE_Orange:Orange\r\n", "SORT-Array");
    
    // 测试范围查询
    testcase(connfd, "RANGE B_Banana D_Grape", "B_Banana:Yellow\r\nC_Cherry:Red\r\nD_Grape:Purple\r\n", "RANGE-Array");
    
    //清理测试数据
    testcase(connfd, "DEL A_Apple", "OK\r\n", "DEL-A_Apple");
    testcase(connfd, "DEL B_Banana", "OK\r\n", "DEL-B_Banana");
    testcase(connfd, "DEL C_Cherry", "OK\r\n", "DEL-C_Cherry");
    testcase(connfd, "DEL D_Grape", "OK\r\n", "DEL-D_Grape");
    testcase(connfd, "DEL E_Orange", "OK\r\n", "DEL-E_Orange");
}

void rbtree_test_sort_range(int connfd) {
    // 准备测试数据
    testcase(connfd, "RSET R_Apple Red", "OK\r\n", "RSET-R_Apple");
    testcase(connfd, "RSET R_Banana Yellow", "OK\r\n", "RSET-R_Banana");
    testcase(connfd, "RSET R_Cherry Red", "OK\r\n", "RSET-R_Cherry");
    testcase(connfd, "RSET R_Grape Purple", "OK\r\n", "RSET-R_Grape");
    testcase(connfd, "RSET R_Orange Orange", "OK\r\n", "RSET-R_Orange");
    
    // 测试排序
    testcase(connfd, "RSORT", "R_Apple:Red\r\nR_Banana:Yellow\r\nR_Cherry:Red\r\nR_Grape:Purple\r\nR_Orange:Orange\r\n", "RSORT-RBTree");
    
    // 测试范围查询
    testcase(connfd, "RRANGE R_Banana R_Grape", "R_Banana:Yellow\r\nR_Cherry:Red\r\nR_Grape:Purple\r\n", "RRANGE-RBTree");
    
    // 清理测试数据
    testcase(connfd, "RDEL R_Apple", "OK\r\n", "RDEL-R_Apple");
    testcase(connfd, "RDEL R_Banana", "OK\r\n", "RDEL-R_Banana");
    testcase(connfd, "RDEL R_Cherry", "OK\r\n", "RDEL-R_Cherry");
    testcase(connfd, "RDEL R_Grape", "OK\r\n", "RDEL-R_Grape");
    testcase(connfd, "RDEL R_Orange", "OK\r\n", "RDEL-R_Orange");
}

void hash_test_sort_range(int connfd) {
    // 准备测试数据
    testcase(connfd, "HSET H_Apple Red", "OK\r\n", "HSET-H_Apple");
    testcase(connfd, "HSET H_Banana Yellow", "OK\r\n", "HSET-H_Banana");
    testcase(connfd, "HSET H_Cherry Red", "OK\r\n", "HSET-H_Cherry");
    testcase(connfd, "HSET H_Grape Purple", "OK\r\n", "HSET-H_Grape");
    testcase(connfd, "HSET H_Orange Orange", "OK\r\n", "HSET-H_Orange");
    
    // 测试排序
    testcase(connfd, "HSORT", "H_Apple:Red\r\nH_Banana:Yellow\r\nH_Cherry:Red\r\nH_Grape:Purple\r\nH_Orange:Orange\r\n", "HSORT-Hash");
    
    // 测试范围查询
    testcase(connfd, "HRANGE H_Banana H_Grape", "H_Banana:Yellow\r\nH_Cherry:Red\r\nH_Grape:Purple\r\n", "HRANGE-Hash");
    
    // 清理测试数据
    testcase(connfd, "HDEL H_Apple", "OK\r\n", "HDEL-H_Apple");
    testcase(connfd, "HDEL H_Banana", "OK\r\n", "HDEL-H_Banana");
    testcase(connfd, "HDEL H_Cherry", "OK\r\n", "HDEL-H_Cherry");
    testcase(connfd, "HDEL H_Grape", "OK\r\n", "HDEL-H_Grape");
    testcase(connfd, "HDEL H_Orange", "OK\r\n", "HDEL-H_Orange");
}

void skiptable_test_sort_range(int connfd) {
    // 准备测试数据
    testcase(connfd, "SSET S_Apple Red", "OK\r\n", "SSET-S_Apple");
    testcase(connfd, "SSET S_Banana Yellow", "OK\r\n", "SSET-S_Banana");
    testcase(connfd, "SSET S_Cherry Red", "OK\r\n", "SSET-S_Cherry");
    testcase(connfd, "SSET S_Grape Purple", "OK\r\n", "SSET-S_Grape");
    testcase(connfd, "SSET S_Orange Orange", "OK\r\n", "SSET-S_Orange");
    
    // 测试排序
    testcase(connfd, "SSORT", "S_Apple:Red\r\nS_Banana:Yellow\r\nS_Cherry:Red\r\nS_Grape:Purple\r\nS_Orange:Orange\r\n", "SSORT-SkipTable");
    
    // 测试范围查询
    testcase(connfd, "SRANGE S_Banana S_Grape", "S_Banana:Yellow\r\nS_Cherry:Red\r\nS_Grape:Purple\r\n", "SRANGE-SkipTable");
    
    // 清理测试数据
    testcase(connfd, "SDEL S_Apple", "OK\r\n", "SDEL-S_Apple");
    testcase(connfd, "SDEL S_Banana", "OK\r\n", "SDEL-S_Banana");
    testcase(connfd, "SDEL S_Cherry", "OK\r\n", "SDEL-S_Cherry");
    testcase(connfd, "SDEL S_Grape", "OK\r\n", "SDEL-S_Grape");
    testcase(connfd, "SDEL S_Orange", "OK\r\n", "SDEL-S_Orange");
}


void array_testcase(int connfd) {

	testcase(connfd, "SET Teacher King", "OK\r\n", "SET-Teacher");
	testcase(connfd, "GET Teacher", "King\r\n", "GET-Teacher");
	testcase(connfd, "MOD Teacher Darren", "OK\r\n", "MOD-Teacher");
	testcase(connfd, "GET Teacher", "Darren\r\n", "GET-Teacher");
	testcase(connfd, "EXIST Teacher", "EXIST\r\n", "GET-Teacher");
	testcase(connfd, "DEL Teacher", "OK\r\n", "DEL-Teacher");
	testcase(connfd, "GET Teacher", "NO EXIST\r\n", "GET-Teacher");
	testcase(connfd, "MOD Teacher KING", "NO EXIST\r\n", "MOD-Teacher");
	testcase(connfd, "EXIST Teacher", "NO EXIST\r\n", "GET-Teacher");
	testcase(connfd, "SET B b", "OK\r\n", "SET-B");
	testcase(connfd, "SET A a", "OK\r\n", "SET-A");
	testcase(connfd, "SET C c", "OK\r\n", "SET-C");
	testcase(connfd, "SET E e", "OK\r\n", "SET-E");
	testcase(connfd, "SET D d", "OK\r\n", "SET-D");
	testcase(connfd, "SORT", "A:a\r\nB:b\r\nC:c\r\nD:d\r\nE:e\r\n", "SORT");
	testcase(connfd, "RANGE A D", "A:a\r\nB:b\r\nC:c\r\nD:d\r\n", "RANGE");
}

void array_testcase_1w(int connfd) {

	int count = 10000;
	int i = 0;

	struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

	for (i = 0;i < count;i ++) {

		testcase(connfd, "SET Teacher King", "OK\r\n", "SET-Teacher");
		testcase(connfd, "GET Teacher", "King\r\n", "GET-Teacher");
		testcase(connfd, "MOD Teacher Darren", "OK\r\n", "MOD-Teacher");
		testcase(connfd, "GET Teacher", "Darren\r\n", "GET-Teacher");
		testcase(connfd, "EXIST Teacher", "EXIST\r\n", "GET-Teacher");
		testcase(connfd, "DEL Teacher", "OK\r\n", "DEL-Teacher");
		testcase(connfd, "GET Teacher", "NO EXIST\r\n", "GET-Teacher");
		testcase(connfd, "MOD Teacher KING", "NO EXIST\r\n", "MOD-Teacher");
		testcase(connfd, "EXIST Teacher", "NO EXIST\r\n", "GET-Teacher");

	}

	struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms

	printf("array testcase --> time_used: %d, qps: %d\n", time_used, 90000 * 1000 / time_used);

}


void rbtree_testcase(int connfd) {

	testcase(connfd, "RSET Teacher King", "OK\r\n", "RSET-Teacher");
	testcase(connfd, "RGET Teacher", "King\r\n", "RGET-King-Teacher");
	testcase(connfd, "RMOD Teacher Darren", "OK\r\n", "RMOD-D-Teacher");
	testcase(connfd, "RGET Teacher", "Darren\r\n", "RGET-Darren-Teacher");
	testcase(connfd, "REXIST Teacher", "EXIST\r\n", "REXIST-Teacher");
	testcase(connfd, "RDEL Teacher", "OK\r\n", "RDEL-Teacher");
	testcase(connfd, "RGET Teacher", "NO EXIST\r\n", "RGET-K-Teacher");
	testcase(connfd, "RMOD Teacher KING", "NO EXIST\r\n", "RMOD-K-Teacher");
	testcase(connfd, "REXIST Teacher", "NO EXIST\r\n", "REXIST-Teacher");

}

void rbtree_testcase_1w(int connfd) {

	int count = 10000;
	int i = 0;

	struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

	for (i = 0;i < count;i ++) {

		testcase(connfd, "RSET Teacher King", "OK\r\n", "RSET-Teacher");
		testcase(connfd, "RGET Teacher", "King\r\n", "RGET-King-Teacher");
		testcase(connfd, "RMOD Teacher Darren", "OK\r\n", "RMOD-D-Teacher");
		testcase(connfd, "RGET Teacher", "Darren\r\n", "RGET-Darren-Teacher");
		testcase(connfd, "REXIST Teacher", "EXIST\r\n", "REXIST-Teacher");
		testcase(connfd, "RDEL Teacher", "OK\r\n", "RDEL-Teacher");
		testcase(connfd, "RGET Teacher", "NO EXIST\r\n", "RGET-K-Teacher");
		testcase(connfd, "RMOD Teacher KING", "NO EXIST\r\n", "RMOD-K-Teacher");
		testcase(connfd, "REXIST Teacher", "NO EXIST\r\n", "REXIST-Teacher");

	}

	struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms

	printf("rbtree testcase --> time_used: %d, qps: %d\n", time_used, 90000 * 1000 / time_used);
	
}


void rbtree_testcase_3w(int connfd) {

	int count = 10000;
	int i = 0;

	struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

	for (i = 0;i < count;i ++) {

		char cmd[128] = {0};
		snprintf(cmd, 128, "RSET Teacher%d King%d", i, i);
		testcase(connfd, cmd, "OK\r\n", "RSET-Teacher");
	}

	for (i = 0;i < count;i ++) {

		char cmd[128] = {0};
		snprintf(cmd, 128, "RGET Teacher%d", i);

		char result[128] = {0};
		snprintf(result, 128, "King%d\r\n", i);
		
		testcase(connfd, cmd, result, "RGET-King-Teacher");
	}

	for (i = 0;i < count;i ++) {

		char cmd[128] = {0};
		snprintf(cmd, 128, "RMOD Teacher%d King%d", i, i);
		testcase(connfd, cmd, "OK\r\n", "RGET-King-Teacher");
	}

	struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms

	printf("rbtree testcase --> time_used: %d, qps: %d\n", time_used, 30000 * 1000 / time_used);

}

void hash_testcase(int connfd) {

	testcase(connfd, "HSET Teacher King", "OK\r\n", "HSET-Teacher");
	testcase(connfd, "HGET Teacher", "King\r\n", "HGET-King-Teacher");
	testcase(connfd, "HMOD Teacher Darren", "OK\r\n", "HMOD-D-Teacher");
	testcase(connfd, "HGET Teacher", "Darren\r\n", "HGET-Darren-Teacher");
	testcase(connfd, "HEXIST Teacher", "EXIST\r\n", "HEXIST-Teacher");
	testcase(connfd, "HDEL Teacher", "OK\r\n", "HDEL-Teacher");
	testcase(connfd, "HGET Teacher", "NO EXIST\r\n", "HGET-K-Teacher");
	testcase(connfd, "HMOD Teacher KING", "NO EXIST\r\n", "HMOD-K-Teacher");
	testcase(connfd, "HEXIST Teacher", "NO EXIST\r\n", "HEXIST-Teacher");

}

void skiptable_testcase(int connfd){

    testcase(connfd, "SSET Teacher King", "OK\r\n", "SSET-Teacher");
	testcase(connfd, "SGET Teacher", "King\r\n", "SGET-Teacher");
	testcase(connfd, "SMOD Teacher Darren", "OK\r\n", "SMOD-Teacher");
	testcase(connfd, "SGET Teacher", "Darren\r\n", "SGET-Teacher");
	testcase(connfd, "SEXIST Teacher", "EXIST\r\n", "SEXIST-Teacher");
	testcase(connfd, "SDEL Teacher", "OK\r\n", "SDEL-Teacher");
	testcase(connfd, "SGET Teacher", "NO EXIST\r\n", "SGET-Teacher");
	testcase(connfd, "SMOD Teacher KING", "NO EXIST\r\n", "SMOD-Teacher");
	testcase(connfd, "SEXIST Teacher", "NO EXIST\r\n", "SEXIST-Teacher");

}
void skiptable_testcase_1w(int connfd){

    int count = 10000;
    int i = 0;

    struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

    
    for(i = 0;i < count;i ++){

        testcase(connfd, "SSET Teacher King", "OK\r\n", "SSET-Teacher");
	    testcase(connfd, "SGET Teacher", "King\r\n", "SGET-Teacher");
	    testcase(connfd, "SMOD Teacher Darren", "OK\r\n", "SMOD-Teacher");
	    testcase(connfd, "SGET Teacher", "Darren\r\n", "SGET-Teacher");
	    testcase(connfd, "SEXIST Teacher", "EXIST\r\n", "SGET-Teacher");
	    testcase(connfd, "SDEL Teacher", "OK\r\n", "SDEL-Teacher");
	    testcase(connfd, "SGET Teacher", "NO EXIST\r\n", "SGET-Teacher");
	    testcase(connfd, "SMOD Teacher KING", "NO EXIST\r\n", "SMOD-Teacher");
	    testcase(connfd, "SEXIST Teacher", "NO EXIST\r\n", "SEXIST-Teacher");

    }

    struct timeval tv_end;
	gettimeofday(&tv_end, NULL);

	int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms

    printf("skiptable testcase --> time_used: %d, qps: %d\n", time_used, 90000 * 1000 / time_used);

}

void pipeline_testcase(int connfd) {
    printf("Testing pipeline functionality...\n");

    // 构建包含多个命令的pipeline请求
    const char *pipeline_cmd = 
        "*3\r\n$4\r\nSSET\r\n$8\r\nPipeKey1\r\n$6\r\nValue1\r\n"
        "*2\r\n$4\r\nSGET\r\n$8\r\nPipeKey1\r\n"
        "*3\r\n$4\r\nSMOD\r\n$8\r\nPipeKey1\r\n$9\r\nNewValue1\r\n"
        "*2\r\n$4\r\nSGET\r\n$8\r\nPipeKey1\r\n"
        "*2\r\n$4\r\nSDEL\r\n$8\r\nPipeKey1\r\n"
        "*2\r\n$4\r\nSGET\r\n$8\r\nPipeKey1\r\n"
        "*3\r\n$4\r\nSSET\r\n$8\r\nPipeKey2\r\n$6\r\nValue2\r\n"
        "*3\r\n$4\r\nSSET\r\n$8\r\nPipeKey3\r\n$6\r\nValue3\r\n"
        "*1\r\n$5\r\nSSORT\r\n";

    // 发送pipeline请求
    send_msg(connfd, (char *)pipeline_cmd, strlen(pipeline_cmd));

    // 接收响应缓冲区
    char response[2048] = {0};
    int total_received = 0;
    int expected_responses = 9; // 我们发送了9个命令

    // 接收所有响应（可能需要多次recv）
    while (total_received < sizeof(response) - 1) {
        int n = recv(connfd, response + total_received, 
                    sizeof(response) - total_received - 1, 0);
        if (n <= 0) break;
        total_received += n;
        
        // 检查是否收到全部响应（按\r\n计数）
        int response_count = 0;
        for (int i = 0; i < total_received; i++) {
            if (response[i] == '\n' && (i > 0 && response[i-1] == '\r')) {
                response_count++;
            }
        }
        if (response_count >= expected_responses) break;
    }
    response[total_received] = '\0';

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
        "*2\r\n$6\r\nValue2\r\n$6\r\nValue3\r\n"; 

    if (strcmp(response, expected_response) == 0) {
        printf("==> PASS -> Pipeline test\n");
    } else {
        printf("==> FAILED -> Pipeline test\n");
        printf("Expected:\n%s\n", expected_response);
        printf("Received:\n%s\n", response);
    }

    // 清理测试数据
    testcase(connfd, "*2\r\n$4\r\nSDEL\r\n$8\r\nPipeKey2\r\n", "+OK\r\n", "Pipeline cleanup");
    testcase(connfd, "*2\r\n$4\r\nSDEL\r\n$8\r\nPipeKey3\r\n", "+OK\r\n", "Pipeline cleanup");
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
	
}

// testcase 192.168.243.131  2000
int main(int argc, char *argv[]) {

	if (argc != 4) {
		printf("arg error\n");
		return -1;
	}

	char *ip = argv[1];
	int port = atoi(argv[2]);
	int mode = atoi(argv[3]);

	int connfd = connect_tcpserver(ip, port);

	if (mode == 0) {//正确
		printf("  0: rbtree_testcase_1w\n");
		rbtree_testcase(connfd);
	} else if (mode == 1) {//正确
		rbtree_testcase_3w(connfd);
	} else if (mode == 2) {//正确
		printf("  2: array_testcase_1w\n");
		array_testcase(connfd);
		//array_testcase(connfd);
	} else if (mode == 3) {//正确
		printf("  3: hash_testcase\n");
		hash_testcase(connfd);
	}else if(mode == 4){//不正确
		printf("  4: skiptable_testcase\n");
        skiptable_testcase(connfd);
    }else if(mode == 5){//正确
		printf("  5: Array Sort & Range Test\n");
		array_test_sort_range(connfd);
	}else if(mode == 6){//正确
		printf("  6: RBTree Sort & Range Test\n");
		rbtree_test_sort_range(connfd);	
	}else if(mode == 7){//正确
		printf("  7: Hash Sort & Range Test\n");
		hash_test_sort_range(connfd);
	}else if(mode == 8){//正确
		printf("  8: SkipTable Sort & Range Test\n");
		skiptable_test_sort_range(connfd);
	}else if(mode == 9){
		pipeline_testcase(connfd);
	}else if(mode == 10){
		array_resp_testcase(connfd);
	}else if(mode == 11){
		rbtree_resp_testcase(connfd);
	}else if(mode == 12){
		hash_resp_testcase(connfd);
	}else if(mode == 13){
		skiptable_resp_testcase(connfd);
	}

	return 0;
	
}



