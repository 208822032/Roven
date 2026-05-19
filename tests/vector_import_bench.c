#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/tcp.h>

#define BENCH_BUF_SIZE 8192
#define BENCH_LINE_SIZE 1024
#define BENCH_KEY_SIZE 256
#define BENCH_VALUE_SIZE 512
#define BENCH_RAW_PREVIEW_SIZE 4096

typedef struct resp_raw_s {
	char data[BENCH_RAW_PREVIEW_SIZE];
	size_t len;
} resp_raw_t;

static long long now_ms(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void raw_append(resp_raw_t *raw, const char *data, size_t len) {
	size_t remain;
	size_t copy_len;

	if (!raw || !data || len == 0) {
		return;
	}

	if (raw->len >= sizeof(raw->data) - 1) {
		return;
	}

	remain = sizeof(raw->data) - 1 - raw->len;
	copy_len = len < remain ? len : remain;

	memcpy(raw->data + raw->len, data, copy_len);
	raw->len += copy_len;
	raw->data[raw->len] = '\0';
}

static int connect_tcp(const char *host, const char *port) {
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	struct addrinfo *rp = NULL;
	int fd = -1;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	ret = getaddrinfo(host, port, &hints, &result);
	if (ret != 0) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(ret));
		return -1;
	}

	for (rp = result; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0) {
			continue;
		}

		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
			int flag = 1;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));
			break;
		}

		close(fd);
		fd = -1;
	}

	freeaddrinfo(result);

	return fd;
}

static int send_all(int fd, const char *buf, size_t len) {
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = send(fd, buf + sent, len - sent, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}

		if (n == 0) {
			return -1;
		}

		sent += (size_t)n;
	}

	return 0;
}

static int read_exact(int fd, char *buf, size_t len, resp_raw_t *raw) {
	size_t got = 0;

	while (got < len) {
		ssize_t n = recv(fd, buf + got, len - got, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}

		if (n == 0) {
			return -1;
		}

		if (raw) {
			raw_append(raw, buf + got, (size_t)n);
		}

		got += (size_t)n;
	}

	return 0;
}

static int read_line(int fd, char *line, size_t cap, resp_raw_t *raw) {
	size_t len = 0;
	char ch;

	if (!line || cap == 0) {
		return -1;
	}

	while (len + 1 < cap) {
		ssize_t n = recv(fd, &ch, 1, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}

		if (n == 0) {
			return -1;
		}

		if (raw) {
			raw_append(raw, &ch, 1);
		}

		line[len++] = ch;

		if (len >= 2 && line[len - 2] == '\r' && line[len - 1] == '\n') {
			line[len - 2] = '\0';
			return 0;
		}
	}

	return -1;
}

static int read_resp_value(int fd, resp_raw_t *raw);

static int read_resp_bulk(int fd, const char *len_line, resp_raw_t *raw) {
	long bulk_len;
	char *tmp;

	bulk_len = strtol(len_line, NULL, 10);
	if (bulk_len < 0) {
		return 0;
	}

	tmp = (char *)malloc((size_t)bulk_len + 2);
	if (!tmp) {
		return -1;
	}

	if (read_exact(fd, tmp, (size_t)bulk_len + 2, raw) != 0) {
		free(tmp);
		return -1;
	}

	free(tmp);
	return 0;
}

static int read_resp_array(int fd, const char *count_line, resp_raw_t *raw) {
	long count;
	long i;

	count = strtol(count_line, NULL, 10);
	if (count < 0) {
		return 0;
	}

	for (i = 0; i < count; ++i) {
		if (read_resp_value(fd, raw) != 0) {
			return -1;
		}
	}

	return 0;
}

static int read_resp_value(int fd, resp_raw_t *raw) {
	char prefix;
	char line[BENCH_LINE_SIZE];
	ssize_t n;

	n = recv(fd, &prefix, 1, 0);
	if (n < 0) {
		if (errno == EINTR) {
			return read_resp_value(fd, raw);
		}
		return -1;
	}

	if (n == 0) {
		return -1;
	}

	if (raw) {
		raw_append(raw, &prefix, 1);
	}

	if (read_line(fd, line, sizeof(line), raw) != 0) {
		return -1;
	}

	switch (prefix) {
		case '+':
		case '-':
		case ':':
			return 0;

		case '$':
			return read_resp_bulk(fd, line, raw);

		case '*':
			return read_resp_array(fd, line, raw);

		default:
			return -1;
	}
}

