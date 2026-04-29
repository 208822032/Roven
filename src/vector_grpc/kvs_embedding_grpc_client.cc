#include "kvs_embedding_grpc_client.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include "kvs_log.h"

#include "embedding.grpc.pb.h"

#include <cstdio>

#define KVS_EMBEDDING_GRPC_LOG_MODULE "embedding-grpc"

namespace{

class KvsEmbeddingGrpcClient {
public:
    explicit KvsEmbeddingGrpcClient(const std::string& addr, int timeout_ms)
        : addr_(addr), 
          timeout_ms_(timeout_ms > 0 ? timeout_ms : 1000),
          stub_(kvlite::embedding::EmbeddingService::NewStub(
              grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()))) {}

    int Health(int *dimension, char *provider, size_t provider_cap, char *model, size_t model_cap) {
        grpc::ClientContext context;
        kvlite::embedding::HealthRequest request;
        kvlite::embedding::HealthResponse response;

        SetDeadline(context);

        grpc::Status status = stub_->Health(&context, request, &response);
		if (!status.ok()) {
			KVS_LOG_ERROR(
				KVS_EMBEDDING_GRPC_LOG_MODULE,
				"health rpc failed addr=%s code=%d message=%s details=%s",
				addr_.c_str(),
				(int)status.error_code(),
				status.error_message().c_str(),
				status.error_details().c_str()
			);
			return KVS_EMBEDDING_GRPC_ERR_RPC_FAILED;
		}

        if (!response.ok() || response.dimension() <= 0) {
			KVS_LOG_ERROR(
				KVS_EMBEDDING_GRPC_LOG_MODULE,
				"health bad response addr=%s ok=%d dimension=%d provider=%s model=%s",
				addr_.c_str(),
				response.ok() ? 1 : 0,
				response.dimension(),
				response.provider().c_str(),
				response.model().c_str()
			);
			return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
		}

        if (dimension) {
            *dimension = response.dimension();
        }

        CopyString(provider, provider_cap, response.provider());
		CopyString(model, model_cap, response.model());

		KVS_LOG_DEBUG(
			KVS_EMBEDDING_GRPC_LOG_MODULE,
			"health success addr=%s provider=%s model=%s dimension=%d",
			addr_.c_str(),
			response.provider().c_str(),
			response.model().c_str(),
			response.dimension()
		);

		return KVS_EMBEDDING_GRPC_OK;
    }

