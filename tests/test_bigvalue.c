#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_BUF (1024 * 1024)

static int send_all(int fd, const char* buf, int len){
	int sent = 0;
	while(sent < len){
		ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
		if(n < 0){
			if(errno == EINTR) continue;
			return -1;
		}
		if(n == 0){
			break;
		}
		sent += (int)n;
	}
	return sent;
}

static int recv_line(int fd, char* buf, int buflen){
	int total = 0;
	while (total + 1 < buflen) {
		char c;
		int n = recv(fd, &c, 1, 0);
		if (n < 0) {
			if (errno == EINTR) continue;
			perror("recv");
			return -1;
		}
		if (n == 0) break;
		buf[total++] = c;
		if (c == '\n') break;
	}
	buf[total] = '\0';
	return total;
}

static int connect_kvstore(const char* ip, int port){
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_port        = htons(port);
	addr.sin_addr.s_addr = inet_addr(ip);

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		perror("connect");
		close(fd);
		return -1;
	}
	return fd;
}

static char* load_file(const char* path, size_t* out_len){
	FILE* fp = fopen(path, "rb");
	if(!fp){
		perror("fopen");
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if(sz <= 0 || sz > MAX_BUF){
		fprintf(stderr, "file size invalid: %ld\n", sz);
		fclose(fp);
		return NULL;
	}

	char* buf = (char*)malloc(sz + 1);
	if(!buf){
		perror("malloc");
		fclose(fp);
		return NULL;
	}
	size_t read_len = fread(buf, 1, sz, fp);
	fclose(fp);
	if(read_len != (size_t)sz){
		fprintf(stderr, "fread size mismatch: %zu != %ld\n", read_len, sz);
		free(buf);
		return NULL;
	}
	buf[sz] = '\0';
	if(out_len) *out_len = sz;
	return buf;
}

static int do_one_ds(int fd, const char* set_cmd, const char* get_cmd,
				 const char* del_cmd,
				 const char* key_desc){
	char *big = NULL;
	size_t big_len = 0;
	big = load_file("paper.txt", &big_len);
	if(!big){
		return -1;
	}

	{
		char* cmd = (char*)malloc(64 + big_len);
		int n = snprintf(cmd, 64, "%s$%zu\r\n", set_cmd, big_len);
		memcpy(cmd + n, big, big_len);
		n += (int)big_len;
		memcpy(cmd + n, "\r\n", 2);
		n += 2;
		if (send_all(fd, cmd, n) != n) {
			fprintf(stderr, "%s SET send failed\n", key_desc);
			free(cmd);
			free(big);
			return -1;
		}
		free(cmd);
		char line[128];
		recv_line(fd, line, sizeof(line));
		printf("%s SET resp: %s", key_desc, line);
		if (strcmp(line, "+OK\r\n") != 0) {
			fprintf(stderr, "%s SET not OK\n", key_desc);
			free(big);
			return -1;
		}
	}

	{
		if (send_all(fd, get_cmd, (int)strlen(get_cmd)) != (int)strlen(get_cmd)) {
			fprintf(stderr, "%s GET send failed\n", key_desc);
			free(big);
			return -1;
		}
		char header[64];
		if (recv_line(fd, header, sizeof(header)) <= 0) {
			fprintf(stderr, "%s GET recv header failed\n", key_desc);
			free(big);
			return -1;
		}
		if (header[0] != '$') {
			fprintf(stderr, "%s GET invalid header: %s\n", key_desc, header);
			free(big);
			return -1;
		}
		long resp_len = atol(header + 1);
		if (resp_len != (long)big_len) {
			fprintf(stderr, "%s GET len mismatch: %ld != %zu\n", key_desc, resp_len, big_len);
			free(big);
			return -1;
		}
		char* resp = (char*)malloc(resp_len + 2);
		int got = 0;
		while (got < resp_len + 2) {
			int m = recv(fd, resp + got, (int)(resp_len + 2 - got), 0);
			if (m <= 0) {
				perror("recv body");
				free(resp);
				free(big);
				return -1;
			}
			got += m;
		}
		if (memcmp(resp, big, big_len) != 0) {
			fprintf(stderr, "%s GET body mismatch\n", key_desc);
			free(resp);
			free(big);
			return -1;
		}
		free(resp);
		printf("%s GET content OK\n", key_desc);
	}

	{
		if (send_all(fd, del_cmd, (int)strlen(del_cmd)) != (int)strlen(del_cmd)) {
			fprintf(stderr, "%s DEL send failed\n", key_desc);
			free(big);
			return -1;
		}
		char line[128];
		recv_line(fd, line, sizeof(line));
		printf("%s DEL resp: %s", key_desc, line);
		if (strcmp(line, "+OK\r\n") != 0) {
			fprintf(stderr, "%s DEL not OK\n", key_desc);
			free(big);
			return -1;
		}
	}

	free(big);
	return 0;
}

int main(int argc, char* argv[]){
	if(argc < 3){
		printf("Usage: %s <kvstore_ip> <kvstore_port>\n", argv[0]);
		return 1;
	}
	const char* ip = argv[1];
	int port = atoi(argv[2]);
	int fd = connect_kvstore(ip, port);
	if(fd < 0) return 1;

	if (do_one_ds(fd,
		"*3\r\n$3\r\nSET\r\n$8\r\nBV_arr01\r\n",
		"*2\r\n$3\r\nGET\r\n$8\r\nBV_arr01\r\n",
		"*2\r\n$3\r\nDEL\r\n$8\r\nBV_arr01\r\n",
		"ARRAY") != 0) {
		close(fd);
		return 1;
	}

	if (do_one_ds(fd,
		"*3\r\n$4\r\nHSET\r\n$8\r\nBV_hash1\r\n",
		"*2\r\n$4\r\nHGET\r\n$8\r\nBV_hash1\r\n",
		"*2\r\n$4\r\nHDEL\r\n$8\r\nBV_hash1\r\n",
		"HASH") != 0) {
		close(fd);
		return 1;
	}

	if (do_one_ds(fd,
		"*3\r\n$4\r\nRSET\r\n$8\r\nBV_tree1\r\n",
		"*2\r\n$4\r\nRGET\r\n$8\r\nBV_tree1\r\n",
		"*2\r\n$4\r\nRDEL\r\n$8\r\nBV_tree1\r\n",
		"RBTREE") != 0) {
		close(fd);
		return 1;
	}

	if (do_one_ds(fd,
		"*3\r\n$4\r\nSSET\r\n$8\r\nBV_skip1\r\n",
		"*2\r\n$4\r\nSGET\r\n$8\r\nBV_skip1\r\n",
		"*2\r\n$4\r\nSDEL\r\n$8\r\nBV_skip1\r\n",
		"SKIPTABLE") != 0) {
		close(fd);
		return 1;
	}

	printf("All 4 data-structures big-value test PASSED\n");
	close(fd);
	return 0;
}