static int read_resp(int fd, resp_raw_t *raw) {
	if (!raw) {
		return -1;
	}

	memset(raw, 0, sizeof(*raw));
	return read_resp_value(fd, raw);
}

static int resp_is_ok(const resp_raw_t *raw) {
	if (!raw || raw->len < 3) {
		return 0;
	}

	return strncmp(raw->data, "+OK", 3) == 0;
}

static int resp_is_error(const resp_raw_t *raw) {
	if (!raw || raw->len == 0) {
		return 0;
	}

	return raw->data[0] == '-';
}

static int send_resp_command(int fd, const char **parts, const size_t *lens, size_t count) {
	char stack_buf[8192];
	char *buf = stack_buf;
	size_t cap = sizeof(stack_buf);
	size_t pos = 0;
	size_t i;
	int n;

	if (!parts || !lens || count == 0) {
		return -1;
	}

	/*
	 * 先估算 RESP 命令总长度，然后一次性 send。
	 *
	 * 原来的实现会把一条命令拆成很多次 send：
	 *   *N\r\n
	 *   $len\r\n
	 *   data
	 *   \r\n
	 *
	 * VGET/VSEARCH 这种小请求会触发 TCP 小包 / delayed ACK，
	 * 实测出现约 40ms/请求，导致 qps 只有 20 多。
	 *
	 * 这里把整条 RESP 命令拼成一个连续 buffer，一次 send_all。
	 */
	cap = 64;
	for (i = 0; i < count; ++i) {
		cap += 64 + lens[i] + 2;
	}

	if (cap > sizeof(stack_buf)) {
		buf = (char *)malloc(cap);
		if (!buf) {
			return -1;
		}
	}

	n = snprintf(buf + pos, cap - pos, "*%zu\r\n", count);
	if (n <= 0 || (size_t)n >= cap - pos) {
		if (buf != stack_buf) {
			free(buf);
		}
		return -1;
	}
	pos += (size_t)n;

	for (i = 0; i < count; ++i) {
		n = snprintf(buf + pos, cap - pos, "$%zu\r\n", lens[i]);
		if (n <= 0 || (size_t)n >= cap - pos) {
			if (buf != stack_buf) {
				free(buf);
			}
			return -1;
		}
		pos += (size_t)n;

		if (lens[i] > 0) {
			if (!parts[i]) {
				if (buf != stack_buf) {
					free(buf);
				}
				return -1;
			}

			if (pos + lens[i] + 2 > cap) {
				if (buf != stack_buf) {
					free(buf);
				}
				return -1;
			}

			memcpy(buf + pos, parts[i], lens[i]);
			pos += lens[i];
		}

		buf[pos++] = '\r';
		buf[pos++] = '\n';
	}

	if (send_all(fd, buf, pos) != 0) {
		if (buf != stack_buf) {
			free(buf);
		}
		return -1;
	}

	if (buf != stack_buf) {
		free(buf);
	}

	return 0;
}

static int send_vset(
	int fd,
	const char *question,
	const char *answer,
	const char *scope,
	resp_raw_t *raw
) {
	const char *parts[4];
	size_t lens[4];
	size_t count = 0;

	parts[count] = "VSET";
	lens[count] = 4;
	count++;

	parts[count] = question;
	lens[count] = strlen(question);
	count++;

	parts[count] = answer;
	lens[count] = strlen(answer);
	count++;

	if (scope && scope[0] != '\0') {
		parts[count] = scope;
		lens[count] = strlen(scope);
		count++;
	}

	if (send_resp_command(fd, parts, lens, count) != 0) {
		return -1;
	}

	return read_resp(fd, raw);
}

static int send_vget(
	int fd,
	const char *question,
	const char *scope,
	resp_raw_t *raw
) {
	const char *parts[3];
	size_t lens[3];
	size_t count = 0;

	parts[count] = "VGET";
	lens[count] = 4;
	count++;

	parts[count] = question;
	lens[count] = strlen(question);
	count++;

	if (scope && scope[0] != '\0') {
		parts[count] = scope;
		lens[count] = strlen(scope);
		count++;
	}

	if (send_resp_command(fd, parts, lens, count) != 0) {
		return -1;
	}

	return read_resp(fd, raw);
}

