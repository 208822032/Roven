/*
 * faiss_baseline.cpp — Faiss ANN baseline for KVLite vector comparison.
 *
 * Supports: IndexFlatIP, IndexFlatL2, IndexHNSWFlat
 *
 * Build (requires Faiss installed):
 *   g++ -std=c++17 -O3 -o faiss_baseline faiss_baseline.cpp -lfaiss -lpthread -lm
 *
 * Usage:
 *   ./faiss_baseline <index_type> <nb> <dim> <nq> <topk> [M] [efConstruction] [efSearch]
 *
 *   index_type: flat_ip | flat_l2 | hnsw
 *
 * Output:
 *   CSV with add QPS, search QPS, p50/p95/p99, recall@k, RSS memory
 */

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/utils/distances.h>
#include <faiss/Index.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <vector>
#include <algorithm>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static long get_rss_kb() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return usage.ru_maxrss;
    }
    return -1;
}

static void rand_vec(float *v, int dim) {
    for (int i = 0; i < dim; i++) {
        v[i] = (float)((double)rand() / (double)RAND_MAX * 2.0 - 1.0);
    }
}

static void normalize(float *v, int dim) {
    double norm = 0.0;
    for (int i = 0; i < dim; i++) norm += (double)v[i] * (double)v[i];
    norm = sqrt(norm);
    if (norm > 0) {
        for (int i = 0; i < dim; i++) v[i] = (float)((double)v[i] / norm);
    }
}

