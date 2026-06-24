#define KVS_VECTOR_INTERNAL

// Faiss pulls in C++ atomic helpers that conflict with C stdatomic macros
// from KVLite headers unless it is included first in this translation unit.
#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>

#ifndef _Atomic
#define _Atomic
#endif
#ifndef _Bool
#define _Bool bool
#endif

extern "C" {
#include "embedding_c_api.h"
#include "kvs_config.h"
#include "kvs_log.h"
#include "kvs_memory.h"
#include "kvs_owner_lock.h"
#include "kvs_vector.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

extern "C" {
extern kvs_config_t g_cfg;
extern kvs_vector_state_t g_vector_state;
pthread_rwlock_t g_kv_data_rwlock = PTHREAD_RWLOCK_INITIALIZER;
}

namespace {

struct QaRow {
    std::string question;
    std::string answer;
};

struct RunOptions {
    std::string mode;
    std::string dataset_path;
    std::string query_path;
    int topk = 10;
    int ef_search = 64;
    int hnsw_m = 16;
    int ef_construction = 200;
    int embedding_workers = 1;
    int intra_threads = 1;
    int inter_threads = 0;
    int limit_nb = 0;
    int limit_nq = 0;
};

struct Stats {
    std::vector<uint64_t> samples_us;

    void add(uint64_t us) { samples_us.push_back(us); }

    uint64_t total() const {
        return std::accumulate(samples_us.begin(), samples_us.end(), uint64_t{0});
    }

    uint64_t avg() const { return samples_us.empty() ? 0 : total() / samples_us.size(); }

    uint64_t percentile(double p) const {
        if (samples_us.empty()) return 0;
        std::vector<uint64_t> sorted = samples_us;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
        return sorted[idx];
    }

    uint64_t max() const {
        return samples_us.empty() ? 0 : *std::max_element(samples_us.begin(), samples_us.end());
    }

    double qps() const {
        uint64_t t = total();
        if (t == 0) return 0.0;
        return static_cast<double>(samples_us.size()) / (static_cast<double>(t) / 1000000.0);
    }
};

struct EmbeddingStageDelta {
    uint64_t total_us = 0;
    uint64_t tokenize_us = 0;
    uint64_t onnx_run_us = 0;
    uint64_t normalize_us = 0;
};

struct SearchStageDelta {
    uint64_t total_us = 0;
    uint64_t ann_core_us = 0;
    uint64_t export_us = 0;
};

struct CacheDelta {
    uint64_t enabled = 0;
    uint64_t lookup_count = 0;
    uint64_t hit_count = 0;
    uint64_t miss_count = 0;
    uint64_t lookup_total_us = 0;
    uint64_t insert_count = 0;
    uint64_t insert_total_us = 0;
    uint64_t evict_count = 0;
    uint64_t memory_bytes = 0;
    uint64_t saved_onnx_us = 0;

    double hit_rate() const {
        return lookup_count == 0 ? 0.0 :
            static_cast<double>(hit_count) * 100.0 / static_cast<double>(lookup_count);
    }
};

struct RecallSummary {
    double recall1 = 0.0;
    double recall5 = 0.0;
    double recall10 = 0.0;
};

static uint64_t now_us() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

static void l2_normalize(float *vec, int dim) {
    double norm = 0.0;
    for (int i = 0; i < dim; ++i) norm += static_cast<double>(vec[i]) * static_cast<double>(vec[i]);
    norm = std::sqrt(norm);
    if (norm <= 0.0) return;
    double inv = 1.0 / norm;
    for (int i = 0; i < dim; ++i) vec[i] = static_cast<float>(static_cast<double>(vec[i]) * inv);
}

static std::vector<QaRow> load_tsv(const std::string &path, int limit) {
    std::ifstream in(path);
    std::vector<QaRow> rows;
    std::string line;

    if (!in.is_open()) {
        throw std::runtime_error("failed to open " + path);
    }

    while (std::getline(in, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        QaRow row;
        row.question = line.substr(0, tab);
        row.answer = line.substr(tab + 1);
        rows.push_back(std::move(row));
        if (limit > 0 && static_cast<int>(rows.size()) >= limit) break;
    }

    return rows;
}

static void print_stage_line(const std::string &mode,
                             const std::string &stage,
                             const Stats &stats,
                             uint64_t total_avg_us) {
    double share = total_avg_us > 0 ? (100.0 * static_cast<double>(stats.avg()) / static_cast<double>(total_avg_us)) : 0.0;
    std::printf(
        "STAGE|mode=%s|stage=%s|count=%zu|avg_us=%llu|p50_us=%llu|p95_us=%llu|p99_us=%llu|max_us=%llu|qps=%.2f|share_pct=%.2f\n",
        mode.c_str(),
        stage.c_str(),
        stats.samples_us.size(),
        static_cast<unsigned long long>(stats.avg()),
        static_cast<unsigned long long>(stats.percentile(0.50)),
        static_cast<unsigned long long>(stats.percentile(0.95)),
        static_cast<unsigned long long>(stats.percentile(0.99)),
        static_cast<unsigned long long>(stats.max()),
        stats.qps(),
        share
    );
}

static kvs_vector_embedding_runtime_stats_t runtime_stats_snapshot() {
    kvs_vector_embedding_runtime_stats_t stats;
    std::memset(&stats, 0, sizeof(stats));
    kvs_vector_embedding_runtime_get_stats(&stats);
    return stats;
}

static kvs_vector_vset_commit_stats_t vector_stats_snapshot() {
    kvs_vector_vset_commit_stats_t stats;
    std::memset(&stats, 0, sizeof(stats));
    kvs_vector_vset_commit_stats_get(&stats);
    return stats;
}

static CacheDelta cache_delta(const kvs_vector_embedding_runtime_stats_t &before,
                              const kvs_vector_embedding_runtime_stats_t &after) {
    CacheDelta out;
    out.enabled = after.embedding_cache_enabled;
    out.lookup_count = after.embedding_cache_lookup_count - before.embedding_cache_lookup_count;
    out.hit_count = after.embedding_cache_hit_count - before.embedding_cache_hit_count;
    out.miss_count = after.embedding_cache_miss_count - before.embedding_cache_miss_count;
    out.lookup_total_us = after.embedding_cache_lookup_total_us - before.embedding_cache_lookup_total_us;
    out.insert_count = after.embedding_cache_insert_count - before.embedding_cache_insert_count;
    out.insert_total_us = after.embedding_cache_insert_total_us - before.embedding_cache_insert_total_us;
    out.evict_count = after.embedding_cache_evict_count - before.embedding_cache_evict_count;
    out.memory_bytes = after.embedding_cache_memory_bytes;
    out.saved_onnx_us = after.embedding_cache_saved_onnx_us - before.embedding_cache_saved_onnx_us;
    return out;
}

static void print_cache_line(const std::string &mode, const CacheDelta &cache) {
    std::printf(
        "CACHE|mode=%s|enabled=%llu|lookup_count=%llu|hit_count=%llu|miss_count=%llu|hit_rate=%.2f|lookup_avg_us=%llu|insert_count=%llu|insert_avg_us=%llu|evict_count=%llu|memory_bytes=%llu|saved_onnx_us=%llu\n",
        mode.c_str(),
        static_cast<unsigned long long>(cache.enabled),
        static_cast<unsigned long long>(cache.lookup_count),
        static_cast<unsigned long long>(cache.hit_count),
        static_cast<unsigned long long>(cache.miss_count),
        cache.hit_rate(),
        static_cast<unsigned long long>(cache.lookup_count > 0 ? cache.lookup_total_us / cache.lookup_count : 0),
        static_cast<unsigned long long>(cache.insert_count),
        static_cast<unsigned long long>(cache.insert_count > 0 ? cache.insert_total_us / cache.insert_count : 0),
        static_cast<unsigned long long>(cache.evict_count),
        static_cast<unsigned long long>(cache.memory_bytes),
        static_cast<unsigned long long>(cache.saved_onnx_us)
    );
}

static void init_kvlite(const RunOptions &opts, const char *index_type, bool enable_cache = false) {
    kvs_config_init_default(&g_cfg);
    g_cfg.mem_backend = KVS_MEM_BACKEND_SYSTEM;
    std::snprintf(g_cfg.log_level, sizeof(g_cfg.log_level), "%s", "error");
    g_cfg.log_async = 0;
    g_cfg.log_file[0] = '\0';
    std::snprintf(g_cfg.embedding_provider, sizeof(g_cfg.embedding_provider), "%s", "local_c");
    std::snprintf(g_cfg.embedding_model_path, sizeof(g_cfg.embedding_model_path), "%s", "models/bge-small-zh-v1.5/model.onnx");
    g_cfg.embedding_dim = 512;
    g_cfg.embedding_num_threads = opts.intra_threads;
    g_cfg.embedding_inter_threads = opts.inter_threads;
    g_cfg.embedding_max_text_len = 512;
    std::snprintf(g_cfg.embedding_ort_graph_opt_level, sizeof(g_cfg.embedding_ort_graph_opt_level), "%s", "basic");
    std::snprintf(g_cfg.embedding_ort_execution_mode, sizeof(g_cfg.embedding_ort_execution_mode), "%s", "sequential");
    g_cfg.embedding_worker_pool_enabled = opts.embedding_workers > 1 ? 1 : 0;
    g_cfg.embedding_worker_threads = opts.embedding_workers;
    g_cfg.embedding_prepare_queue_capacity = 1024;
    g_cfg.embedding_worker_queue_capacity = 1024;
    g_cfg.embedding_per_thread_runtime_enabled = 0;
    g_cfg.embedding_per_thread_runtime_fallback_mutex = 1;
    g_cfg.embedding_shared_ort_env_enabled = 1;
    g_cfg.embedding_batch_enabled = 0;
    g_cfg.embedding_cache_enabled = enable_cache ? 1 : 0;
    g_cfg.embedding_cache_capacity = 10000;
    g_cfg.embedding_cache_ttl_ms = 3600000;
    std::snprintf(g_cfg.embedding_cache_policy, sizeof(g_cfg.embedding_cache_policy), "%s", "lru");
    std::snprintf(g_cfg.vector_index_type, sizeof(g_cfg.vector_index_type), "%s", index_type);
    std::snprintf(g_cfg.vector_metric, sizeof(g_cfg.vector_metric), "%s", "ip");
    g_cfg.vector_hnsw_m = opts.hnsw_m;
    g_cfg.vector_hnsw_ef_construction = opts.ef_construction;
    g_cfg.vector_hnsw_ef_search = opts.ef_search;

    kvs_log_init(NULL, KVS_LOG_LEVEL_ERROR, 0);
    mem_init();

    if (kvs_vector_init() != KVS_VECTOR_OK) {
        throw std::runtime_error("kvs_vector_init failed");
    }
}

static void destroy_kvlite() {
    kvs_vector_destroy();
    mem_destroy();
    kvs_log_shutdown();
}

static EmbeddingHandle *create_embedding_handle(const RunOptions &opts) {
    EmbeddingConfig cfg;
    EmbeddingHandle *handle = NULL;
    kvs_config_init_default(&g_cfg);
    std::snprintf(g_cfg.embedding_ort_graph_opt_level, sizeof(g_cfg.embedding_ort_graph_opt_level), "%s", "basic");
    std::snprintf(g_cfg.embedding_ort_execution_mode, sizeof(g_cfg.embedding_ort_execution_mode), "%s", "sequential");
    g_cfg.embedding_inter_threads = opts.inter_threads;
    g_cfg.embedding_shared_ort_env_enabled = 1;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.model_path = "models/bge-small-zh-v1.5/model.onnx";
    cfg.embedding_dim = 512;
    cfg.num_threads = opts.intra_threads;
    cfg.max_text_len = 512;
    cfg.normalize = 0;
    if (embedding_create(&cfg, &handle) != EMBEDDING_OK || !handle) {
        throw std::runtime_error("embedding_create failed");
    }
    return handle;
}

static std::vector<float> encode_texts_direct(const RunOptions &opts,
                                              const std::vector<std::string> &texts,
                                              Stats *embed_total,
                                              Stats *tokenize,
                                              Stats *onnx_run,
                                              Stats *normalize) {
    EmbeddingHandle *handle = create_embedding_handle(opts);
    std::vector<float> out(texts.size() * 512);

    for (size_t i = 0; i < texts.size(); ++i) {
        EmbeddingRuntimeResourceStats before{};
        EmbeddingRuntimeResourceStats after{};
        uint64_t t0 = now_us();
        embedding_runtime_get_resource_stats(&before);
        if (embedding_encode(handle, texts[i].c_str(), texts[i].size(), out.data() + i * 512, 512) != EMBEDDING_OK) {
            std::string err = embedding_last_error(handle);
            embedding_destroy(handle);
            throw std::runtime_error("embedding_encode failed: " + err);
        }
        embedding_runtime_get_resource_stats(&after);
        uint64_t t1 = now_us();
        uint64_t norm0 = now_us();
        l2_normalize(out.data() + i * 512, 512);
        uint64_t norm1 = now_us();

        if (embed_total) embed_total->add(t1 - t0);
        if (tokenize) tokenize->add(after.tokenizer_total_us - before.tokenizer_total_us);
        if (onnx_run) onnx_run->add(after.ort_run_total_us - before.ort_run_total_us);
        if (normalize) normalize->add(norm1 - norm0);
    }

    embedding_destroy(handle);
    return out;
}

static void populate_with_cached_embeddings(const std::vector<QaRow> &dataset,
                                            const std::vector<float> &embeddings,
                                            int dim) {
    for (size_t i = 0; i < dataset.size(); ++i) {
        kvs_vector_set_prepared_t prepared;
        kvs_vector_set_prepared_reset(&prepared);
        int ret = kvs_vector_set_prepare_with_embedding(
            dataset[i].question.c_str(), dataset[i].question.size(),
            dataset[i].answer.c_str(), dataset[i].answer.size(),
            NULL, 0,
            embeddings.data() + i * dim, dim,
            &prepared
        );
        if (ret != KVS_VECTOR_OK) {
            throw std::runtime_error("kvs_vector_set_prepare_with_embedding failed");
        }
        kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "populate_cached");
        ret = kvs_vector_set_commit_prepared(&prepared);
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "populate_cached");
        kvs_vector_set_prepared_destroy(&prepared);
        if (ret != KVS_VECTOR_OK) {
            throw std::runtime_error("kvs_vector_set_commit_prepared failed");
        }
    }
}

static std::vector<std::vector<int>> exact_topk(const std::vector<float> &base,
                                                const std::vector<float> &queries,
                                                int nb,
                                                int nq,
                                                int dim,
                                                int topk) {
    std::vector<std::vector<int>> gt(nq, std::vector<int>(topk, -1));
    for (int qi = 0; qi < nq; ++qi) {
        std::vector<std::pair<float, int>> scores;
        scores.reserve(nb);
        const float *q = queries.data() + static_cast<size_t>(qi) * dim;
        for (int bi = 0; bi < nb; ++bi) {
            const float *b = base.data() + static_cast<size_t>(bi) * dim;
            float score = 0.0f;
            for (int d = 0; d < dim; ++d) score += q[d] * b[d];
            scores.push_back({score, bi});
        }
        std::partial_sort(scores.begin(), scores.begin() + std::min(topk, nb), scores.end(),
                          [](const auto &lhs, const auto &rhs) { return lhs.first > rhs.first; });
        for (int k = 0; k < topk && k < nb; ++k) gt[qi][k] = scores[k].second;
    }
    return gt;
}

static RecallSummary compute_recall(const std::vector<std::vector<int>> &gt,
                                    const std::vector<std::vector<int>> &pred) {
    RecallSummary out;
    auto recall_at = [&](int k) {
        double hit = 0.0;
        for (size_t i = 0; i < gt.size(); ++i) {
            int denom = std::min(k, static_cast<int>(gt[i].size()));
            int local_hit = 0;
            for (int j = 0; j < denom; ++j) {
                if (std::find(pred[i].begin(), pred[i].begin() + std::min(k, static_cast<int>(pred[i].size())), gt[i][j]) !=
                    pred[i].begin() + std::min(k, static_cast<int>(pred[i].size()))) {
                    local_hit++;
                }
            }
            hit += static_cast<double>(local_hit) / static_cast<double>(denom);
        }
        return gt.empty() ? 0.0 : hit / static_cast<double>(gt.size());
    };
    out.recall1 = recall_at(1);
    out.recall5 = recall_at(5);
    out.recall10 = recall_at(10);
    return out;
}

static void run_embed_only(const RunOptions &opts, const std::vector<QaRow> &dataset) {
    std::vector<std::string> texts;
    texts.reserve(dataset.size());
    for (const auto &row : dataset) texts.push_back(row.question);

    Stats total, tokenize, onnx_run, normalize;
    (void)encode_texts_direct(opts, texts, &total, &tokenize, &onnx_run, &normalize);

    std::printf("SUMMARY|mode=embed_only|count=%zu|dim=512|workers=%d|intra_threads=%d\n",
                texts.size(), opts.embedding_workers, opts.intra_threads);
    print_stage_line(opts.mode, "tokenize", tokenize, total.avg());
    print_stage_line(opts.mode, "onnx_run", onnx_run, total.avg());
    print_stage_line(opts.mode, "normalize", normalize, total.avg());
    print_stage_line(opts.mode, "embedding_total", total, total.avg());
}

static void run_vset_text_onnx(const RunOptions &opts, const std::vector<QaRow> &dataset) {
    init_kvlite(opts, "hnsw");

    Stats total, embedding_total, onnx_run, tokenize, ann_add, record_commit;
    for (const auto &row : dataset) {
        kvs_vector_set_prepared_t prepared;
        kvs_vector_set_prepared_reset(&prepared);
        auto rt_before = runtime_stats_snapshot();
        uint64_t prep0 = now_us();
        int ret = kvs_vector_set_prepare(
            row.question.c_str(), row.question.size(),
            row.answer.c_str(), row.answer.size(),
            NULL, 0, &prepared
        );
        uint64_t prep1 = now_us();
        auto rt_after = runtime_stats_snapshot();
        if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_set_prepare failed");

        auto vs_before = vector_stats_snapshot();
        uint64_t c0 = now_us();
        kvs_owner_rwlock_wrlock(&g_kv_data_rwlock, "vset_text_onnx");
        ret = kvs_vector_set_commit_prepared(&prepared);
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "vset_text_onnx");
        uint64_t c1 = now_us();
        auto vs_after = vector_stats_snapshot();
        kvs_vector_set_prepared_destroy(&prepared);
        if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_set_commit_prepared failed");

        embedding_total.add(prep1 - prep0);
        total.add((prep1 - prep0) + (c1 - c0));
        tokenize.add(rt_after.tokenizer_total_us - rt_before.tokenizer_total_us);
        onnx_run.add(rt_after.ort_run_total_us - rt_before.ort_run_total_us);
        record_commit.add(c1 - c0);
        ann_add.add(vs_after.vset_backend_add_total_us - vs_before.vset_backend_add_total_us);
    }

    std::printf("SUMMARY|mode=vset_text_onnx|count=%zu|dim=512|topk=%d\n", dataset.size(), opts.topk);
    print_stage_line(opts.mode, "tokenize", tokenize, total.avg());
    print_stage_line(opts.mode, "onnx_run", onnx_run, total.avg());
    print_stage_line(opts.mode, "embedding_total", embedding_total, total.avg());
    print_stage_line(opts.mode, "record_commit", record_commit, total.avg());
    print_stage_line(opts.mode, "ann_add", ann_add, total.avg());
    print_stage_line(opts.mode, "total", total, total.avg());

    destroy_kvlite();
}

