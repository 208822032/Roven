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
	kvs_embedding_grpc_destroy();

	printf("C ABI embedding gRPC test passed\n");
	return 0;
}