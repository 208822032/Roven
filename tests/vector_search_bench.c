#define KVS_VECTOR_INTERNAL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <strings.h>

#include "kvs_config.h"
#include "kvs_log.h"
#include "kvs_memory.h"
#include "kvs_memory_internal.h"
#include "kvs_vector.h"


#define SEARCH_BENCH_KEY_SIZE 256
#define SEARCH_BENCH_VALUE_SIZE 512
#define SEARCH_BENCH_DEFAULT_DIM 384
#define SEARCH_BENCH_DEFAULT_BATCH 1024
#define SEARCH_BENCH_MAX_TOPK 128

extern kvs_config_t g_cfg;

static long long now_us(void) {
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000000LL + (long long)tv.tv_usec;
}

static long long now_ms(void) {
	return now_us() / 1000LL;
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
		unsigned int v = (unsigned int)(
			(row_index + 1) * 1315423911u +
			(unsigned int)i * 2654435761u
		);
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

static int reset_flat_index_dimension(int dimension) {
	if (dimension <= 0) {
		return -1;
	}

	if (g_vector_state.flat_index.dimension == (size_t)dimension) {
		return 0;
	}

	kvs_vector_flat_index_destroy(&g_vector_state.flat_index);

	if (kvs_vector_flat_index_init(
			&g_vector_state.flat_index,
			(size_t)dimension) != KVS_VECTOR_OK) {
		return -1;
	}

	return 0;
}

static int load_mock_records(
	size_t count,
	size_t batch_size,
	int dimension,
	const char *scope,
	const char *prefix
) {
	size_t done = 0;

	while (done < count) {
		size_t current_batch = batch_size;
		size_t i;
		size_t success_count = 0;
		size_t failed_count = 0;
		int ret;

		char (*questions)[SEARCH_BENCH_KEY_SIZE] = NULL;
		char (*answers)[SEARCH_BENCH_VALUE_SIZE] = NULL;
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

		questions = (char (*)[SEARCH_BENCH_KEY_SIZE])malloc(
			sizeof(char[SEARCH_BENCH_KEY_SIZE]) * current_batch
		);
		answers = (char (*)[SEARCH_BENCH_VALUE_SIZE])malloc(
			sizeof(char[SEARCH_BENCH_VALUE_SIZE]) * current_batch
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
			fprintf(stderr, "malloc load batch buffers failed\n");

			free(questions);
			free(answers);
			free(question_ptrs);
			free(answer_ptrs);
			free(scope_ptrs);
			free(question_lens);
			free(answer_lens);
			free(scope_lens);
			free(embeddings);

			return -1;
		}

		for (i = 0; i < current_batch; ++i) {
			size_t global_index = done + i;

			make_question(questions[i], SEARCH_BENCH_KEY_SIZE, prefix, global_index);
			make_answer(answers[i], SEARCH_BENCH_VALUE_SIZE, prefix, global_index);

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

		free(questions);
		free(answers);
		free(question_ptrs);
		free(answer_ptrs);
		free(scope_ptrs);
		free(question_lens);
		free(answer_lens);
		free(scope_lens);
		free(embeddings);

		if (ret != KVS_VECTOR_OK || success_count != current_batch || failed_count != 0) {
			fprintf(stderr,
			        "load batch failed done=%zu ret=%d success=%zu failed=%zu\n",
			        done,
			        ret,
			        success_count,
			        failed_count);
			return -1;
		}

		done += current_batch;
	}

	return 0;
}

static int result_same_record(
	const kvs_vector_search_result_t *lhs,
	const kvs_vector_search_result_t *rhs
) {
	if (!lhs || !rhs) {
		return 0;
	}

	if (lhs->question_len != rhs->question_len) {
		return 0;
	}

	if (lhs->scope_len != rhs->scope_len) {
		return 0;
	}

	if (!lhs->question || !rhs->question) {
		return 0;
	}

	if (lhs->question_len > 0 &&
	    memcmp(lhs->question, rhs->question, lhs->question_len) != 0) {
		return 0;
	}

	if (lhs->scope_len > 0) {
		if (!lhs->scope || !rhs->scope) {
			return 0;
		}

		if (memcmp(lhs->scope, rhs->scope, lhs->scope_len) != 0) {
			return 0;
		}
	}

	return 1;
}

static size_t count_overlap_results(
	const kvs_vector_search_result_t *exact_results,
	size_t exact_count,
	const kvs_vector_search_result_t *ann_results,
	size_t ann_count,
	size_t limit
) {
	size_t i;
	size_t j;
	size_t hit = 0;

	if (!exact_results || !ann_results || limit == 0) {
		return 0;
	}

	if (exact_count > limit) {
		exact_count = limit;
	}

	if (ann_count > limit) {
		ann_count = limit;
	}

	for (i = 0; i < exact_count; ++i) {
		for (j = 0; j < ann_count; ++j) {
			if (result_same_record(&exact_results[i], &ann_results[j])) {
				hit++;
				break;
			}
		}
	}

	return hit;
}

static int run_recall_bench(
	size_t record_count,
	size_t search_count,
	int topk,
	int dimension,
	const char *scope
) {
	kvs_vector_search_result_t *flat_results = NULL;
	kvs_vector_search_result_t *hnsw_results = NULL;
	float *query_embedding = NULL;

	size_t flat_count = 0;
	size_t hnsw_count = 0;
	size_t failed_flat = 0;
	size_t failed_hnsw = 0;
	size_t total_hit_at_1 = 0;
	size_t total_hit_at_k = 0;
	size_t total_possible_at_1 = 0;
	size_t total_possible_at_k = 0;

	long long flat_total_us = 0;
	long long hnsw_total_us = 0;
	long long flat_min_us = 0;
	long long flat_max_us = 0;
	long long hnsw_min_us = 0;
	long long hnsw_max_us = 0;

	size_t i;
	size_t result_capacity;
	int ret;

	if (topk <= 0) {
		topk = 10;
	}

	result_capacity = (size_t)topk;

	flat_results = (kvs_vector_search_result_t *)malloc(
		sizeof(kvs_vector_search_result_t) * result_capacity
	);
	hnsw_results = (kvs_vector_search_result_t *)malloc(
		sizeof(kvs_vector_search_result_t) * result_capacity
	);
	query_embedding = (float *)malloc((size_t)dimension * sizeof(float));

	if (!flat_results || !hnsw_results || !query_embedding) {
		fprintf(stderr, "malloc recall bench buffers failed\n");
		free(flat_results);
		free(hnsw_results);
		free(query_embedding);
		return -1;
	}

	memset(flat_results, 0, sizeof(kvs_vector_search_result_t) * result_capacity);
    memset(hnsw_results, 0, sizeof(kvs_vector_search_result_t) * result_capacity);

    if (!g_vector_state.ann_index.ready ||
        g_vector_state.ann_index.size == 0 ||
        !g_vector_state.ann_index.has_entry) {
        fprintf(stderr,
                "HNSW index is not ready: ready=%d size=%zu has_entry=%d\n",
                g_vector_state.ann_index.ready,
                g_vector_state.ann_index.size,
                g_vector_state.ann_index.has_entry);
        free(flat_results);
        free(hnsw_results);
        free(query_embedding);
        return -1;
    }

    printf("========== recall running ==========\n");

	for (i = 0; i < search_count; ++i) {
		size_t query_index = (i * 131u) % record_count;
		long long start_us;
		long long end_us;
		long long cost_us;
		size_t hit_at_1;
		size_t hit_at_k;

		fill_mock_embedding(query_embedding, dimension, query_index);

		flat_count = 0;
		hnsw_count = 0;

		// ret = kvs_vector_set_index_type_internal(KVS_VECTOR_INDEX_TYPE_FLAT);
		// if (ret != KVS_VECTOR_OK) {
		// 	failed_flat++;
		// 	continue;
		// }
        /*
        * recall 模式不能调用 kvs_vector_set_index_type_internal() 来频繁切换索引。
        *
        * 原因：
        * kvs_vector_set_index_type_internal(KVS_VECTOR_INDEX_TYPE_HNSW)
        * 会触发 HNSW rebuild。
        *
        * recall 每个 query 都要先跑 flat 再跑 hnsw，
        * 如果每次切回 hnsw 都 rebuild，会导致压测卡住。
        *
        * 这里直接切换 g_vector_state.index_type，只改变搜索分支，
        * 不重建任何索引。
        */
        g_vector_state.index_type = KVS_VECTOR_INDEX_TYPE_FLAT;

		start_us = now_us();

		ret = kvs_vector_search_with_embedding_internal(
			query_embedding,
			dimension,
			scope,
			scope ? strlen(scope) : 0,
			topk,
			-1.0f,
			flat_results,
			result_capacity,
			&flat_count
		);

		end_us = now_us();
		cost_us = end_us - start_us;

		if (ret != KVS_VECTOR_OK) {
			failed_flat++;
			continue;
		}

		flat_total_us += cost_us;
		if (flat_min_us == 0 || cost_us < flat_min_us) {
			flat_min_us = cost_us;
		}
		if (cost_us > flat_max_us) {
			flat_max_us = cost_us;
		}

		// ret = kvs_vector_set_index_type_internal(KVS_VECTOR_INDEX_TYPE_HNSW);
		// if (ret != KVS_VECTOR_OK) {
		// 	failed_hnsw++;
		// 	continue;
		// }
        g_vector_state.index_type = KVS_VECTOR_INDEX_TYPE_HNSW;

		start_us = now_us();

		ret = kvs_vector_search_with_embedding_internal(
			query_embedding,
			dimension,
			scope,
			scope ? strlen(scope) : 0,
			topk,
			-1.0f,
			hnsw_results,
			result_capacity,
			&hnsw_count
		);

		end_us = now_us();
		cost_us = end_us - start_us;

		if (ret != KVS_VECTOR_OK) {
			failed_hnsw++;
			continue;
		}

		hnsw_total_us += cost_us;
		if (hnsw_min_us == 0 || cost_us < hnsw_min_us) {
			hnsw_min_us = cost_us;
		}
		if (cost_us > hnsw_max_us) {
			hnsw_max_us = cost_us;
		}

		if (flat_count > 0) {
			total_possible_at_1 += 1;
			hit_at_1 = count_overlap_results(
				flat_results,
				flat_count,
				hnsw_results,
				hnsw_count,
				1
			);
			total_hit_at_1 += hit_at_1;
		}

		if (flat_count > 0) {
			size_t possible = flat_count < (size_t)topk ? flat_count : (size_t)topk;

			total_possible_at_k += possible;
			hit_at_k = count_overlap_results(
				flat_results,
				flat_count,
				hnsw_results,
				hnsw_count,
				(size_t)topk
			);
			total_hit_at_k += hit_at_k;
		}
	}

	printf("========== recall result ==========\n");
	printf("record_count=%zu\n", record_count);
	printf("search_count=%zu\n", search_count);
	printf("topk=%d\n", topk);
	printf("failed_flat=%zu\n", failed_flat);
	printf("failed_hnsw=%zu\n", failed_hnsw);

	printf("flat_total_us=%lld\n", (long long)flat_total_us);
	printf("flat_search_qps=%.2f\n",
	       flat_total_us > 0
	           ? (double)(search_count - failed_flat) /
	             ((double)flat_total_us / 1000000.0)
	           : 0.0);
	printf("flat_avg_latency_ms=%.4f\n",
	       (search_count - failed_flat) > 0
	           ? ((double)flat_total_us / 1000.0) /
	             (double)(search_count - failed_flat)
	           : 0.0);
	printf("flat_min_latency_ms=%.4f\n", (double)flat_min_us / 1000.0);
	printf("flat_max_latency_ms=%.4f\n", (double)flat_max_us / 1000.0);

	printf("hnsw_total_us=%lld\n", (long long)hnsw_total_us);
	printf("hnsw_search_qps=%.2f\n",
	       hnsw_total_us > 0
	           ? (double)(search_count - failed_hnsw) /
	             ((double)hnsw_total_us / 1000000.0)
	           : 0.0);
	printf("hnsw_avg_latency_ms=%.4f\n",
	       (search_count - failed_hnsw) > 0
	           ? ((double)hnsw_total_us / 1000.0) /
	             (double)(search_count - failed_hnsw)
	           : 0.0);
	printf("hnsw_min_latency_ms=%.4f\n", (double)hnsw_min_us / 1000.0);
	printf("hnsw_max_latency_ms=%.4f\n", (double)hnsw_max_us / 1000.0);

	printf("speedup=%.2f\n",
	       hnsw_total_us > 0
	           ? (double)flat_total_us / (double)hnsw_total_us
	           : 0.0);

	printf("recall_at_1=%.4f\n",
	       total_possible_at_1 > 0
	           ? (double)total_hit_at_1 / (double)total_possible_at_1
	           : 0.0);

	printf("recall_at_%d=%.4f\n",
	       topk,
	       total_possible_at_k > 0
	           ? (double)total_hit_at_k / (double)total_possible_at_k
	           : 0.0);

	if (flat_count > 0 && hnsw_count > 0) {
		size_t show_flat = flat_count < 3 ? flat_count : 3;
		size_t show_hnsw = hnsw_count < 3 ? hnsw_count : 3;

		printf("========== sample flat results ==========\n");
		for (i = 0; i < show_flat; ++i) {
			printf("[%zu] score=%.6f question=%.*s answer=%.*s\n",
			       i,
			       flat_results[i].score,
			       (int)flat_results[i].question_len,
			       flat_results[i].question ? flat_results[i].question : "",
			       (int)flat_results[i].answer_len,
			       flat_results[i].answer ? flat_results[i].answer : "");
		}

		printf("========== sample hnsw results ==========\n");
		for (i = 0; i < show_hnsw; ++i) {
			printf("[%zu] score=%.6f question=%.*s answer=%.*s\n",
			       i,
			       hnsw_results[i].score,
			       (int)hnsw_results[i].question_len,
			       hnsw_results[i].question ? hnsw_results[i].question : "",
			       (int)hnsw_results[i].answer_len,
			       hnsw_results[i].answer ? hnsw_results[i].answer : "");
		}
	}

	free(flat_results);
	free(hnsw_results);
	free(query_embedding);

	return 0;
}

static void print_usage(const char *prog) {
	printf("Usage:\n");
	printf("  %s <record_count> [search_count] [topk] [batch_size] [dimension] [scope] [prefix] [index_type] [hnsw_m] [ef_construction] [ef_search]\n", prog);
	printf("\n");
	printf("index_type:\n");
	printf("  flat   : use flat exact search\n");
	printf("  hnsw   : use HNSW-lite ANN search\n");
	printf("  recall : compare flat exact search with HNSW-lite ANN search\n");
	printf("\n");
	printf("HNSW params:\n");
	printf("  hnsw_m          : max neighbors per node, default 16\n");
	printf("  ef_construction : candidate size while building, default 64\n");
	printf("  ef_search       : candidate size while searching, default 64\n");
	printf("\n");
	printf("Examples:\n");
	printf("  %s 100000\n", prog);
	printf("  %s 100000 1000 10 1024 384 bench_scope search_100k flat\n", prog);
	printf("  %s 100000 1000 10 1024 384 bench_scope search_100k hnsw 16 128 128\n", prog);
	printf("  %s 100000 1000 10 1024 384 bench_scope recall_100k recall 32 256 256\n", prog);
}

int main(int argc, char **argv) {
	size_t record_count;
	size_t search_count = 1000;
	int topk = 10;
	size_t batch_size = SEARCH_BENCH_DEFAULT_BATCH;
	int dimension = SEARCH_BENCH_DEFAULT_DIM;
	const char *scope = "bench_scope";
	const char *prefix = "searchbench";
    const char *index_type_name = "flat";
    kvs_vector_index_type_t index_type = KVS_VECTOR_INDEX_TYPE_FLAT;

    size_t hnsw_m = KVS_VECTOR_HNSW_M;
    size_t hnsw_ef_construction = KVS_VECTOR_HNSW_EF_CONSTRUCTION;
    size_t hnsw_ef_search = KVS_VECTOR_HNSW_EF_SEARCH;

	kvs_vector_search_result_t *results = NULL;
	size_t results_count = 0;
	float *query_embedding = NULL;

	long long load_start_ms;
	long long load_end_ms;
	long long search_start_us;
	long long search_end_us;
	long long min_us = 0;
	long long max_us = 0;
	long long total_search_us = 0;

	size_t i;
	size_t found_total = 0;
	size_t failed_search = 0;

	kvs_vector_info_t info;

	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	record_count = (size_t)strtoull(argv[1], NULL, 10);

	if (argc >= 3) {
		search_count = (size_t)strtoull(argv[2], NULL, 10);
		if (search_count == 0) {
			search_count = 1000;
		}
	}

	if (argc >= 4) {
		topk = atoi(argv[3]);
		if (topk <= 0) {
			topk = 10;
		}
		if (topk > SEARCH_BENCH_MAX_TOPK) {
			topk = SEARCH_BENCH_MAX_TOPK;
		}
	}

	if (argc >= 5) {
		batch_size = (size_t)strtoull(argv[4], NULL, 10);
		if (batch_size == 0) {
			batch_size = SEARCH_BENCH_DEFAULT_BATCH;
		}
	}

	if (argc >= 6) {
		dimension = atoi(argv[5]);
		if (dimension <= 0) {
			dimension = SEARCH_BENCH_DEFAULT_DIM;
		}
	}

	if (argc >= 7) {
		scope = argv[6];
	}

	if (argc >= 8) {
		prefix = argv[7];
	}
    if (argc >= 9) {
        index_type_name = argv[8];

        if (strcasecmp(index_type_name, "hnsw") == 0 ||
            strcasecmp(index_type_name, "ann") == 0) {
            index_type = KVS_VECTOR_INDEX_TYPE_HNSW;
            index_type_name = "hnsw";
        } else if (strcasecmp(index_type_name, "recall") == 0 ||
                strcasecmp(index_type_name, "compare") == 0) {
            index_type = KVS_VECTOR_INDEX_TYPE_HNSW;
            index_type_name = "recall";
        } else {
            index_type = KVS_VECTOR_INDEX_TYPE_FLAT;
            index_type_name = "flat";
        }
    }

    if (argc >= 10) {
        hnsw_m = (size_t)strtoull(argv[9], NULL, 10);
        if (hnsw_m == 0) {
            hnsw_m = KVS_VECTOR_HNSW_M;
        }
    }

    if (argc >= 11) {
        hnsw_ef_construction = (size_t)strtoull(argv[10], NULL, 10);
        if (hnsw_ef_construction == 0) {
            hnsw_ef_construction = KVS_VECTOR_HNSW_EF_CONSTRUCTION;
        }
    }

    if (argc >= 12) {
        hnsw_ef_search = (size_t)strtoull(argv[11], NULL, 10);
        if (hnsw_ef_search == 0) {
            hnsw_ef_search = KVS_VECTOR_HNSW_EF_SEARCH;
        }
    }

	if (record_count == 0) {
		fprintf(stderr, "record_count must be greater than 0\n");
		return 1;
	}

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

	if (reset_flat_index_dimension(dimension) != 0) {
        fprintf(stderr, "reset flat index dimension failed dimension=%d\n", dimension);
        kvs_vector_destroy();
        mem_destroy();
        kvs_log_shutdown();
        return 1;
    }

    /*
    * 如果本次压测使用 hnsw / recall，则先设置 HNSW 参数。
    *
    * 注意：
    * 这里发生在 load_mock_records() 之前，此时 store 还没有数据，
    * 所以 kvs_vector_ann_index_set_params_internal() 触发 rebuild 的成本很低。
    * 后续 load 数据时会按新的 m / ef_construction / ef_search 建图。
    */
    if (index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
        if (kvs_vector_ann_index_set_params_internal(
                hnsw_m,
                hnsw_ef_construction,
                hnsw_ef_search) != KVS_VECTOR_OK) {
            fprintf(stderr,
                    "set hnsw params failed m=%zu ef_construction=%zu ef_search=%zu\n",
                    hnsw_m,
                    hnsw_ef_construction,
                    hnsw_ef_search);
            kvs_vector_destroy();
            mem_destroy();
            kvs_log_shutdown();
            return 1;
        }
    }

    if (kvs_vector_set_index_type_internal(index_type) != KVS_VECTOR_OK) {
        fprintf(stderr, "set vector index type failed type=%s\n", index_type_name);
        kvs_vector_destroy();
        mem_destroy();
        kvs_log_shutdown();
        return 1;
    }

	printf("========== vector search bench begin ==========\n");
	printf("record_count=%zu search_count=%zu topk=%d batch_size=%zu dimension=%d scope=%s prefix=%s index_type=%s hnsw_m=%zu ef_construction=%zu ef_search=%zu\n",
       record_count,
       search_count,
       topk,
       batch_size,
       dimension,
       scope && scope[0] ? scope : "<empty>",
       prefix,
       index_type_name,
       hnsw_m,
       hnsw_ef_construction,
       hnsw_ef_search);

	load_start_ms = now_ms();

	if (load_mock_records(
			record_count,
			batch_size,
			dimension,
			scope,
			prefix) != 0) {
		fprintf(stderr, "load mock records failed\n");
		kvs_vector_destroy();
		mem_destroy();
		kvs_log_shutdown();
		return 1;
	}

	load_end_ms = now_ms();

	printf("========== load result ==========\n");
	printf("load_elapsed_ms=%lld\n", (long long)(load_end_ms - load_start_ms));
	printf("load_write_qps=%.2f\n",
	       (double)record_count /
	       (((double)(load_end_ms - load_start_ms) / 1000.0) <= 0.0
	        ? 0.001
	        : ((double)(load_end_ms - load_start_ms) / 1000.0)));

	memset(&info, 0, sizeof(info));
	if (kvs_vector_info(&info) == KVS_VECTOR_OK) {
		printf("active_records=%zu\n", info.active_records);
		printf("index_size=%zu\n", info.index_size);
		printf("dimension=%zu\n", info.dimension);
		printf("index_type=%s\n", index_type_name);
        if (index_type == KVS_VECTOR_INDEX_TYPE_HNSW) {
            printf("ann_size=%zu\n", g_vector_state.ann_index.size);
            printf("ann_m=%zu\n", g_vector_state.ann_index.m);
            printf("ann_ef_construction=%zu\n", g_vector_state.ann_index.ef_construction);
            printf("ann_ef_search=%zu\n", g_vector_state.ann_index.ef_search);
        }
	}

    if (strcasecmp(index_type_name, "recall") == 0) {
        int recall_ret;

        recall_ret = run_recall_bench(
            record_count,
            search_count,
            topk,
            dimension,
            scope
        );

        printf("========== vector search bench end ==========\n");

        kvs_vector_destroy();
        mem_destroy();
        kvs_log_shutdown();

        return recall_ret == 0 ? 0 : 1;
    }

	results = (kvs_vector_search_result_t *)malloc(
		sizeof(kvs_vector_search_result_t) * (size_t)topk
	);
	query_embedding = (float *)malloc((size_t)dimension * sizeof(float));

	if (!results || !query_embedding) {
		fprintf(stderr, "malloc search buffers failed\n");
		free(results);
		free(query_embedding);
		kvs_vector_destroy();
		mem_destroy();
		kvs_log_shutdown();
		return 1;
	}

	memset(results, 0, sizeof(kvs_vector_search_result_t) * (size_t)topk);

	printf("========== search running ==========\n");

	for (i = 0; i < search_count; ++i) {
		size_t query_index = (i * 131u) % record_count;
		long long one_start_us;
		long long one_end_us;
		long long cost_us;
		int ret;

		fill_mock_embedding(query_embedding, dimension, query_index);

		results_count = 0;
		one_start_us = now_us();

		ret = kvs_vector_search_with_embedding_internal(
			query_embedding,
			dimension,
			scope,
			scope ? strlen(scope) : 0,
			topk,
			-1.0f,
			results,
			(size_t)topk,
			&results_count
		);

		one_end_us = now_us();
		cost_us = one_end_us - one_start_us;

		if (ret != KVS_VECTOR_OK) {
			failed_search++;
			continue;
		}

		found_total += results_count;
		total_search_us += cost_us;

		if (min_us == 0 || cost_us < min_us) {
			min_us = cost_us;
		}

		if (cost_us > max_us) {
			max_us = cost_us;
		}
	}

	search_start_us = 0;
	search_end_us = total_search_us;

	printf("========== search result ==========\n");
	printf("record_count=%zu\n", record_count);
	printf("search_count=%zu\n", search_count);
	printf("failed_search=%zu\n", failed_search);
	printf("topk=%d\n", topk);
	printf("total_search_us=%lld\n", (long long)total_search_us);
	printf("search_qps=%.2f\n",
	       total_search_us > 0
	           ? (double)(search_count - failed_search) /
	             ((double)total_search_us / 1000000.0)
	           : 0.0);
	printf("avg_latency_ms=%.4f\n",
	       (search_count - failed_search) > 0
	           ? ((double)total_search_us / 1000.0) /
	             (double)(search_count - failed_search)
	           : 0.0);
	printf("min_latency_ms=%.4f\n", (double)min_us / 1000.0);
	printf("max_latency_ms=%.4f\n", (double)max_us / 1000.0);
	printf("avg_results=%.2f\n",
	       (search_count - failed_search) > 0
	           ? (double)found_total / (double)(search_count - failed_search)
	           : 0.0);

	if (results_count > 0) {
		size_t show = results_count < 3 ? results_count : 3;

		printf("========== sample results ==========\n");
		for (i = 0; i < show; ++i) {
			printf("[%zu] score=%.6f question=%.*s answer=%.*s\n",
			       i,
			       results[i].score,
			       (int)results[i].question_len,
			       results[i].question ? results[i].question : "",
			       (int)results[i].answer_len,
			       results[i].answer ? results[i].answer : "");
		}
	}

	printf("========== vector search bench end ==========\n");

	free(results);
	free(query_embedding);

	kvs_vector_destroy();
	mem_destroy();
	kvs_log_shutdown();

	(void)search_start_us;
	(void)search_end_us;

	return failed_search == 0 ? 0 : 1;
}