static void run_vsearch_text_onnx(const RunOptions &opts,
                                  const std::vector<QaRow> &dataset,
                                  const std::vector<QaRow> &queries) {
    std::vector<std::string> dataset_texts;
    for (const auto &row : dataset) dataset_texts.push_back(row.question);
    auto dataset_embeddings = encode_texts_direct(opts, dataset_texts, nullptr, nullptr, nullptr, nullptr);

    init_kvlite(opts, "hnsw");
    populate_with_cached_embeddings(dataset, dataset_embeddings, 512);

    Stats total, embedding_total, onnx_run, tokenize, ann_search, candidate_lookup;
    std::vector<kvs_vector_search_result_t> results(static_cast<size_t>(opts.topk));

    for (const auto &query : queries) {
        kvs_vector_search_prepared_t prepared;
        kvs_vector_search_prepared_reset(&prepared);
        auto rt_before = runtime_stats_snapshot();
        uint64_t p0 = now_us();
        int ret = kvs_vector_search_prepare(query.question.c_str(), query.question.size(), NULL, 0, &prepared);
        uint64_t p1 = now_us();
        auto rt_after = runtime_stats_snapshot();
        if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_search_prepare failed");

        auto vs_before = vector_stats_snapshot();
        size_t results_count = 0;
        uint64_t s0 = now_us();
        kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "vsearch_text_onnx");
        ret = kvs_vector_search_with_prepared(&prepared, opts.topk, -1.0f, results.data(), results.size(), &results_count);
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "vsearch_text_onnx");
        uint64_t s1 = now_us();
        auto vs_after = vector_stats_snapshot();
        kvs_vector_search_prepared_destroy(&prepared);
        if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_search_with_prepared failed");

        embedding_total.add(p1 - p0);
        total.add((p1 - p0) + (s1 - s0));
        tokenize.add(rt_after.tokenizer_total_us - rt_before.tokenizer_total_us);
        onnx_run.add(rt_after.ort_run_total_us - rt_before.ort_run_total_us);
        ann_search.add(vs_after.hnsw_search_core_total_us - vs_before.hnsw_search_core_total_us);
        candidate_lookup.add(vs_after.hnsw_search_export_total_us - vs_before.hnsw_search_export_total_us);
    }

    std::printf("SUMMARY|mode=vsearch_text_onnx|count=%zu|dim=512|topk=%d\n", queries.size(), opts.topk);
    print_stage_line(opts.mode, "tokenize", tokenize, total.avg());
    print_stage_line(opts.mode, "onnx_run", onnx_run, total.avg());
    print_stage_line(opts.mode, "embedding_total", embedding_total, total.avg());
    print_stage_line(opts.mode, "ann_search", ann_search, total.avg());
    print_stage_line(opts.mode, "candidate_lookup", candidate_lookup, total.avg());
    print_stage_line(opts.mode, "total", total, total.avg());

    destroy_kvlite();
}