static int send_vsearch(
	int fd,
	const char *query,
	const char *scope,
	int topk,
	resp_raw_t *raw
) {
	const char *parts[4];
	size_t lens[4];
	char topk_buf[32];
	size_t count = 0;

	snprintf(topk_buf, sizeof(topk_buf), "%d", topk);

	parts[count] = "VSEARCH";
	lens[count] = 7;
	count++;

	parts[count] = query;
	lens[count] = strlen(query);
	count++;

	if (scope && scope[0] != '\0') {
		parts[count] = scope;
		lens[count] = strlen(scope);
		count++;

		parts[count] = topk_buf;
		lens[count] = strlen(topk_buf);
		count++;
	} else {
		parts[count] = topk_buf;
		lens[count] = strlen(topk_buf);
		count++;
	}

	if (send_resp_command(fd, parts, lens, count) != 0) {
		return -1;
	}

	return read_resp(fd, raw);
}

static int send_vinfo(int fd, resp_raw_t *raw) {
	const char *parts[1];
	size_t lens[1];

	parts[0] = "VINFO";
	lens[0] = 5;

	if (send_resp_command(fd, parts, lens, 1) != 0) {
		return -1;
	}

	return read_resp(fd, raw);
}

static void make_question(char *buf, size_t cap, const char *prefix, size_t i) {
	snprintf(buf, cap, "%s_question_%08zu", prefix, i);
}

static void make_answer(char *buf, size_t cap, const char *prefix, size_t i) {
	snprintf(buf, cap, "%s_answer_%08zu", prefix, i);
}

static void print_usage(const char *prog) {
	printf("Usage:\n");
	printf("  %s <host> <port> <count> [scope] [prefix] [batch_size] [get_count] [search_count] [topk] [search_batch_size]\n", prog);printf("\n");
	printf("Examples:\n");
	printf("  %s 192.168.36.141 9300 1000\n", prog);
	printf("  %s 192.168.36.141 9300 10000 bench_scope bench 1\n", prog);
	printf("  %s 192.168.36.141 9300 10000 bench_scope bench 64\n", prog);
	printf("  %s 192.168.36.141 9300 100000 bench_scope formal 128 100000 100000 10\n", prog);
}

