#include "kvs_embedding_grpc_client.h"

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kvs_log.h"
#include "embedding.grpc.pb.h"

#define KVS_EMBEDDING_GRPC_LOG_MODULE "embedding-grpc"

namespace {

class KvsEmbeddingGrpcClient {
public:
	explicit KvsEmbeddingGrpcClient(const std::string& addr, int timeout_ms)
		: addr_(addr),
		  timeout_ms_(timeout_ms > 0 ? timeout_ms : 1000),
		  stub_(kvlite::embedding::EmbeddingService::NewStub(
			  grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()))) {}

	int Health(
		int *dimension,
		char *provider,
		size_t provider_cap,
		char *model,
		size_t model_cap
	) {
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

	int GenerateBatch(
		const char **texts,
		const size_t *text_lens,
		size_t text_count,
		float **out_embeddings,
		int *out_dimension
	) {
		grpc::ClientContext context;
		kvlite::embedding::EmbedRequest request;
		kvlite::embedding::EmbedResponse response;
		size_t i;
		size_t dim;
		size_t total_values;
		float *values = nullptr;

		if (!texts || !text_lens || text_count == 0 || !out_embeddings || !out_dimension) {
			return KVS_EMBEDDING_GRPC_ERR_INVALID_ARG;
		}

		*out_embeddings = nullptr;
		*out_dimension = 0;

		for (i = 0; i < text_count; ++i) {
			if (!texts[i] || text_lens[i] == 0) {
				return KVS_EMBEDDING_GRPC_ERR_INVALID_ARG;
			}
		}

		SetDeadline(context);

		request.set_normalize(true);

		for (i = 0; i < text_count; ++i) {
			request.add_texts(std::string(texts[i], text_lens[i]));
		}

		grpc::Status status = stub_->Embed(&context, request, &response);
		if (!status.ok()) {
			KVS_LOG_ERROR(
				KVS_EMBEDDING_GRPC_LOG_MODULE,
				"batch embed rpc failed addr=%s count=%zu code=%d message=%s details=%s",
				addr_.c_str(),
				text_count,
				(int)status.error_code(),
				status.error_message().c_str(),
				status.error_details().c_str()
			);
			return KVS_EMBEDDING_GRPC_ERR_RPC_FAILED;
		}

		if (response.dimension() <= 0 ||
			response.embeddings_size() != static_cast<int>(text_count)) {
			KVS_LOG_ERROR(
				KVS_EMBEDDING_GRPC_LOG_MODULE,
				"batch embed bad response addr=%s count=%zu embeddings_size=%d dimension=%d",
				addr_.c_str(),
				text_count,
				response.embeddings_size(),
				response.dimension()
			);
			return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
		}

		dim = static_cast<size_t>(response.dimension());
		total_values = text_count * dim;

		if (dim == 0 || total_values / dim != text_count) {
			return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
		}

		values = static_cast<float *>(std::malloc(total_values * sizeof(float)));
		if (!values) {
			return KVS_EMBEDDING_GRPC_ERR_NO_MEMORY;
		}

		for (i = 0; i < text_count; ++i) {
			const kvlite::embedding::Embedding &embedding =
				response.embeddings(static_cast<int>(i));

			if (embedding.dimension() <= 0 ||
				embedding.values_size() != embedding.dimension() ||
				embedding.dimension() != response.dimension()) {
				KVS_LOG_ERROR(
					KVS_EMBEDDING_GRPC_LOG_MODULE,
					"batch embed invalid embedding addr=%s index=%zu embedding_dim=%d values_size=%d response_dim=%d",
					addr_.c_str(),
					i,
					embedding.dimension(),
					embedding.values_size(),
					response.dimension()
				);
				std::free(values);
				return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
			}

			for (size_t j = 0; j < dim; ++j) {
				values[i * dim + j] = embedding.values(static_cast<int>(j));
			}
		}

		*out_embeddings = values;
		*out_dimension = response.dimension();

		KVS_LOG_DEBUG(
			KVS_EMBEDDING_GRPC_LOG_MODULE,
			"batch embed success addr=%s count=%zu dimension=%d",
			addr_.c_str(),
			text_count,
			response.dimension()
		);

		return KVS_EMBEDDING_GRPC_OK;
	}

	int GenerateBatchParallel(
		const char **texts,
		const size_t *text_lens,
		size_t text_count,
		size_t batch_size,
		size_t max_inflight,
		float **out_embeddings,
		int *out_dimension
	) {
		struct BatchJob {
			size_t start;
			size_t count;
			float *embeddings;
			int dimension;
			int ret;
		};

		std::vector<BatchJob> jobs;
		std::vector<std::thread> workers;
		std::atomic<size_t> next_job(0);
		size_t batch_count;
		size_t worker_count;
		int final_dim = 0;
		float *merged = nullptr;

		if (!texts || !text_lens || text_count == 0 || !out_embeddings || !out_dimension) {
			return KVS_EMBEDDING_GRPC_ERR_INVALID_ARG;
		}

		*out_embeddings = nullptr;
		*out_dimension = 0;

		if (batch_size == 0) {
			batch_size = 64;
		}

		if (max_inflight == 0) {
			max_inflight = 2;
		}

		for (size_t i = 0; i < text_count; ++i) {
			if (!texts[i] || text_lens[i] == 0) {
				return KVS_EMBEDDING_GRPC_ERR_INVALID_ARG;
			}
		}

		batch_count = (text_count + batch_size - 1) / batch_size;
		if (batch_count == 0) {
			return KVS_EMBEDDING_GRPC_ERR_INVALID_ARG;
		}

		jobs.resize(batch_count);

		for (size_t i = 0; i < batch_count; ++i) {
			size_t start = i * batch_size;
			size_t remain = text_count - start;
			size_t count = remain < batch_size ? remain : batch_size;

			jobs[i].start = start;
			jobs[i].count = count;
			jobs[i].embeddings = nullptr;
			jobs[i].dimension = 0;
			jobs[i].ret = KVS_EMBEDDING_GRPC_OK;
		}

		worker_count = std::min(max_inflight, batch_count);
		if (worker_count == 0) {
			worker_count = 1;
		}

		KVS_LOG_DEBUG(
			KVS_EMBEDDING_GRPC_LOG_MODULE,
			"parallel batch embed start addr=%s count=%zu sub_batch_size=%zu batch_count=%zu max_inflight=%zu worker_count=%zu",
			addr_.c_str(),
			text_count,
			batch_size,
			batch_count,
			max_inflight,
			worker_count
		);

		for (size_t i = 0; i < worker_count; ++i) {
			workers.emplace_back([this, &jobs, &texts, &text_lens, &next_job]() {
				for (;;) {
					size_t job_index = next_job.fetch_add(1);
					BatchJob *job;

					if (job_index >= jobs.size()) {
						break;
					}

					job = &jobs[job_index];

					job->ret = this->GenerateBatch(
						texts + job->start,
						text_lens + job->start,
						job->count,
						&job->embeddings,
						&job->dimension
					);
				}
			});
		}

		for (auto &worker : workers) {
			if (worker.joinable()) {
				worker.join();
			}
		}

		for (size_t i = 0; i < jobs.size(); ++i) {
			if (jobs[i].ret != KVS_EMBEDDING_GRPC_OK) {
				KVS_LOG_ERROR(
					KVS_EMBEDDING_GRPC_LOG_MODULE,
					"parallel batch embed failed addr=%s job=%zu start=%zu count=%zu ret=%d",
					addr_.c_str(),
					i,
					jobs[i].start,
					jobs[i].count,
					jobs[i].ret
				);

				for (size_t j = 0; j < jobs.size(); ++j) {
					if (jobs[j].embeddings) {
						std::free(jobs[j].embeddings);
						jobs[j].embeddings = nullptr;
					}
				}

				return jobs[i].ret;
			}

			if (!jobs[i].embeddings || jobs[i].dimension <= 0) {
				for (size_t j = 0; j < jobs.size(); ++j) {
					if (jobs[j].embeddings) {
						std::free(jobs[j].embeddings);
						jobs[j].embeddings = nullptr;
					}
				}

				return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
			}

			if (final_dim == 0) {
				final_dim = jobs[i].dimension;
			} else if (final_dim != jobs[i].dimension) {
				KVS_LOG_ERROR(
					KVS_EMBEDDING_GRPC_LOG_MODULE,
					"parallel batch embed dimension mismatch addr=%s job=%zu expected=%d actual=%d",
					addr_.c_str(),
					i,
					final_dim,
					jobs[i].dimension
				);

				for (size_t j = 0; j < jobs.size(); ++j) {
					if (jobs[j].embeddings) {
						std::free(jobs[j].embeddings);
						jobs[j].embeddings = nullptr;
					}
				}

				return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
			}
		}

		if (final_dim <= 0) {
			for (size_t i = 0; i < jobs.size(); ++i) {
				if (jobs[i].embeddings) {
					std::free(jobs[i].embeddings);
					jobs[i].embeddings = nullptr;
				}
			}

			return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
		}

		size_t total_values = text_count * static_cast<size_t>(final_dim);
		if (static_cast<size_t>(final_dim) == 0 ||
			total_values / static_cast<size_t>(final_dim) != text_count) {
			for (size_t i = 0; i < jobs.size(); ++i) {
				if (jobs[i].embeddings) {
					std::free(jobs[i].embeddings);
					jobs[i].embeddings = nullptr;
				}
			}

			return KVS_EMBEDDING_GRPC_ERR_BAD_RESPONSE;
		}

		merged = static_cast<float *>(std::malloc(total_values * sizeof(float)));
		if (!merged) {
			for (size_t i = 0; i < jobs.size(); ++i) {
				if (jobs[i].embeddings) {
					std::free(jobs[i].embeddings);
					jobs[i].embeddings = nullptr;
				}
			}

			return KVS_EMBEDDING_GRPC_ERR_NO_MEMORY;
		}

		for (size_t i = 0; i < jobs.size(); ++i) {
			size_t dst_offset = jobs[i].start * static_cast<size_t>(final_dim);
			size_t copy_values = jobs[i].count * static_cast<size_t>(final_dim);

			std::memcpy(
				merged + dst_offset,
				jobs[i].embeddings,
				copy_values * sizeof(float)
			);

			std::free(jobs[i].embeddings);
			jobs[i].embeddings = nullptr;
		}

		*out_embeddings = merged;
		*out_dimension = final_dim;

		KVS_LOG_DEBUG(
			KVS_EMBEDDING_GRPC_LOG_MODULE,
			"parallel batch embed success addr=%s count=%zu sub_batch_size=%zu batch_count=%zu max_inflight=%zu dimension=%d",
			addr_.c_str(),
			text_count,
			batch_size,
			batch_count,
			max_inflight,
			final_dim
		);

		return KVS_EMBEDDING_GRPC_OK;
	}

private:
	void SetDeadline(grpc::ClientContext &context) const {
		context.set_deadline(
			std::chrono::system_clock::now() +
			std::chrono::milliseconds(timeout_ms_));
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

extern "C" int kvs_embedding_grpc_generate_batch(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	float **out_embeddings,
	int *out_dimension
) {
	std::lock_guard<std::mutex> lock(g_client_mu);

	if (!g_client) {
		return KVS_EMBEDDING_GRPC_ERR_NOT_INITIALIZED;
	}

	return g_client->GenerateBatch(
		texts,
		text_lens,
		text_count,
		out_embeddings,
		out_dimension
	);
}

extern "C" int kvs_embedding_grpc_generate_batch_parallel(
	const char **texts,
	const size_t *text_lens,
	size_t text_count,
	size_t batch_size,
	size_t max_inflight,
	float **out_embeddings,
	int *out_dimension
) {
	std::lock_guard<std::mutex> lock(g_client_mu);

	if (!g_client) {
		return KVS_EMBEDDING_GRPC_ERR_NOT_INITIALIZED;
	}

	return g_client->GenerateBatchParallel(
		texts,
		text_lens,
		text_count,
		batch_size,
		max_inflight,
		out_embeddings,
		out_dimension
	);
}

extern "C" void kvs_embedding_grpc_free_embedding(float *embedding) {
	if (embedding) {
		std::free(embedding);
	}
}

extern "C" void kvs_embedding_grpc_free_embeddings(float *embeddings) {
	if (embeddings) {
		std::free(embeddings);
	}
}