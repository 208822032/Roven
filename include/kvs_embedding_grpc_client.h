#ifndef KVS_EMBEDDING_GRPC_CLIENT_H
#define KVS_EMBEDDING_GRPC_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * gRPC embedding client 状态码。
 */
typedef enum kvs_embedding_grpc_status_e {
	KVS_EMBEDDING_GRPC_OK = 0,
	KVS_EMBEDDING_GRPC_ERR = -1,
	KVS_EMBEDDING_GRPC_ERR_INVALID_ARG = -2,
	KVS_EMBEDDING_GRPC_ERR_NOT_INITIALIZED = -3,
	KVS_EMBEDDING_GRPC_ERR_RPC_FAILED = -4,
	KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE = -5,
	KVS_EMBEDDING_GRPC_ERR_NO_MEMORY = -6,
} kvs_embedding_grpc_status_t;

/*
 * 初始化 gRPC embedding client。
 *
 * addr:
 *   embedding server 地址，例如 "127.0.0.1:50051"。
 *
 * timeout_ms:
 *   单次 RPC 超时时间，毫秒。
 */
int kvs_embedding_grpc_init(const char *addr, int timeout_ms);

/*
 * 销毁 gRPC embedding client。
 */
void kvs_embedding_grpc_destroy(void);

/*
 * 健康检查。
 *
 * dimension:
 *   返回 embedding server 当前维度。
 *
 * provider/provider_cap:
 *   可选，返回 provider 字符串。
 *
 * model/model_cap:
 *   可选，返回模型名。
 */
int kvs_embedding_grpc_health(
	int *dimension,
	char *provider,
	size_t provider_cap,
	char *model,
	size_t model_cap
);

/*
 * 生成单条文本 embedding。
 *
 * text/text_len:
 *   输入文本。
 *
 * out_embedding:
 *   输出 float 数组。由本函数 malloc 分配。
 *   调用方必须使用 kvs_embedding_grpc_free_embedding 释放。
 *
 * out_dimension:
 *   输出维度。
 */
int kvs_embedding_grpc_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dimension
);

/*
 * 释放 kvs_embedding_grpc_generate 返回的 embedding。
 */
void kvs_embedding_grpc_free_embedding(float *embedding);

#ifdef __cplusplus
}
#endif

#endif