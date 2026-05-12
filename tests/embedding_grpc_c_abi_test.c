#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvs_embedding_grpc_client.h"

/*
 * 纯 C 测试程序。
 *
 * 目标：
 *   验证 C 代码可以通过 C ABI 调用 C++ gRPC 客户端。
 */

static void print_first_values(const float *values, int dim, int n) {
	int i;

	if (!values || dim <= 0) {
		return;
	}

	if (n > dim) {
		n = dim;
	}

	printf("[");
	for (i = 0; i < n; ++i) {
		if (i > 0) {
			printf(", ");
		}
		printf("%.6f", values[i]);
	}
	printf("]");
}

int main(int argc, char **argv) {
	const char *addr = "127.0.0.1:50051";
	const char *text = "向量检索测试";
	int timeout_ms = 3000;
	int ret;
	int dimension = 0;
	char provider[128];
	char model[256];

	float *embedding = NULL;
	int embedding_dim = 0;

	const char *batch_texts[8];
	size_t batch_lens[8];
	float *batch_embeddings = NULL;
	int batch_dim = 0;
	size_t batch_count = 8;

	float *parallel_embeddings = NULL;
	int parallel_dim = 0;
	size_t parallel_sub_batch_size = 2;
	size_t parallel_max_inflight = 2;

	if (argc >= 2) {
		addr = argv[1];
	}

	if (argc >= 3) {
		text = argv[2];
	}

	memset(provider, 0, sizeof(provider));
	memset(model, 0, sizeof(model));

	printf("C ABI embedding gRPC test\n");
	printf("addr=%s\n", addr);
	printf("text=%s\n", text);

	ret = kvs_embedding_grpc_init(addr, timeout_ms);
	if (ret != KVS_EMBEDDING_GRPC_OK) {
		printf("FAILED: init ret=%d\n", ret);
		return 1;
	}

	ret = kvs_embedding_grpc_health(
		&dimension,
		provider,
		sizeof(provider),
		model,
		sizeof(model)
	);
	if (ret != KVS_EMBEDDING_GRPC_OK) {
		printf("FAILED: health ret=%d\n", ret);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	printf("health:\n");
	printf("  provider=%s\n", provider);
	printf("  model=%s\n", model);
	printf("  dimension=%d\n", dimension);

	ret = kvs_embedding_grpc_generate(
		text,
		strlen(text),
		&embedding,
		&embedding_dim
	);
	if (ret != KVS_EMBEDDING_GRPC_OK) {
		printf("FAILED: generate ret=%d\n", ret);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	if (!embedding || embedding_dim <= 0) {
		printf("FAILED: invalid embedding\n");
		kvs_embedding_grpc_free_embedding(embedding);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	printf("embedding:\n");
	printf("  dimension=%d\n", embedding_dim);
	printf("  first5=");
	print_first_values(embedding, embedding_dim, 5);
	printf("\n");

	kvs_embedding_grpc_free_embedding(embedding);
	embedding = NULL;

	batch_texts[0] = "你好 世界";
	batch_texts[1] = "hello world";
	batch_texts[2] = "向量批量检索测试";
	batch_texts[3] = "KVLite 向量写入";
	batch_texts[4] = "gRPC 并发批量请求";
	batch_texts[5] = "embedding server 性能测试";
	batch_texts[6] = "semantic search";
	batch_texts[7] = "百万级导入压测";

	for (size_t i = 0; i < batch_count; ++i) {
		batch_lens[i] = strlen(batch_texts[i]);
	}

	ret = kvs_embedding_grpc_generate_batch(
		batch_texts,
		batch_lens,
		batch_count,
		&batch_embeddings,
		&batch_dim
	);
	if (ret != KVS_EMBEDDING_GRPC_OK) {
		printf("FAILED: batch generate ret=%d\n", ret);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	if (!batch_embeddings || batch_dim <= 0) {
		printf("FAILED: invalid batch embeddings\n");
		kvs_embedding_grpc_free_embeddings(batch_embeddings);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	if (batch_dim != dimension) {
		printf("FAILED: batch dimension mismatch health_dim=%d batch_dim=%d\n",
		       dimension,
		       batch_dim);
		kvs_embedding_grpc_free_embeddings(batch_embeddings);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	printf("batch embedding:\n");
	printf("  count=%zu\n", batch_count);
	printf("  dimension=%d\n", batch_dim);

	for (size_t i = 0; i < batch_count; ++i) {
		printf("  [%zu] text=\"%s\" first5=", i, batch_texts[i]);
		print_first_values(batch_embeddings + (i * (size_t)batch_dim), batch_dim, 5);
		printf("\n");
	}

	ret = kvs_embedding_grpc_generate_batch_parallel(
		batch_texts,
		batch_lens,
		batch_count,
		parallel_sub_batch_size,
		parallel_max_inflight,
		&parallel_embeddings,
		&parallel_dim
	);
	if (ret != KVS_EMBEDDING_GRPC_OK) {
		printf("FAILED: parallel batch generate ret=%d\n", ret);
		kvs_embedding_grpc_free_embeddings(batch_embeddings);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	if (!parallel_embeddings || parallel_dim <= 0) {
		printf("FAILED: invalid parallel batch embeddings\n");
		kvs_embedding_grpc_free_embeddings(parallel_embeddings);
		kvs_embedding_grpc_free_embeddings(batch_embeddings);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	if (parallel_dim != dimension) {
		printf("FAILED: parallel dimension mismatch health_dim=%d parallel_dim=%d\n",
		       dimension,
		       parallel_dim);
		kvs_embedding_grpc_free_embeddings(parallel_embeddings);
		kvs_embedding_grpc_free_embeddings(batch_embeddings);
		kvs_embedding_grpc_destroy();
		return 1;
	}

	printf("parallel batch embedding:\n");
	printf("  count=%zu\n", batch_count);
	printf("  sub_batch_size=%zu\n", parallel_sub_batch_size);
	printf("  max_inflight=%zu\n", parallel_max_inflight);
	printf("  dimension=%d\n", parallel_dim);

	for (size_t i = 0; i < batch_count; ++i) {
		printf("  [%zu] text=\"%s\" first5=", i, batch_texts[i]);
		print_first_values(
			parallel_embeddings + (i * (size_t)parallel_dim),
			parallel_dim,
			5
		);
		printf("\n");
	}

	kvs_embedding_grpc_free_embeddings(parallel_embeddings);
	kvs_embedding_grpc_free_embeddings(batch_embeddings);
	kvs_embedding_grpc_destroy();

	printf("C ABI embedding gRPC test passed\n");
	return 0;
}