static std::vector<std::vector<int>> run_kvlite_ann_mode(const RunOptions &opts,
                                                         const std::vector<QaRow> &dataset,
                                                         const std::vector<float> &dataset_embeddings,
                                                         const std::vector<QaRow> &queries,
                                                         const std::vector<float> &query_embeddings,
                                                         const char *index_type,
                                                         const char *mode_name) {
    std::unordered_map<std::string, int> question_to_index;
    for (size_t i = 0; i < dataset.size(); ++i) {
        question_to_index.emplace(dataset[i].question, static_cast<int>(i));
    }

    init_kvlite(opts, index_type);
    populate_with_cached_embeddings(dataset, dataset_embeddings, 512);

    Stats total, ann_core, export_stage;
    std::vector<std::vector<int>> predicted;
    predicted.reserve(queries.size());

    auto add_stats = vector_stats_snapshot();
    uint64_t ann_add_total_us = (std::strcmp(index_type, "hnsw") == 0 ?
        add_stats.vset_backend_add_total_us :
        add_stats.vset_flat_index_update_total_us);
    uint64_t ann_add_avg_us = dataset.empty() ? 0 : ann_add_total_us / dataset.size();
    double ann_add_qps = ann_add_total_us == 0 ? 0.0 :
        static_cast<double>(dataset.size()) / (static_cast<double>(ann_add_total_us) / 1000000.0);

    for (size_t i = 0; i < queries.size(); ++i) {
        kvs_vector_search_prepared_t prepared;
        kvs_vector_search_prepared_reset(&prepared);
        int ret = kvs_vector_search_prepare_with_embedding(
            queries[i].question.c_str(), queries[i].question.size(),
            NULL, 0, query_embeddings.data() + i * 512, 512, &prepared
        );
        if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_search_prepare_with_embedding failed");

        auto before = vector_stats_snapshot();
        std::vector<kvs_vector_search_result_t> results(static_cast<size_t>(opts.topk));
        size_t results_count = 0;
        uint64_t s0 = now_us();
        kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, mode_name);
        ret = kvs_vector_search_with_prepared(&prepared, opts.topk, -1.0f, results.data(), results.size(), &results_count);
        kvs_owner_rwlock_unlock(&g_kv_data_rwlock, mode_name);
        uint64_t s1 = now_us();
        auto after = vector_stats_snapshot();
        kvs_vector_search_prepared_destroy(&prepared);
        if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_search_with_prepared failed");

        total.add(s1 - s0);
        if (std::strcmp(index_type, "hnsw") == 0) {
            ann_core.add(after.hnsw_search_core_total_us - before.hnsw_search_core_total_us);
            export_stage.add(after.hnsw_search_export_total_us - before.hnsw_search_export_total_us);
        } else {
            ann_core.add(after.flat_search_core_total_us - before.flat_search_core_total_us);
            export_stage.add(after.flat_search_export_total_us - before.flat_search_export_total_us);
        }

        std::vector<int> ids(opts.topk, -1);
        for (size_t k = 0; k < results_count && k < static_cast<size_t>(opts.topk); ++k) {
            auto it = question_to_index.find(results[k].question ? results[k].question : "");
            if (it != question_to_index.end()) {
                ids[k] = it->second;
            }
        }
        predicted.push_back(std::move(ids));
    }

    std::printf("SUMMARY|mode=%s|count=%zu|dim=512|topk=%d|ef_search=%d|index=%s|ann_add_total_us=%llu|ann_add_avg_us=%llu|ann_add_qps=%.2f\n",
                mode_name, queries.size(), opts.topk, opts.ef_search, index_type,
                static_cast<unsigned long long>(ann_add_total_us),
                static_cast<unsigned long long>(ann_add_avg_us),
                ann_add_qps);
    print_stage_line(mode_name, "ann_search", ann_core, total.avg());
    print_stage_line(mode_name, "candidate_lookup", export_stage, total.avg());
    print_stage_line(mode_name, "total", total, total.avg());

    destroy_kvlite();
    return predicted;
}