static void print_stats(const char *label, const std::vector<uint64_t> &times,
                         uint64_t total_us, int count, double &qps_out) {
    std::vector<uint64_t> sorted = times;
    std::sort(sorted.begin(), sorted.end());

    uint64_t avg = total_us / (uint64_t)count;
    uint64_t p50 = sorted[(int)(count * 0.50)];
    uint64_t p95 = sorted[(int)(count * 0.95)];
    uint64_t p99 = sorted[(int)(count * 0.99)];
    double qps = (double)count / ((double)total_us / 1000000.0);
    qps_out = qps;

    printf("%s: avg=%lu us p50=%lu us p95=%lu us p99=%lu us max=%lu us QPS=%.1f\n",
           label,
           (unsigned long)avg, (unsigned long)p50,
           (unsigned long)p95, (unsigned long)p99,
           (unsigned long)sorted.back(), qps);
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr,
            "Usage: %s <index_type> <nb> <dim> <nq> <topk> [M] [efConstruction] [efSearch]\n"
            "\n"
            "  index_type: flat_ip | flat_l2 | hnsw\n"
            "  nb:         number of base vectors\n"
            "  dim:        vector dimension\n"
            "  nq:         number of query vectors\n"
            "  topk:       number of results per query\n"
            "  M:          HNSW M parameter (default: 16)\n"
            "  efConstruction: HNSW efConstruction (default: 200)\n"
            "  efSearch:  HNSW efSearch (default: 128)\n",
            argv[0]);
        return 1;
    }

    const char *index_type = argv[1];
    int nb = atoi(argv[2]);
    int dim = atoi(argv[3]);
    int nq = atoi(argv[4]);
    int topk = atoi(argv[5]);
    int M = argc >= 7 ? atoi(argv[6]) : 16;
    int efConstruction = argc >= 8 ? atoi(argv[7]) : 200;
    int efSearch = argc >= 9 ? atoi(argv[8]) : 128;

    printf("# Faiss Baseline Benchmark\n");
    printf("# index_type=%s nb=%d dim=%d nq=%d topk=%d\n",
           index_type, nb, dim, nq, topk);
    if (strcmp(index_type, "hnsw") == 0) {
        printf("# HNSW params: M=%d efConstruction=%d efSearch=%d\n",
               M, efConstruction, efSearch);
    }
    printf("# RSS_before_load=%ld KB\n", get_rss_kb());

    /* Build index */
    faiss::Index *index = nullptr;
    bool use_ip = false;

    if (strcmp(index_type, "flat_ip") == 0) {
        index = new faiss::IndexFlatIP(dim);
        use_ip = true;
    } else if (strcmp(index_type, "flat_l2") == 0) {
        index = new faiss::IndexFlatL2(dim);
    } else if (strcmp(index_type, "hnsw") == 0) {
        faiss::IndexHNSWFlat *hnsw = new faiss::IndexHNSWFlat(dim, M, faiss::METRIC_INNER_PRODUCT);
        hnsw->hnsw.efConstruction = efConstruction;
        hnsw->hnsw.efSearch = efSearch;
        index = hnsw;
        use_ip = true;
    } else {
        fprintf(stderr, "Unknown index_type: %s\n", index_type);
        return 1;
    }

    srand(42);

    /* Phase 1: Add vectors */
    std::vector<float> base_vectors(nb * dim);
    std::vector<uint64_t> add_times(nb);
    uint64_t add_total = 0;

    printf("\n# Phase 1: Adding %d vectors...\n", nb);
    for (int i = 0; i < nb; i++) {
        float *v = &base_vectors[i * dim];
        rand_vec(v, dim);
        if (use_ip) normalize(v, dim);

        uint64_t t0 = now_us();
        index->add(1, v);
        uint64_t t1 = now_us();
        add_times[i] = t1 - t0;
        add_total += add_times[i];
    }
    {
        double add_qps;
        print_stats("  Add   ", add_times, add_total, nb, add_qps);
        printf("# Add QPS: %.1f\n", add_qps);
    }

    /* Phase 2: Search */
    printf("\n# Phase 2: Searching %d queries...\n", nq);
    std::vector<float> query_vectors(nq * dim);
    std::vector<uint64_t> search_times(nq);
    std::vector<faiss::idx_t> labels(nq * topk);
    std::vector<float> distances(nq * topk);
    uint64_t search_total = 0;

    for (int i = 0; i < nq; i++) {
        float *q = &query_vectors[i * dim];
        rand_vec(q, dim);
        if (use_ip) normalize(q, dim);

        uint64_t t0 = now_us();
        index->search(1, q, topk, &distances[i * topk], &labels[i * topk]);
        uint64_t t1 = now_us();
        search_times[i] = t1 - t0;
        search_total += search_times[i];
    }
    {
        double search_qps;
        print_stats("  Search", search_times, search_total, nq, search_qps);
        printf("# Search QPS: %.1f\n", search_qps);
    }

    /* Phase 3: Ground truth recall (using flat IP for ground truth) */
    printf("\n# Phase 3: Computing recall@%d...\n", topk);
    faiss::IndexFlatIP ground_truth(dim);
    ground_truth.add(nb, base_vectors.data());

    std::vector<faiss::idx_t> gt_labels(nq * topk);
    std::vector<float> gt_distances(nq * topk);
    ground_truth.search(nq, query_vectors.data(), topk,
                        gt_distances.data(), gt_labels.data());

    double total_recall = 0.0;
    for (int i = 0; i < nq; i++) {
        std::vector<faiss::idx_t> gt_set(gt_labels.begin() + i * topk,
                                          gt_labels.begin() + (i + 1) * topk);
        int hits = 0;
        for (int j = 0; j < topk; j++) {
            faiss::idx_t pred = labels[i * topk + j];
            if (pred >= 0 &&
                std::find(gt_set.begin(), gt_set.end(), pred) != gt_set.end()) {
                hits++;
            }
        }
        total_recall += (double)hits / (double)topk;
    }
    double recall_at_k = total_recall / (double)nq;
    printf("  recall@%d: %.4f\n", topk, recall_at_k);

    printf("\n# RSS_after=%ld KB\n", get_rss_kb());

    /* CSV output */
    printf("\n# --- FAISS_CSV ---\n");
    printf("# index_type,nb,dim,nq,topk,add_qps,search_qps,recall_at_k,rss_kb\n");
    {
        double add_qps = (double)nb / ((double)add_total / 1000000.0);
        double search_qps = (double)nq / ((double)search_total / 1000000.0);
        printf("FAISS_CSV,%s,%d,%d,%d,%d,%.1f,%.1f,%.4f,%ld\n",
               index_type, nb, dim, nq, topk,
               add_qps, search_qps, recall_at_k, get_rss_kb());
    }

    /* Detailed search percentiles */
    {
        std::vector<uint64_t> sorted = search_times;
        std::sort(sorted.begin(), sorted.end());
        printf("FAISS_SEARCH_PCT,%s,%d,%d,%d,%d,%lu,%lu,%lu\n",
               index_type, nb, dim, nq, topk,
               (unsigned long)sorted[(int)(nq * 0.50)],
               (unsigned long)sorted[(int)(nq * 0.95)],
               (unsigned long)sorted[(int)(nq * 0.99)]);
    }

    delete index;
    return 0;
}
