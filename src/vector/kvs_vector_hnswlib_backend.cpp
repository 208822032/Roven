#include "kvs_vector_hnswlib_backend.h"

#include <hnswlib/hnswlib.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <queue>

struct kvs_hnswlib_index_s {
	hnswlib::HierarchicalNSW<float> *index;
	size_t dim;
	size_t max_elements;
	size_t M;
	size_t ef_construction;
	size_t ef_search;
};

extern "C" {

kvs_hnswlib_index_t *kvs_hnswlib_index_create(
	size_t dim,
	size_t max_elements,
	size_t M,
	size_t ef_construction
) {
	if (dim == 0) {
		std::fprintf(stderr, "[hnswlib] create: dim=0\n");
		return nullptr;
	}
	if (max_elements == 0) max_elements = 16;
	if (M == 0) M = 16;
	if (ef_construction == 0) ef_construction = 200;

	try {
		auto *space = new hnswlib::InnerProductSpace((int)dim);
		auto *hnsw = new hnswlib::HierarchicalNSW<float>(
			space, max_elements, (int)M, (int)ef_construction);

		kvs_hnswlib_index_t *idx = (kvs_hnswlib_index_t *)
			std::malloc(sizeof(kvs_hnswlib_index_t));
		if (!idx) {
			delete hnsw;
			delete space;
			return nullptr;
		}
		idx->index = hnsw;
		idx->dim = dim;
		idx->max_elements = max_elements;
		idx->M = M;
		idx->ef_construction = ef_construction;
		idx->ef_search = ef_construction < 16 ? ef_construction : 16;
		hnsw->setEf((int)idx->ef_search);

		std::fprintf(stderr, "[hnswlib] created dim=%zu max_elements=%zu M=%zu ef_construction=%zu\n",
			dim, max_elements, M, ef_construction);
		return idx;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[hnswlib] create exception: %s\n", e.what());
		return nullptr;
	} catch (...) {
		std::fprintf(stderr, "[hnswlib] create unknown exception\n");
		return nullptr;
	}
}

int kvs_hnswlib_index_add(
	kvs_hnswlib_index_t *index,
	size_t record_index,
	const float *embedding,
	int embedding_dim
) {
	if (!index || !index->index || !embedding || embedding_dim <= 0) {
		std::fprintf(stderr, "[hnswlib] add: invalid args\n");
		return -1;
	}
	if ((size_t)embedding_dim != index->dim) {
		std::fprintf(stderr, "[hnswlib] add: dim mismatch idx=%zu in=%d\n",
			index->dim, embedding_dim);
		return -1;
	}

	try {
		index->index->addPoint((void *)embedding, (size_t)record_index);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[hnswlib] add exception: %s label=%zu\n",
			e.what(), record_index);
		return -1;
	} catch (...) {
		std::fprintf(stderr, "[hnswlib] add unknown exception label=%zu\n",
			record_index);
		return -1;
	}
	return 0;
}

int kvs_hnswlib_index_search(
	kvs_hnswlib_index_t *index,
	const float *query_embedding,
	int query_dim,
	int topk,
	size_t *out_labels,
	float *out_scores
) {
	if (!index || !index->index || !query_embedding || query_dim <= 0)
		return -1;
	if ((size_t)query_dim != index->dim) {
		std::fprintf(stderr, "[hnswlib] search: dim mismatch idx=%zu q=%d\n",
			index->dim, query_dim);
		return -1;
	}
	if (topk <= 0 || !out_labels || !out_scores)
		return -1;

	try {
		/* hnswlib searchKnn returns further-first priority_queue.
		 * Pop all into a vector, then reverse to get closer-first. */
		auto pq = index->index->searchKnn((void *)query_embedding, topk);
		int n = (int)pq.size();
		if (n > topk) n = topk;

		/* Collect in reverse (further-first → closer-last) */
		for (int i = n - 1; i >= 0; --i) {
			auto top = pq.top(); pq.pop();
			out_labels[i] = (size_t)top.second;
			/* InnerProductSpace: distance is 1.0 - inner_product.
			 * Convert back to inner product for KVLite larger-is-better */
			out_scores[i] = 1.0f - top.first;
		}
		return n;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[hnswlib] search exception: %s\n", e.what());
		return -1;
	} catch (...) {
		std::fprintf(stderr, "[hnswlib] search unknown exception\n");
		return -1;
	}
}

int kvs_hnswlib_index_reset(kvs_hnswlib_index_t *index) {
	if (!index || !index->index) return -1;
	try {
		/* hnswlib has no reset() — destroy and recreate */
		auto *old = index->index;
		auto *space = new hnswlib::InnerProductSpace((int)index->dim);
		auto *hnsw = new hnswlib::HierarchicalNSW<float>(
			space, index->max_elements, (int)index->M,
			(int)index->ef_construction);
		hnsw->setEf((int)index->ef_search);
		index->index = hnsw;
		delete old; /* old's space is owned by old */
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[hnswlib] reset exception: %s\n", e.what());
		return -1;
	} catch (...) {
		std::fprintf(stderr, "[hnswlib] reset unknown exception\n");
		return -1;
	}
	return 0;
}

void kvs_hnswlib_index_destroy(kvs_hnswlib_index_t *index) {
	if (!index) return;
	if (index->index) {
		/* HierarchicalNSW owns its space, deleted via destructor */
		delete index->index;
		index->index = nullptr;
	}
	std::free(index);
}

size_t kvs_hnswlib_index_ntotal(const kvs_hnswlib_index_t *index) {
	if (!index || !index->index) return 0;
	return (size_t)index->index->getCurrentElementCount();
}

size_t kvs_hnswlib_index_get_max_elements(const kvs_hnswlib_index_t *index) {
	if (!index) return 0;
	return index->max_elements;
}

int kvs_hnswlib_index_get_stats(
	const kvs_hnswlib_index_t *index,
	kvs_hnswlib_index_stats_t *stats
) {
	if (!index || !stats) return -1;
	std::memset(stats, 0, sizeof(*stats));
	stats->ntotal = kvs_hnswlib_index_ntotal(index);
	stats->max_elements = index->max_elements;
	stats->M = index->M;
	stats->ef_construction = index->ef_construction;
	stats->ef_search = index->ef_search;
	return 0;
}

void kvs_hnswlib_index_set_ef(kvs_hnswlib_index_t *index, size_t ef) {
	if (!index || !index->index || ef == 0) return;
	index->ef_search = ef;
	index->index->setEf((int)ef);
}

int kvs_hnswlib_index_resize(kvs_hnswlib_index_t *index, size_t new_max_elements) {
	if (!index || !index->index) return -1;
	if (new_max_elements <= index->max_elements) return 0;
	try {
		index->index->resizeIndex(new_max_elements);
		index->max_elements = new_max_elements;
		std::fprintf(stderr, "[hnswlib] resized to %zu\n", new_max_elements);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[hnswlib] resize exception: %s\n", e.what());
		return -1;
	} catch (...) {
		std::fprintf(stderr, "[hnswlib] resize unknown exception\n");
		return -1;
	}
	return 0;
}

} /* extern "C" */