static void run_embed_cache_mode(const RunOptions &opts,
                                 const std::vector<QaRow> &dataset,
                                 bool warm_before_measure) {
    init_kvlite(opts, "hnsw", true);

    std::vector<std::string> texts;
    texts.reserve(dataset.size());
    for (const auto &row : dataset) texts.push_back(row.question);

    auto run_once = [&](Stats *total, Stats *tokenize, Stats *onnx_run) {
        for (const auto &text : texts) {
            auto before = runtime_stats_snapshot();
            uint64_t t0 = now_us();
            float *embedding = nullptr;
            int dim = 0;
            int ret = kvs_vector_embedding_generate(text.c_str(), text.size(), &embedding, &dim);
            uint64_t t1 = now_us();
            auto after = runtime_stats_snapshot();
            if (ret != KVS_VECTOR_OK || !embedding || dim != 512) {
                if (embedding) kvs_free(embedding);
                throw std::runtime_error("kvs_vector_embedding_generate failed");
            }
            if (total) total->add(t1 - t0);
            if (tokenize) tokenize->add(after.tokenizer_total_us - before.tokenizer_total_us);
            if (onnx_run) onnx_run->add(after.ort_run_total_us - before.ort_run_total_us);
            kvs_free(embedding);
        }
    };

    if (warm_before_measure) {
        run_once(nullptr, nullptr, nullptr);
    }

    Stats total, tokenize, onnx_run;
    auto runtime_before = runtime_stats_snapshot();
    run_once(&total, &tokenize, &onnx_run);
    auto runtime_after = runtime_stats_snapshot();
    CacheDelta cache = cache_delta(runtime_before, runtime_after);

    std::printf("SUMMARY|mode=%s|count=%zu|dim=512|workers=%d|intra_threads=%d\n",
                opts.mode.c_str(), texts.size(), opts.embedding_workers, opts.intra_threads);
    print_stage_line(opts.mode, "tokenize", tokenize, total.avg());
    print_stage_line(opts.mode, "onnx_run", onnx_run, total.avg());
    print_stage_line(opts.mode, "embedding_total", total, total.avg());
    print_cache_line(opts.mode, cache);

    destroy_kvlite();
}

