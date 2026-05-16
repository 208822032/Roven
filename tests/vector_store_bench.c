#define KVS_VECTOR_INTERNAL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "kvs_config.h"
#include "kvs_log.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"

#define STORE_BENCH_KEY_SIZE 256
#define STORE_BENCH_VALUE_SIZE 512
#define STORE_BENCH_DEFAULT_DIM 384
#define STORE_BENCH_DEFAULT_BATCH 1024

extern kvs_config_t g_cfg;

static long long now_ms(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000LL + (long long)tv.tv_usec / 1000LL;
}

static void make_question(char *buf, size_t cap, const char *prefix, size_t i) {
	snprintf(buf, cap, "%s_question_%08zu", prefix, i);
}

static void make_answer(char *buf, size_t cap, const char *prefix, size_t i) {
	snprintf(buf, cap, "%s_answer_%08zu", prefix, i);
}

static void fill_mock_embedding(float *embedding, int dim, size_t row_index) {
	int i;
	float norm = 0.0f;

	if (!embedding || dim <= 0) {
		return;
	}

	for (i = 0; i < dim; ++i) {
		/*
		 * 确定性 mock 向量：
		 * 1. 不调用随机数；
		 * 2. 不调用 sqrt；
		 * 3. 模拟 384 维 float 写入和 memcpy 成本。
		 */
		unsigned int v = (unsigned int)((row_index + 1) * 1315423911u + (unsigned int)i * 2654435761u);
		float x = (float)((int)(v % 2001) - 1000) / 1000.0f;
		embedding[i] = x;

		if (x < 0.0f) {
			norm -= x;
		} else {
			norm += x;
		}
	}

	if (norm <= 0.0f) {
		embedding[0] = 1.0f;
		return;
	}

	for (i = 0; i < dim; ++i) {
		embedding[i] /= norm;
	}
}

static void print_usage(const char *prog) {
	printf("Usage:\n");
	printf("  %s <count> [batch_size] [dimension] [scope] [prefix]\n", prog);
	printf("\n");
	printf("Examples:\n");
	printf("  %s 10000\n", prog);
	printf("  %s 100000 1024 384 bench_scope storebench\n", prog);
}