    int Generate(
		const char *text,
		size_t text_len,
		float **out_embedding,
		int *out_dimension
	) {
		grpc::ClientContext context;
		kvlite::embedding::EmbedRequest request;
		kvlite::embedding::EmbedResponse response;

		if (!text || text_len == 0 || !out_embedding || !out_dimension) {
			return KVS_EMBEDDING_GRPC_ERR_INVALID_ARG;
		}

		*out_embedding = nullptr;
		*out_dimension = 0;

		SetDeadline(context);

		request.set_normalize(true);
		request.add_texts(std::string(text, text_len));

		grpc::Status status = stub_->Embed(&context, request, &response);
		if (!status.ok()) {
			KVS_LOG_ERROR(
				KVS_EMBEDDING_GRPC_LOG_MODULE,
				"embed rpc failed addr=%s text_len=%zu code=%d message=%s details=%s",
				addr_.c_str(),
				text_len,
				(int)status.error_code(),
				status.error_message().c_str(),
				status.error_details().c_str()
			);
			return KVS_EMBEDDING_GRPC_ERR_RPC_FAILED;
		}

		if (response.embeddings_size() != 1 || response.dimension() <= 0) {
			KVS_LOG_ERROR(
				KVS_EMBEDDING_GRPC_LOG_MODULE,
				"embed bad response addr=%s text_len=%zu embeddings_size=%d dimension=%d",
				addr_.c_str(),
				text_len,
				response.embeddings_size(),
				response.dimension()
			);
			return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
		}

		const kvlite::embedding::Embedding &embedding = response.embeddings(0);
		if (embedding.dimension() <= 0 ||
			embedding.values_size() != embedding.dimension() ||
			embedding.dimension() != response.dimension()) {
			KVS_LOG_ERROR(
				KVS_EMBEDDING_GRPC_LOG_MODULE,
				"embed invalid embedding addr=%s text_len=%zu embedding_dim=%d values_size=%d response_dim=%d",
				addr_.c_str(),
				text_len,
				embedding.dimension(),
				embedding.values_size(),
				response.dimension()
			);
			return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
		}

		size_t dim = static_cast<size_t>(embedding.dimension());
		float *values = static_cast<float *>(std::malloc(dim * sizeof(float)));
		if (!values) {
			return KVS_EMBEDDING_GRPC_ERR_NO_MEMORY;
		}

		for (size_t i = 0; i < dim; ++i) {
			values[i] = embedding.values(static_cast<int>(i));
		}

		*out_embedding = values;
		*out_dimension = embedding.dimension();

		KVS_LOG_DEBUG(
			KVS_EMBEDDING_GRPC_LOG_MODULE,
			"embed success addr=%s text_len=%zu dimension=%d",
			addr_.c_str(),
			text_len,
			embedding.dimension()
		);

		return KVS_EMBEDDING_GRPC_OK;
	}
private:
    void SetDeadline(grpc::ClientContext &context) const {
		context.set_deadline(
			std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_));
	}

    static void CopyString(char *dst, size_t dst_cap, const std::string &src) {
		if (!dst || dst_cap == 0) {
			return;
		}

		size_t n = src.size();
		if (n >= dst_cap) {
			n = dst_cap - 1;
		}

		if (n > 0) {
			std::memcpy(dst, src.data(), n);
		}
		dst[n] = '\0';
	}
    std::string addr_;
    int timeout_ms_;
    std::unique_ptr<kvlite::embedding::EmbeddingService::Stub> stub_;
};

std::mutex g_client_mu;
std::unique_ptr<KvsEmbeddingGrpcClient> g_client;

}

extern "C" int kvs_embedding_grpc_init(const char* addr, int timeout_ms) {
    if (!addr || addr[0] == '\0') {
		return KVS_EMBEDDING_GRPC_ERR_INVALID_ARG;
	}

	std::lock_guard<std::mutex> lock(g_client_mu);

	try {
		g_client.reset(new KvsEmbeddingGrpcClient(addr, timeout_ms));
	} catch (...) {
		g_client.reset();
		return KVS_EMBEDDING_GRPC_ERR;
	}

	return KVS_EMBEDDING_GRPC_OK;
}

extern "C" void kvs_embedding_grpc_destroy(void) {
	std::lock_guard<std::mutex> lock(g_client_mu);
	g_client.reset();
}

extern "C" int kvs_embedding_grpc_health(
	int *dimension,
	char *provider,
	size_t provider_cap,
	char *model,
	size_t model_cap
) {
	std::lock_guard<std::mutex> lock(g_client_mu);

	if (!g_client) {
		return KVS_EMBEDDING_GRPC_ERR_NOT_INITIALIZED;
	}

	return g_client->Health(
		dimension,
		provider,
		provider_cap,
		model,
		model_cap
	);
}

extern "C" int kvs_embedding_grpc_generate(
	const char *text,
	size_t text_len,
	float **out_embedding,
	int *out_dimension
) {
	std::lock_guard<std::mutex> lock(g_client_mu);

	if (!g_client) {
		return KVS_EMBEDDING_GRPC_ERR_NOT_INITIALIZED;
	}

	return g_client->Generate(
		text,
		text_len,
		out_embedding,
		out_dimension
	);
}

extern "C" void kvs_embedding_grpc_free_embedding(float *embedding) {
	if (embedding) {
		std::free(embedding);
	}
}