int main(int argc, char **argv) {
	const char *host;
	const char *port;
	const char *scope = "";
	const char *prefix = "bench";
	size_t count;
	size_t i;
	size_t success = 0;
	size_t failed = 0;
	size_t request_count = 0;
	size_t vset_request_count = 0;
	size_t progress_step;
	size_t batch_size = 1;
	size_t get_count = 0;
	size_t search_count = 0;
	int topk = 10;

	size_t search_batch_size = 1;
	size_t search_request_count = 0;

	size_t get_success = 0;
	size_t get_failed = 0;
	size_t search_success = 0;
	size_t search_failed = 0;

	long long get_start_ms;
	long long get_end_ms;
	long long search_start_ms;
	long long search_end_ms;
	int fd;
	long long start_ms;
	long long end_ms;
	double elapsed_sec;
	double qps;
	double avg_records_per_request;
	resp_raw_t raw;
	char question[BENCH_KEY_SIZE];
	char answer[BENCH_VALUE_SIZE];

	if (argc < 4) {
		print_usage(argv[0]);
		return 1;
	}

	host = argv[1];
	port = argv[2];
	count = (size_t)strtoull(argv[3], NULL, 10);

	if (argc >= 5) {
		scope = argv[4];
	}

	if (argc >= 6) {
		prefix = argv[5];
	}

	if (argc >= 7) {
		batch_size = (size_t)strtoull(argv[6], NULL, 10);
		if (batch_size == 0) {
			batch_size = 1;
		}
	}

	if (argc >= 8) {
		get_count = (size_t)strtoull(argv[7], NULL, 10);
	}

	if (argc >= 9) {
		search_count = (size_t)strtoull(argv[8], NULL, 10);
	}

	if (argc >= 10) {
		topk = atoi(argv[9]);
		if (topk <= 0) {
			topk = 10;
		}
	}

	if (argc >= 11) {
		search_batch_size = (size_t)strtoull(argv[10], NULL, 10);
		if (search_batch_size == 0) {
			search_batch_size = 1;
		}
	}

	if (count == 0) {
		fprintf(stderr, "count must be greater than 0\n");
		return 1;
	}

	fd = connect_tcp(host, port);
	if (fd < 0) {
		fprintf(stderr, "failed to connect %s:%s\n", host, port);
		return 1;
	}

	printf("connected to kvstore %s:%s\n", host, port);
	printf("========== vector import bench begin ==========\n");
	printf("count=%zu scope=%s prefix=%s batch_size=%zu\n",
	       count,
	       (scope && scope[0] != '\0') ? scope : "<empty>",
	       prefix,
	       batch_size);

	progress_step = count / 10;
	if (progress_step == 0) {
		progress_step = 1;
	}

	start_ms = now_ms();

	i = 0;
	while (i < count) {
		size_t current_batch;
		size_t j;

		if (batch_size <= 1) {
			make_question(question, sizeof(question), prefix, i);
			make_answer(answer, sizeof(answer), prefix, i);

			request_count++;
			vset_request_count++;

			if (send_vset(fd, question, answer, scope, &raw) != 0) {
				failed++;
				fprintf(stderr, "VSET failed at %zu: socket/read error\n", i);
				break;
			}

			if (resp_is_ok(&raw)) {
				success++;
			} else {
				failed++;
				fprintf(stderr, "VSET failed at %zu response:\n%s\n", i, raw.data);
				if (resp_is_error(&raw)) {
					break;
				}
			}

			i++;
		} else {
			char (*batch_questions)[BENCH_KEY_SIZE];
			char (*batch_answers)[BENCH_VALUE_SIZE];

			current_batch = batch_size;
			if (current_batch > count - i) {
				current_batch = count - i;
			}

			batch_questions = malloc(sizeof(char[BENCH_KEY_SIZE]) * current_batch);
			batch_answers = malloc(sizeof(char[BENCH_VALUE_SIZE]) * current_batch);

			if (!batch_questions || !batch_answers) {
				fprintf(stderr, "malloc batch buffer failed\n");
				if (batch_questions) {
					free(batch_questions);
				}
				if (batch_answers) {
					free(batch_answers);
				}
				break;
			}

			for (j = 0; j < current_batch; ++j) {
				make_question(batch_questions[j], BENCH_KEY_SIZE, prefix, i + j);
				make_answer(batch_answers[j], BENCH_VALUE_SIZE, prefix, i + j);
			}

			request_count++;


			if (resp_is_ok(&raw)) {
				success += current_batch;
			} else {
				failed += current_batch;
				fprintf(stderr, "VBATCHSET failed at %zu response:\n%s\n", i, raw.data);
				free(batch_questions);
				free(batch_answers);
				if (resp_is_error(&raw)) {
					break;
				}
			}

			free(batch_questions);
			free(batch_answers);

			i += current_batch;
		}

		if ((success + failed) % progress_step == 0 || success + failed >= count) {
			printf("progress %zu/%zu success=%zu failed=%zu requests=%zu\n",
			       success + failed,
			       count,
			       success,
			       failed,
			       request_count);
		}
	}

	end_ms = now_ms();

	elapsed_sec = (double)(end_ms - start_ms) / 1000.0;
	if (elapsed_sec <= 0.0) {
		elapsed_sec = 0.001;
	}

	qps = (double)success / elapsed_sec;
	avg_records_per_request =
		request_count > 0 ? (double)success / (double)request_count : 0.0;

	printf("========== import result ==========\n");
	printf("success=%zu\n", success);
	printf("failed=%zu\n", failed);
	printf("elapsed_ms=%lld\n", (long long)(end_ms - start_ms));
	printf("write_qps=%.2f\n", qps);
	printf("batch_size=%zu\n", batch_size);
	printf("request_count=%zu\n", request_count);
	printf("vset_request_count=%zu\n", vset_request_count);
	printf("avg_records_per_request=%.2f\n", avg_records_per_request);

	if (success > 0) {
		size_t sample_indices[3];
		size_t sample_count = 0;

		sample_indices[sample_count++] = 0;

		if (success > 2) {
			sample_indices[sample_count++] = success / 2;
		}

		if (success > 1) {
			sample_indices[sample_count++] = success - 1;
		}

		printf("========== sample VGET ==========\n");

		for (i = 0; i < sample_count; ++i) {
			size_t idx = sample_indices[i];

			make_question(question, sizeof(question), prefix, idx);

			if (send_vget(fd, question, scope, &raw) != 0) {
				printf("sample VGET index=%zu failed: socket/read error\n", idx);
				continue;
			}

			printf("sample index=%zu question=%s response:\n%s\n",
			       idx,
			       question,
			       raw.data);
		}
		
	}

	if (get_count > 0 && success > 0) {
		printf("========== VGET bench begin ==========\n");

		get_start_ms = now_ms();

		for (i = 0; i < get_count; ++i) {
			size_t idx = i % success;

			make_question(question, sizeof(question), prefix, idx);

			if (send_vget(fd, question, scope, &raw) != 0) {
				get_failed++;
				continue;
			}

			/*
			 * VGET 命中时应该返回 bulk string:
			 *   $len\r\nanswer\r\n
			 */
			if (raw.data[0] == '$') {
				get_success++;
			} else {
				get_failed++;

				if (get_failed <= 3) {
					fprintf(stderr,
					        "VGET unexpected response at i=%zu question=%s response:\n%s\n",
					        i,
					        question,
					        raw.data);
				}
			}
		}

		get_end_ms = now_ms();

		{
			double sec = (double)(get_end_ms - get_start_ms) / 1000.0;
			if (sec <= 0.0) {
				sec = 0.001;
			}

			printf("========== VGET bench result ==========\n");
			printf("get_count=%zu\n", get_count);
			printf("get_success=%zu\n", get_success);
			printf("get_failed=%zu\n", get_failed);
			printf("get_elapsed_ms=%lld\n", (long long)(get_end_ms - get_start_ms));
			printf("get_qps=%.2f\n", (double)get_success / sec);
		}
	}

	if (search_count > 0 && success > 0) {
		printf("========== VSEARCH bench begin ==========\n");
		printf("search_batch_size=%zu\n", search_batch_size);

		search_start_ms = now_ms();

		i = 0;
		while (i < search_count) {
			if (search_batch_size <= 1) {
				size_t idx = i % success;

				make_question(question, sizeof(question), prefix, idx);

				search_request_count++;
				if (send_vsearch(fd, question, scope, topk, &raw) != 0) {
					search_failed++;
					i++;
					continue;
				}

				/*
				* VSEARCH 正常返回 RESP array:
				*   *N\r\n...
				*/
				if (raw.data[0] == '*') {
					search_success++;
				} else {
					search_failed++;

					if (search_failed <= 3) {
						fprintf(stderr,
								"VSEARCH unexpected response at i=%zu query=%s response:\n%s\n",
								i,
								question,
								raw.data);
					}
				}

				i++;
			} else {
				char (*batch_queries)[BENCH_KEY_SIZE];
				size_t current_batch = search_batch_size;
				size_t j;

				if (current_batch > search_count - i) {
					current_batch = search_count - i;
				}

				batch_queries = malloc(sizeof(char[BENCH_KEY_SIZE]) * current_batch);
				if (!batch_queries) {
					fprintf(stderr, "malloc batch search buffer failed\n");
					search_failed += current_batch;
					break;
				}

				for (j = 0; j < current_batch; ++j) {
					size_t idx = (i + j) % success;
					make_question(batch_queries[j], BENCH_KEY_SIZE, prefix, idx);
				}

				search_request_count++;

				/*
				* 批量 VSEARCH 正常返回二维 RESP array:
				*   *batch_count\r\n...
				*/
				if (raw.data[0] == '*') {
					search_success += current_batch;
				} else {
					search_failed += current_batch;

					if (search_failed <= 3) {
						fprintf(stderr,
								"VSEARCH batch unexpected response at i=%zu batch=%zu response:\n%s\n",
								i,
								current_batch,
								raw.data);
					}
				}

				free(batch_queries);
				i += current_batch;
			}
		}

		search_end_ms = now_ms();

		{
			double sec = (double)(search_end_ms - search_start_ms) / 1000.0;
			if (sec <= 0.0) {
				sec = 0.001;
			}

			printf("========== VSEARCH bench result ==========\n");
			printf("search_count=%zu\n", search_count);
			printf("search_success=%zu\n", search_success);
			printf("search_failed=%zu\n", search_failed);
			printf("search_elapsed_ms=%lld\n", (long long)(search_end_ms - search_start_ms));
			printf("search_qps=%.2f\n", (double)search_success / sec);
			printf("search_request_qps=%.2f\n", (double)search_request_count / sec);
			printf("search_request_count=%zu\n", search_request_count);
			printf("search_batch_size=%zu\n", search_batch_size);
			printf("topk=%d\n", topk);
		}
	}

	printf("========== VINFO ==========\n");
	if (send_vinfo(fd, &raw) == 0) {
		printf("%s\n", raw.data);
	} else {
		printf("VINFO failed\n");
	}

	printf("========== vector import bench end ==========\n");

	close(fd);

	return (failed == 0 && get_failed == 0 && search_failed == 0) ? 0 : 1;
}