#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <queue>
#include <string>
#include <sys/resource.h>
#include <vector>

#include <hnswlib/hnswlib.h>
#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>

struct Args {
    std::string backend;
    std::string dataset_path;
    std::string queries_path;
    size_t n = 0;
    size_t nq = 0;
    size_t dim = 0;
    size_t topk = 10;
    size_t m = 32;
    size_t ef_construction = 256;
    size_t ef_search = 128;
};

static uint64_t now_us() {
    using clock = std::chrono::steady_clock;
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        clock::now().time_since_epoch()).count();
}

static long rss_kb() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) return usage.ru_maxrss;
    return -1;
}

static bool read_f32_file(const std::string& path, std::vector<float>& out, size_t expected) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.resize(expected);
    in.read(reinterpret_cast<char*>(out.data()), (std::streamsize)(expected * sizeof(float)));
    return in.good() || (in.eof() && (size_t)in.gcount() == expected * sizeof(float));
}

static float ip_score(const float* a, const float* b, size_t dim) {
    float s = 0.0f;
    for (size_t i = 0; i < dim; ++i) s += a[i] * b[i];
    return s;
}

static std::vector<size_t> exact_topk(const std::vector<float>& base, const float* q,
                                      size_t n, size_t dim, size_t topk) {
    using Item = std::pair<float, size_t>;
    auto cmp = [](const Item& lhs, const Item& rhs) { return lhs.first > rhs.first; };
    std::priority_queue<Item, std::vector<Item>, decltype(cmp)> heap(cmp);
    for (size_t i = 0; i < n; ++i) {
        float score = ip_score(&base[i * dim], q, dim);
        if (heap.size() < topk) {
            heap.emplace(score, i);
        } else if (score > heap.top().first) {
            heap.pop();
            heap.emplace(score, i);
        }
    }
    std::vector<Item> items;
    while (!heap.empty()) {
        items.push_back(heap.top());
        heap.pop();
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    std::vector<size_t> ids;
    ids.reserve(items.size());
    for (const auto& item : items) ids.push_back(item.second);
    return ids;
}

static void stats(const std::vector<uint64_t>& values, uint64_t* total, uint64_t* avg,
                  uint64_t* p50, uint64_t* p95, uint64_t* p99, uint64_t* maxv) {
    if (values.empty()) return;
    std::vector<uint64_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    uint64_t sum = 0;
    for (uint64_t v : values) sum += v;
    *total = sum;
    *avg = sum / values.size();
    *p50 = sorted[(size_t)((sorted.size() - 1) * 0.50)];
    *p95 = sorted[(size_t)((sorted.size() - 1) * 0.95)];
    *p99 = sorted[(size_t)((sorted.size() - 1) * 0.99)];
    *maxv = sorted.back();
}

static double recall_at_k(const std::vector<size_t>& got, const std::vector<size_t>& truth, size_t topk) {
    size_t hits = 0;
    for (size_t id : got) {
        if (std::find(truth.begin(), truth.end(), id) != truth.end()) ++hits;
    }
    return topk ? (double)hits / (double)topk : 0.0;
}

static bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--backend") == 0) args.backend = need(argv[i]);
        else if (std::strcmp(argv[i], "--dataset") == 0) args.dataset_path = need(argv[i]);
        else if (std::strcmp(argv[i], "--queries") == 0) args.queries_path = need(argv[i]);
        else if (std::strcmp(argv[i], "--n") == 0) args.n = std::strtoull(need(argv[i]), nullptr, 10);
        else if (std::strcmp(argv[i], "--nq") == 0) args.nq = std::strtoull(need(argv[i]), nullptr, 10);
        else if (std::strcmp(argv[i], "--dim") == 0) args.dim = std::strtoull(need(argv[i]), nullptr, 10);
        else if (std::strcmp(argv[i], "--topk") == 0) args.topk = std::strtoull(need(argv[i]), nullptr, 10);
        else if (std::strcmp(argv[i], "--m") == 0) args.m = std::strtoull(need(argv[i]), nullptr, 10);
        else if (std::strcmp(argv[i], "--ef-construction") == 0) args.ef_construction = std::strtoull(need(argv[i]), nullptr, 10);
        else if (std::strcmp(argv[i], "--ef-search") == 0) args.ef_search = std::strtoull(need(argv[i]), nullptr, 10);
        else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    return !args.backend.empty() && !args.dataset_path.empty() && !args.queries_path.empty() &&
           args.n > 0 && args.nq > 0 && args.dim > 0 && args.topk > 0;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 2;

    std::vector<float> base;
    std::vector<float> queries;
    if (!read_f32_file(args.dataset_path, base, args.n * args.dim) ||
        !read_f32_file(args.queries_path, queries, args.nq * args.dim)) {
        std::fprintf(stderr, "failed to read dataset or queries\n");
        return 3;
    }

    std::vector<std::vector<size_t>> ground_truth;
    ground_truth.reserve(args.nq);
    for (size_t qi = 0; qi < args.nq; ++qi) {
        ground_truth.push_back(exact_topk(base, &queries[qi * args.dim], args.n, args.dim, args.topk));
    }

    std::vector<uint64_t> add_us(args.n);
    std::vector<uint64_t> search_us(args.nq);
    double recall_sum = 0.0;
    int errors = 0;

    if (args.backend == "native_flat") {
        for (size_t i = 0; i < args.n; ++i) {
            uint64_t t0 = now_us();
            uint64_t t1 = now_us();
            add_us[i] = t1 - t0;
        }
        for (size_t qi = 0; qi < args.nq; ++qi) {
            uint64_t t0 = now_us();
            auto got = exact_topk(base, &queries[qi * args.dim], args.n, args.dim, args.topk);
            uint64_t t1 = now_us();
            search_us[qi] = t1 - t0;
            recall_sum += recall_at_k(got, ground_truth[qi], args.topk);
        }
    } else if (args.backend == "hnswlib_hnsw") {
        hnswlib::InnerProductSpace space((int)args.dim);
        hnswlib::HierarchicalNSW<float> index(&space, args.n, args.m, args.ef_construction);
        index.setEf(args.ef_search);
        for (size_t i = 0; i < args.n; ++i) {
            uint64_t t0 = now_us();
            try {
                index.addPoint(&base[i * args.dim], i);
            } catch (...) {
                ++errors;
            }
            uint64_t t1 = now_us();
            add_us[i] = t1 - t0;
        }
        for (size_t qi = 0; qi < args.nq; ++qi) {
            std::vector<size_t> got;
            uint64_t t0 = now_us();
            try {
                auto result = index.searchKnn(&queries[qi * args.dim], args.topk);
                while (!result.empty()) {
                    got.push_back((size_t)result.top().second);
                    result.pop();
                }
            } catch (...) {
                ++errors;
            }
            uint64_t t1 = now_us();
            search_us[qi] = t1 - t0;
            recall_sum += recall_at_k(got, ground_truth[qi], args.topk);
        }
    } else if (args.backend == "faiss_flat" || args.backend == "faiss_hnsw") {
        faiss::Index* index = nullptr;
        faiss::IndexFlatIP flat((int)args.dim);
        faiss::IndexHNSWFlat hnsw((int)args.dim, (int)args.m, faiss::METRIC_INNER_PRODUCT);
        if (args.backend == "faiss_flat") {
            index = &flat;
        } else {
            hnsw.hnsw.efConstruction = (int)args.ef_construction;
            hnsw.hnsw.efSearch = (int)args.ef_search;
            index = &hnsw;
        }
        for (size_t i = 0; i < args.n; ++i) {
            uint64_t t0 = now_us();
            try {
                index->add(1, &base[i * args.dim]);
            } catch (...) {
                ++errors;
            }
            uint64_t t1 = now_us();
            add_us[i] = t1 - t0;
        }
        std::vector<float> distances(args.topk);
        std::vector<faiss::idx_t> labels(args.topk);
        for (size_t qi = 0; qi < args.nq; ++qi) {
            std::vector<size_t> got;
            uint64_t t0 = now_us();
            try {
                index->search(1, &queries[qi * args.dim], (faiss::idx_t)args.topk,
                              distances.data(), labels.data());
                for (size_t j = 0; j < args.topk; ++j) {
                    if (labels[j] >= 0) got.push_back((size_t)labels[j]);
                }
            } catch (...) {
                ++errors;
            }
            uint64_t t1 = now_us();
            search_us[qi] = t1 - t0;
            recall_sum += recall_at_k(got, ground_truth[qi], args.topk);
        }
    } else {
        std::fprintf(stderr, "unsupported backend: %s\n", args.backend.c_str());
        return 4;
    }

    uint64_t add_total = 0, add_avg = 0, add_p50 = 0, add_p95 = 0, add_p99 = 0, add_max = 0;
    uint64_t search_total = 0, search_avg = 0, search_p50 = 0, search_p95 = 0, search_p99 = 0, search_max = 0;
    stats(add_us, &add_total, &add_avg, &add_p50, &add_p95, &add_p99, &add_max);
    stats(search_us, &search_total, &search_avg, &search_p50, &search_p95, &search_p99, &search_max);
    double add_qps = add_total ? (double)args.n / ((double)add_total / 1000000.0) : 0.0;
    double search_qps = search_total ? (double)args.nq / ((double)search_total / 1000000.0) : 0.0;
    double recall = args.nq ? recall_sum / (double)args.nq : 0.0;

    std::printf("operation,total_ms,qps,avg_us,p50_us,p95_us,p99_us,max_us,recall_at_10,rss_kb,error_count\n");
    std::printf("add,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,,%ld,%d\n",
                (double)add_total / 1000.0, add_qps,
                (unsigned long long)add_avg, (unsigned long long)add_p50,
                (unsigned long long)add_p95, (unsigned long long)add_p99,
                (unsigned long long)add_max, rss_kb(), errors);
    std::printf("search,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%.6f,%ld,%d\n",
                (double)search_total / 1000.0, search_qps,
                (unsigned long long)search_avg, (unsigned long long)search_p50,
                (unsigned long long)search_p95, (unsigned long long)search_p99,
                (unsigned long long)search_max, recall, rss_kb(), errors);

    return errors == 0 ? 0 : 5;
}