int main(int argc, char **argv) {
	size_t count;
	size_t batch_size = STORE_BENCH_DEFAULT_BATCH;
	int dimension = STORE_BENCH_DEFAULT_DIM;
	const char *scope = "store_scope";
	const char *prefix = "storebench";

	size_t done = 0;
	size_t total_success = 0;
	size_t total_failed = 0;
	size_t request_count = 0;
	size_t progress_step;

	long long start_ms;
	long long end_ms;
	double elapsed_sec;
	double write_qps;
	double avg_records_per_batch;

	kvs_vector_info_t info;
	kvs_vector_search_result_t result;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	count = (size_t)strtoull(argv[1], NULL, 10);

	if (argc >= 3) {
		batch_size = (size_t)strtoull(argv[2], NULL, 10);
		if (batch_size == 0) {
			batch_size = STORE_BENCH_DEFAULT_BATCH;
		}
	}

	if (argc >= 4) {
		dimension = atoi(argv[3]);
		if (dimension <= 0) {
			dimension = STORE_BENCH_DEFAULT_DIM;
		}
	}

	if (argc >= 5) {
		scope = argv[4];
	}

	if (argc >= 6) {
		prefix = argv[5];
	}

	if (count == 0) {
		fprintf(stderr, "count must be greater than 0\n");
		return 1;
	}

	/*
	 * 初始化最小运行环境。
	 *
	 * 这里强制使用 system malloc 和 hash provider。
	 * 但本压测调用的是 kvs_vector_set_batch_with_embeddings_internal，
	 * 不会真正生成 hash embedding。
	 */
	kvs_config_init_default(&g_cfg);
	g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
	snprintf(g_cfg.embedding_provider, sizeof(g_cfg.embedding_provider), "%s", "hash");
	snprintf(g_cfg.log_level, sizeof(g_cfg.log_level), "%s", "error");
	g_cfg.log_async = 0;
	g_cfg.log_file[0] = '\0';

	kvs_log_init(NULL, KVS_LOG_LEVEL_ERROR, 0);

	mem_set_backend(KVS_MEM_BACKEND_SYSTEM);
	mem_init();

	if (kvs_vector_init() != KVS_VECTOR_OK) {
        fprintf(stderr, "kvs_vector_init failed\n");
        mem_destroy();
        kvs_log_shutdown();
        return 1;
    }

    /*
    * 纯存储压测使用调用方指定的 mock embedding 维度。
    *
    * kvs_vector_init() 在 hash provider 下会把 flat_index 初始化成
    * KVS_VECTOR_DEFAULT_DIMENSION，也就是 64 维。
    *
    * 但本压测需要模拟特定维度的写入测试，
    * 所以这里在还没有写入任何 record 前，重新初始化 flat_index 维度。
    */
    if (g_vector_state.flat_index.dimension != (size_t)dimension) {
        kvs_vector_flat_index_destroy(&g_vector_state.flat_index);

        if (kvs_vector_flat_index_init(
                &g_vector_state.flat_index,
                (size_t)dimension) != KVS_VECTOR_OK) {
            fprintf(stderr,
                    "flat index init failed dimension=%d\n",
                    dimension);
            kvs_vector_destroy();
            mem_destroy();
            kvs_log_shutdown();
            return 1;
        }
    }

    printf("========== vector store bench begin ==========\n");
	printf("count=%zu batch_size=%zu dimension=%d scope=%s prefix=%s\n",
	       count,
	       batch_size,
	       dimension,
	       scope && scope[0] ? scope : "<empty>",
	       prefix);

	progress_step = count / 10;
	if (progress_step == 0) {
		progress_step = 1;
	}

	start_ms = now_ms();

	while (done < count) {
		size_t current_batch = batch_size;
		size_t i;
		size_t success_count = 0;
		size_t failed_count = 0;
		int ret;

		char (*questions)[STORE_BENCH_KEY_SIZE] = NULL;
		char (*answers)[STORE_BENCH_VALUE_SIZE] = NULL;
		const char **question_ptrs = NULL;
		const char **answer_ptrs = NULL;
		const char **scope_ptrs = NULL;
		size_t *question_lens = NULL;
		size_t *answer_lens = NULL;
		size_t *scope_lens = NULL;
		float *embeddings = NULL;

		if (current_batch > count - done) {
			current_batch = count - done;
		}

		questions = (char (*)[STORE_BENCH_KEY_SIZE])malloc(
			sizeof(char[STORE_BENCH_KEY_SIZE]) * current_batch
		);
		answers = (char (*)[STORE_BENCH_VALUE_SIZE])malloc(
			sizeof(char[STORE_BENCH_VALUE_SIZE]) * current_batch
		);
		question_ptrs = (const char **)malloc(sizeof(char *) * current_batch);
		answer_ptrs = (const char **)malloc(sizeof(char *) * current_batch);
		scope_ptrs = (const char **)malloc(sizeof(char *) * current_batch);
		question_lens = (size_t *)malloc(sizeof(size_t) * current_batch);
		answer_lens = (size_t *)malloc(sizeof(size_t) * current_batch);
		scope_lens = (size_t *)malloc(sizeof(size_t) * current_batch);
		embeddings = (float *)malloc(
			current_batch * (size_t)dimension * sizeof(float)
		);

		if (!questions || !answers ||
		    !question_ptrs || !answer_ptrs || !scope_ptrs ||
		    !question_lens || !answer_lens || !scope_lens ||
		    !embeddings) {
			fprintf(stderr, "malloc batch buffers failed\n");

			free(questions);
			free(answers);
			free(question_ptrs);
			free(answer_ptrs);
			free(scope_ptrs);
			free(question_lens);
			free(answer_lens);
			free(scope_lens);
			free(embeddings);

			total_failed += current_batch;
			break;
		}

		for (i = 0; i < current_batch; ++i) {
			size_t global_index = done + i;

			make_question(questions[i], STORE_BENCH_KEY_SIZE, prefix, global_index);
			make_answer(answers[i], STORE_BENCH_VALUE_SIZE, prefix, global_index);

			question_ptrs[i] = questions[i];
			answer_ptrs[i] = answers[i];
			scope_ptrs[i] = scope ? scope : "";

			question_lens[i] = strlen(questions[i]);
			answer_lens[i] = strlen(answers[i]);
			scope_lens[i] = strlen(scope_ptrs[i]);

			fill_mock_embedding(
				embeddings + (i * (size_t)dimension),
				dimension,
				global_index
			);
		}

		request_count++;

		ret = kvs_vector_set_batch_with_embeddings_internal(
			question_ptrs,
			question_lens,
			answer_ptrs,
			answer_lens,
			scope_ptrs,
			scope_lens,
			embeddings,
			dimension,
			current_batch,
			&success_count,
			&failed_count
		);

		total_success += success_count;
		total_failed += failed_count;

		free(questions);
		free(answers);
		free(question_ptrs);
		free(answer_ptrs);
		free(scope_ptrs);
		free(question_lens);
		free(answer_lens);
		free(scope_lens);
		free(embeddings);

		done += current_batch;

		if (ret != KVS_VECTOR_OK) {
			fprintf(stderr,
			        "batch insert failed at done=%zu ret=%d success=%zu failed=%zu\n",
			        done,
			        ret,
			        success_count,
			        failed_count);
			break;
		}

		if (done % progress_step == 0 || done >= count) {
			printf("progress %zu/%zu success=%zu failed=%zu batches=%zu\n",
			       done,
			       count,
			       total_success,
			       total_failed,
			       request_count);
		}
	}

	end_ms = now_ms();

	elapsed_sec = (double)(end_ms - start_ms) / 1000.0;
	if (elapsed_sec <= 0.0) {
		elapsed_sec = 0.001;
	}

	write_qps = (double)total_success / elapsed_sec;
	avg_records_per_batch =
		request_count ? (double)total_success / (double)request_count : 0.0;

	printf("========== store bench result ==========\n");
	printf("success=%zu\n", total_success);
	printf("failed=%zu\n", total_failed);
	printf("elapsed_ms=%lld\n", (long long)(end_ms - start_ms));
	printf("store_write_qps=%.2f\n", write_qps);
	printf("batch_size=%zu\n", batch_size);
	printf("request_count=%zu\n", request_count);
	printf("avg_records_per_batch=%.2f\n", avg_records_per_batch);
	printf("dimension=%d\n", dimension);

	memset(&info, 0, sizeof(info));
	if (kvs_vector_info(&info) == KVS_VECTOR_OK) {
		printf("========== VINFO-like ==========\n");
		printf("total_records=%zu\n", info.total_records);
		printf("active_records=%zu\n", info.active_records);
		printf("deleted_records=%zu\n", info.deleted_records);
		printf("capacity=%zu\n", info.capacity);
		printf("index_size=%zu\n", info.index_size);
		printf("dimension=%zu\n", info.dimension);
		printf("index_ready=%d\n", info.index_ready);
		printf("needs_rebuild=%d\n", info.needs_rebuild);
	}

	if (total_success > 0) {
		char sample_question[STORE_BENCH_KEY_SIZE];
		size_t sample_index = total_success / 2;

		make_question(sample_question, sizeof(sample_question), prefix, sample_index);
		kvs_vector_search_result_reset(&result);

		if (kvs_vector_get(
				sample_question,
				strlen(sample_question),
				scope,
				scope ? strlen(scope) : 0,
				&result) == KVS_VECTOR_OK) {
			printf("========== sample VGET-like ==========\n");
			printf("sample_index=%zu\n", sample_index);
			printf("question=%.*s\n", (int)result.question_len, result.question);
			printf("answer=%.*s\n", (int)result.answer_len, result.answer);
			printf("scope=%.*s\n", (int)result.scope_len, result.scope ? result.scope : "");
		} else {
			printf("sample VGET-like failed index=%zu question=%s\n",
			       sample_index,
			       sample_question);
		}
	}

	printf("========== vector store bench end ==========\n");

	kvs_vector_destroy();
	mem_destroy();
	kvs_log_shutdown();

	return total_failed == 0 ? 0 : 1;
}