/*
 * Faiss benchmark-only index backend — C++ implementation.
 *
 * Compile only when ENABLE_FAISS_BASELINE=1.
 * Default builds do not compile or link this file.
 */

#include "kvs_vector_faiss_backend.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/IndexIDMap.h>
#include <faiss/MetricType.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>

struct kvs_faiss_index_s {
	faiss::Index *index;       /* IndexIDMap2 wrapping the real index */
	int kind;
	size_t dim;
};

extern "C" {

static const char *kvs_faiss_kind_name(int kind) {
	switch (kind) {
	case KVS_FAISS_INDEX_FLAT_IP: return "flat_ip";
	case KVS_FAISS_INDEX_HNSW:    return "hnsw";
	default: return "unknown";
	}
}

kvs_faiss_index_t *kvs_faiss_index_create(
	int kind,
	size_t dim,
	size_t M,
	size_t ef_construction
) {
	kvs_faiss_index_t *faiss_idx;
	faiss::Index *storage = nullptr;
	faiss::IndexIDMap2 *idmap = nullptr;

	if (dim == 0) return nullptr;

	try {
		switch (kind) {
		case KVS_FAISS_INDEX_FLAT_IP: {
			storage = new faiss::IndexFlatIP((int)dim);
			break;
		}
		case KVS_FAISS_INDEX_HNSW: {
			size_t m = M ? M : 32;
			size_t efc = ef_construction ? ef_construction : 200;
			auto *hnsw = new faiss::IndexHNSWFlat((int)dim, (int)m,
				faiss::METRIC_INNER_PRODUCT);
			hnsw->hnsw.efConstruction = (int)efc;
			storage = hnsw;
			break;
		}
		default:
			std::fprintf(stderr, "[faiss] create: unsupported kind=%d\n", kind);
			return nullptr;
		}

		/* Wrap with IndexIDMap2 so add_with_ids works for any storage type. */
		idmap = new faiss::IndexIDMap2(storage);
	} catch (const faiss::FaissException &e) {
		std::fprintf(stderr, "[faiss] create FaissException: %s\n", e.what());
		delete storage;
		return nullptr;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[faiss] create std::exception: %s\n", e.what());
		delete storage;
		return nullptr;
	} catch (...) {
		std::fprintf(stderr, "[faiss] create: unknown exception\n");
		delete storage;
		return nullptr;
	}

	faiss_idx = (kvs_faiss_index_t *)std::malloc(sizeof(kvs_faiss_index_t));
	if (!faiss_idx) {
		delete idmap; /* idmap owns storage */
		return nullptr;
	}
	faiss_idx->index = idmap;
	faiss_idx->kind = kind;
	faiss_idx->dim = dim;

	std::fprintf(stderr, "[faiss] created %s dim=%zu M=%zu ef_construction=%zu\n",
	             kvs_faiss_kind_name(kind), dim, M, ef_construction);
	return faiss_idx;
}

int kvs_faiss_index_add(
	kvs_faiss_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
) {
	if (!index || !index->index || !embedding || embedding_dim <= 0) {
		std::fprintf(stderr, "[faiss] add: invalid args\n");
		return -1;
	}
	if ((size_t)embedding_dim != index->dim) {
		std::fprintf(stderr, "[faiss] add: dim mismatch index=%zu input=%d\n",
		             index->dim, embedding_dim);
		return -1;
	}

	faiss::idx_t id = (faiss::idx_t)record_index;
	try {
		index->index->add_with_ids(1, embedding, &id);
	} catch (const faiss::FaissException &e) {
		std::fprintf(stderr, "[faiss] add FaissException: %s id=%lld dim=%zu\n",
		             e.what(), (long long)id, index->dim);
		return -1;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[faiss] add std::exception: %s\n", e.what());
		return -1;
	} catch (...) {
		std::fprintf(stderr, "[faiss] add: unknown exception\n");
		return -1;
	}
	return 0;
}

int kvs_faiss_index_search(
	kvs_faiss_index_t *index,
	const float *query_embedding,
	int query_dim,
	int topk,
	size_t *out_record_indices,
	float *out_scores
) {
	if (!index || !index->index || !query_embedding || query_dim <= 0)
		return -1;
	if ((size_t)query_dim != index->dim) {
		std::fprintf(stderr, "[faiss] search: dim mismatch index=%zu query=%d\n",
		             index->dim, query_dim);
		return -1;
	}
	if (topk <= 0 || !out_record_indices || !out_scores)
		return -1;

	faiss::idx_t ntotal = index->index->ntotal;
	if (ntotal == 0) return 0;

	int k = topk;
	if (k > (int)ntotal) k = (int)ntotal;

	std::vector<faiss::idx_t> ids((size_t)k);
	std::vector<float> dists((size_t)k);

	try {
		index->index->search(1, query_embedding, k, dists.data(), ids.data());
	} catch (const faiss::FaissException &e) {
		std::fprintf(stderr, "[faiss] search FaissException: %s\n", e.what());
		return -1;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[faiss] search std::exception: %s\n", e.what());
		return -1;
	} catch (...) {
		std::fprintf(stderr, "[faiss] search: unknown exception\n");
		return -1;
	}

	for (int i = 0; i < k; ++i) {
		out_record_indices[i] = (size_t)ids[i];
		out_scores[i] = dists[i];
	}
	return k;
}

int kvs_faiss_index_reset(kvs_faiss_index_t *index) {
	if (!index || !index->index) {
		std::fprintf(stderr, "[faiss] reset: invalid args\n");
		return -1;
	}
	try {
		index->index->reset();
	} catch (const faiss::FaissException &e) {
		std::fprintf(stderr, "[faiss] reset FaissException: %s\n", e.what());
		return -1;
	} catch (...) {
		std::fprintf(stderr, "[faiss] reset: unknown exception\n");
		return -1;
	}
	return 0;
}

void kvs_faiss_index_destroy(kvs_faiss_index_t *index) {
	if (!index) return;
	if (index->index) {
		delete index->index;  /* IndexIDMap2 owns and deletes storage */
		index->index = nullptr;
	}
	std::free(index);
}

size_t kvs_faiss_index_ntotal(const kvs_faiss_index_t *index) {
	if (!index || !index->index) return 0;
	return (size_t)index->index->ntotal;
}

} /* extern "C" */