static void run_vsearch_text_cache_mode(const RunOptions &opts,
                                        const std::vector<QaRow> &dataset,
                                        const std::vector<QaRow> &queries,
                                        bool warm_before_measure) {
    std::vector<std::string> dataset_texts;
    for (const auto &row : dataset) dataset_texts.push_back(row.question);
    auto dataset_embeddings = encode_texts_direct(opts, dataset_texts, nullptr, nullptr, nullptr, nullptr);

    init_kvlite(opts, "hnsw", true);
    populate_with_cached_embeddings(dataset, dataset_embeddings, 512);

    std::vector<kvs_vector_search_result_t> results(static_cast<size_t>(opts.topk));

    auto run_once = [&](Stats *total,
                        Stats *embedding_total,
                        Stats *onnx_run,
                        Stats *tokenize,
                        Stats *ann_search,
                        Stats *candidate_lookup) {
        for (const auto &query : queries) {
            kvs_vector_search_prepared_t prepared;
            kvs_vector_search_prepared_reset(&prepared);
            auto rt_before = runtime_stats_snapshot();
            uint64_t p0 = now_us();
            int ret = kvs_vector_search_prepare(query.question.c_str(), query.question.size(), NULL, 0, &prepared);
            uint64_t p1 = now_us();
            auto rt_after = runtime_stats_snapshot();
            if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_search_prepare failed");

            auto vs_before = vector_stats_snapshot();
            size_t results_count = 0;
            uint64_t s0 = now_us();
            kvs_owner_rwlock_rdlock(&g_kv_data_rwlock, "vsearch_text_cache");
            ret = kvs_vector_search_with_prepared(&prepared, opts.topk, -1.0f, results.data(), results.size(), &results_count);
            kvs_owner_rwlock_unlock(&g_kv_data_rwlock, "vsearch_text_cache");
            uint64_t s1 = now_us();
            auto vs_after = vector_stats_snapshot();
            kvs_vector_search_prepared_destroy(&prepared);
            if (ret != KVS_VECTOR_OK) throw std::runtime_error("kvs_vector_search_with_prepared failed");

            if (embedding_total) embedding_total->add(p1 - p0);
            if (total) total->add((p1 - p0) + (s1 - s0));
            if (tokenize) tokenize->add(rt_after.tokenizer_total_us - rt_before.tokenizer_total_us);
            if (onnx_run) onnx_run->add(rt_after.ort_run_total_us - rt_before.ort_run_total_us);
            if (ann_search) ann_search->add(vs_after.hnsw_search_core_total_us - vs_before.hnsw_search_core_total_us);
            if (candidate_lookup) candidate_lookup->add(vs_after.hnsw_search_export_total_us - vs_before.hnsw_search_export_total_us);
        }
    };

    if (warm_before_measure) {
        run_once(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    Stats total, embedding_total, onnx_run, tokenize, ann_search, candidate_lookup;
    auto runtime_before = runtime_stats_snapshot();
    run_once(&total, &embedding_total, &onnx_run, &tokenize, &ann_search, &candidate_lookup);
    auto runtime_after = runtime_stats_snapshot();
    CacheDelta cache = cache_delta(runtime_before, runtime_after);

    std::printf("SUMMARY|mode=%s|count=%zu|dim=512|topk=%d\n", opts.mode.c_str(), queries.size(), opts.topk);
    print_stage_line(opts.mode, "tokenize", tokenize, total.avg());
    print_stage_line(opts.mode, "onnx_run", onnx_run, total.avg());
    print_stage_line(opts.mode, "embedding_total", embedding_total, total.avg());
    print_stage_line(opts.mode, "ann_search", ann_search, total.avg());
    print_stage_line(opts.mode, "candidate_lookup", candidate_lookup, total.avg());
    print_stage_line(opts.mode, "total", total, total.avg());
    print_cache_line(opts.mode, cache);

    destroy_kvlite();
}

static void run_faiss_mode(const RunOptions &opts,
                           const std::vector<float> &dataset_embeddings,
                           const std::vector<float> &query_embeddings,
                           int nb,
                           int nq,
                           const std::vector<std::vector<int>> &gt,
                           bool use_hnsw) {
    Stats add_stats;
    Stats search_stats;
    std::vector<std::vector<int>> predicted(nq, std::vector<int>(opts.topk, -1));

    faiss::Index *index = nullptr;
    if (use_hnsw) {
        auto *hnsw = new faiss::IndexHNSWFlat(512, opts.hnsw_m, faiss::METRIC_INNER_PRODUCT);
        hnsw->hnsw.efConstruction = opts.ef_construction;
        hnsw->hnsw.efSearch = opts.ef_search;
        index = hnsw;
    } else {
        index = new faiss::IndexFlatIP(512);
    }

    for (int i = 0; i < nb; ++i) {
        uint64_t t0 = now_us();
        index->add(1, dataset_embeddings.data() + static_cast<size_t>(i) * 512);
        uint64_t t1 = now_us();
        add_stats.add(t1 - t0);
    }

    std::vector<faiss::idx_t> labels(static_cast<size_t>(opts.topk));
    std::vector<float> distances(static_cast<size_t>(opts.topk));
    for (int i = 0; i < nq; ++i) {
        uint64_t t0 = now_us();
        index->search(1, query_embeddings.data() + static_cast<size_t>(i) * 512, opts.topk, distances.data(), labels.data());
        uint64_t t1 = now_us();
        search_stats.add(t1 - t0);
        for (int k = 0; k < opts.topk; ++k) predicted[i][k] = static_cast<int>(labels[k]);
    }

    RecallSummary recall = compute_recall(gt, predicted);
    std::printf(
        "ANN|backend=Faiss|index=%s|nb=%d|nq=%d|dim=512|ef_search=%d|add_qps=%.2f|search_qps=%.2f|p95_us=%llu|recall1=%.4f|recall5=%.4f|recall10=%.4f\n",
        use_hnsw ? "HNSW" : "FlatIP",
        nb, nq, opts.ef_search,
        add_stats.qps(), search_stats.qps(),
        static_cast<unsigned long long>(search_stats.percentile(0.95)),
        recall.recall1, recall.recall5, recall.recall10
    );

    delete index;
}

static RunOptions parse_args(int argc, char **argv) {
    if (argc < 3) {
        throw std::runtime_error(
            "usage: vcmd_onnx_faiss_bench <mode> <dataset.tsv> [queries.tsv] "
            "[--topk N] [--ef-search N] [--workers N] [--intra-threads N] [--nb N] [--nq N]\n"
            "modes: embed_only, vset_text_onnx, vsearch_text_onnx, vsearch_vec_from_onnx_cache,\n"
            "       kvlite_ann_flat_from_onnx, kvlite_ann_hnsw_from_onnx, faiss_flat_from_onnx,\n"
            "       faiss_hnsw_from_onnx, embed_cache_cold, embed_cache_hot,\n"
            "       vsearch_text_cache_cold, vsearch_text_cache_hot"
        );
    }

    RunOptions opts;
    opts.mode = argv[1];
    opts.dataset_path = argv[2];
    int argi = 3;
    if (argi < argc && std::strncmp(argv[argi], "--", 2) != 0) {
        opts.query_path = argv[argi++];
    }

    while (argi < argc) {
        std::string key = argv[argi++];
        if (argi >= argc) break;
        int value = std::atoi(argv[argi++]);
        if (key == "--topk") opts.topk = value;
        else if (key == "--ef-search") opts.ef_search = value;
        else if (key == "--workers") opts.embedding_workers = value;
        else if (key == "--intra-threads") opts.intra_threads = value;
        else if (key == "--inter-threads") opts.inter_threads = value;
        else if (key == "--nb") opts.limit_nb = value;
        else if (key == "--nq") opts.limit_nq = value;
    }

    return opts;
}

}  // namespace

int main(int argc, char **argv) {
    try {
        RunOptions opts = parse_args(argc, argv);
        std::vector<QaRow> dataset = load_tsv(opts.dataset_path, opts.limit_nb);
        std::vector<QaRow> queries = opts.query_path.empty() ? std::vector<QaRow>{} : load_tsv(opts.query_path, opts.limit_nq);

        std::printf("CONFIG|mode=%s|dataset=%s|queries=%s|nb=%zu|nq=%zu|topk=%d|ef_search=%d|workers=%d|intra_threads=%d\n",
                    opts.mode.c_str(), opts.dataset_path.c_str(), opts.query_path.c_str(),
                    dataset.size(), queries.size(), opts.topk, opts.ef_search,
                    opts.embedding_workers, opts.intra_threads);

        if (opts.mode == "embed_only") {
            run_embed_only(opts, dataset);
            return 0;
        }
        if (opts.mode == "embed_cache_cold") {
            run_embed_cache_mode(opts, dataset, false);
            return 0;
        }
        if (opts.mode == "embed_cache_hot") {
            run_embed_cache_mode(opts, dataset, true);
            return 0;
        }
        if (opts.mode == "vset_text_onnx") {
            run_vset_text_onnx(opts, dataset);
            return 0;
        }
        if (queries.empty()) {
            throw std::runtime_error("query dataset is required for this mode");
        }

        std::vector<std::string> dataset_texts;
        std::vector<std::string> query_texts;
        for (const auto &row : dataset) dataset_texts.push_back(row.question);
        for (const auto &row : queries) query_texts.push_back(row.question);

        auto dataset_embeddings = encode_texts_direct(opts, dataset_texts, nullptr, nullptr, nullptr, nullptr);
        auto query_embeddings = encode_texts_direct(opts, query_texts, nullptr, nullptr, nullptr, nullptr);
        auto gt = exact_topk(dataset_embeddings, query_embeddings, static_cast<int>(dataset.size()), static_cast<int>(queries.size()), 512, opts.topk);

        if (opts.mode == "vsearch_text_onnx") {
            run_vsearch_text_onnx(opts, dataset, queries);
            return 0;
        }
        if (opts.mode == "vsearch_text_cache_cold") {
            run_vsearch_text_cache_mode(opts, dataset, queries, false);
            return 0;
        }
        if (opts.mode == "vsearch_text_cache_hot") {
            run_vsearch_text_cache_mode(opts, dataset, queries, true);
            return 0;
        }
        if (opts.mode == "vsearch_vec_from_onnx_cache") {
            auto pred = run_kvlite_ann_mode(opts, dataset, dataset_embeddings, queries, query_embeddings, "hnsw", opts.mode.c_str());
            RecallSummary recall = compute_recall(gt, pred);
            std::printf("ANN|backend=KVLite|index=HNSW|nb=%zu|nq=%zu|dim=512|ef_search=%d|recall1=%.4f|recall5=%.4f|recall10=%.4f\n",
                        dataset.size(), queries.size(), opts.ef_search, recall.recall1, recall.recall5, recall.recall10);
            return 0;
        }
        if (opts.mode == "kvlite_ann_flat_from_onnx") {
            auto pred = run_kvlite_ann_mode(opts, dataset, dataset_embeddings, queries, query_embeddings, "flat", opts.mode.c_str());
            RecallSummary recall = compute_recall(gt, pred);
            std::printf("ANN|backend=KVLite|index=FlatIP|nb=%zu|nq=%zu|dim=512|ef_search=0|recall1=%.4f|recall5=%.4f|recall10=%.4f\n",
                        dataset.size(), queries.size(), recall.recall1, recall.recall5, recall.recall10);
            return 0;
        }
        if (opts.mode == "kvlite_ann_hnsw_from_onnx") {
            auto pred = run_kvlite_ann_mode(opts, dataset, dataset_embeddings, queries, query_embeddings, "hnsw", opts.mode.c_str());
            RecallSummary recall = compute_recall(gt, pred);
            std::printf("ANN|backend=KVLite|index=HNSW|nb=%zu|nq=%zu|dim=512|ef_search=%d|recall1=%.4f|recall5=%.4f|recall10=%.4f\n",
                        dataset.size(), queries.size(), opts.ef_search, recall.recall1, recall.recall5, recall.recall10);
            return 0;
        }
        if (opts.mode == "faiss_flat_from_onnx") {
            run_faiss_mode(opts, dataset_embeddings, query_embeddings, static_cast<int>(dataset.size()), static_cast<int>(queries.size()), gt, false);
            return 0;
        }
        if (opts.mode == "faiss_hnsw_from_onnx") {
            run_faiss_mode(opts, dataset_embeddings, query_embeddings, static_cast<int>(dataset.size()), static_cast<int>(queries.size()), gt, true);
            return 0;
        }

        throw std::runtime_error("unknown mode: " + opts.mode);
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "ERROR: %s\n", ex.what());
        return 1;
    }
}
