/*
 * B-scale-3 real embedding HNSW/FAISS sweep benchmark.
 *
 * Index-only runner for an already-generated real embedding cache and exact
 * ground truth. It does not call ONNX, does not regenerate ground truth, and
 * does not touch product V command behavior.
 */

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <inttypes.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <omp.h>
#include <openssl/evp.h>

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/Index.h>
#include <faiss/impl/io.h>
#include <faiss/index_io.h>

extern "C" {
typedef struct bench_core_hit_s {
    uint64_t record_id;
    float score;
} bench_core_hit_t;

typedef struct bench_core_hnsw_v2_stats_s {
    uint64_t M;
    uint64_t M0;
    uint64_t ef_construction;
    uint64_t ef_search;
    uint64_t memory_bytes;
    uint64_t size;
    uint64_t active_count;
    uint64_t deleted_count;
    uint64_t capacity;
    uint64_t duplicate_edge_count;
    uint64_t degree_overflow_count;
} bench_core_hnsw_v2_stats_t;

int bench_core_hnsw_v2_build(size_t dim,
                             size_t count,
                             const float* base,
                             const uint64_t* insertion_order,
                             uint64_t seed,
                             void** out_handle,
                             bench_core_hnsw_v2_stats_t* out_stats,
                             uint64_t* out_build_us);

int bench_core_hnsw_v2_search(void* handle,
                              const float* query,
                              size_t dim,
                              size_t topk,
                              size_t ef_search,
                              bench_core_hit_t* hits,
                              size_t* out_count);

void bench_core_hnsw_v2_destroy(void* handle);
}

static const char* GEN_VERSION = "1";
static const char* INSERTION_ALGO = "sha256(source_revision_tab_hnsw-insertion_tab_external_pid)";
static const char* DEFAULT_EF_LIST = "32,64,128,256,512,1024";
static const char* DEFAULT_SOURCE_REVISION =
    "2a369a430a70979223f1b9a41b1919774d46b432";
static const int DEFAULT_TOPK = 10;
static const int WARMUP_ROUNDS = 1;
static const int MEASURED_ROUNDS = 5;
static const int HNSW_M = 48;
static const int HNSW_M0 = 96;
static const int HNSW_EFC = 512;
static const uint64_t CORE_HNSW_SEED = 0x9d000000ULL;
static const int FAISS_THREAD_COUNT = 1;
static const double NORM_TOL = 1e-3;
static const double FLAT_SCORE_TOL = 1e-4;

static void faiss_version_string(char* out, size_t outsz) {
    snprintf(out,
             outsz,
             "%d.%d.%d",
             FAISS_VERSION_MAJOR,
             FAISS_VERSION_MINOR,
             FAISS_VERSION_PATCH);
}

struct CliArgs {
    const char* dataset_id = nullptr;
    const char* cache_dir = nullptr;
    const char* groundtruth_dir = nullptr;
    const char* output_dir = nullptr;
    const char* source_revision = DEFAULT_SOURCE_REVISION;
    const char* ef_search_list = DEFAULT_EF_LIST;
    int topk = DEFAULT_TOPK;
};

struct InputsInfo {
    std::string dataset_id;
    long long record_count = 0;
    long long query_count = 0;
    long long dim = 0;
    long long topk = 0;
    char manifest_sha[65] = {0};
    char emb_sha[65] = {0};
    char qemb_sha[65] = {0};
    char records_sha[65] = {0};
    char queries_sha[65] = {0};
    char gt_sha[65] = {0};
    char gt_manifest_sha[65] = {0};
};

struct LatencyStats {
    uint64_t total_us = 0;
    double avg_us = 0.0;
    uint64_t p50_us = 0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
    uint64_t min_us = 0;
    uint64_t max_us = 0;
    double qps = 0.0;
};

struct Quality {
    double recall_at_1 = 0.0;
    double recall_at_10 = 0.0;
    double top1_match_rate = 0.0;
    double topk_exact_match_rate = 0.0;
    double min_per_query_recall_at_10 = 1.0;
    long long queries_checked = 0;
    long long failed_query_count = 0;
};

struct SweepResult {
    int ef_search = 0;
    Quality quality;
    LatencyStats latency;
};

struct InsertionItem {
    uint64_t record_id = 0;
    unsigned char hash[32] = {0};
};

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static long peak_rss_kb() {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return -1;
    }
    return ru.ru_maxrss;
}

static int host_is_little_endian() {
    uint16_t v = 1;
    return *(uint8_t*)&v == 1;
}

static uint32_t swap_u32(uint32_t v) {
    return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
           ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

static int path_exists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static int mkdir_p(const char* path) {
    char tmp[8192];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void dirname_of(const char* path, char* out, size_t outsz) {
    const char* slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, outsz, ".");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= outsz) {
        len = outsz - 1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

static void iso8601_utc_now(char* out, size_t outsz) {
    time_t t = time(nullptr);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, outsz, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static void json_write_escaped(FILE* f, const char* s) {
    fputc('"', f);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        default:
            if (*p < 0x20) {
                fprintf(f, "\\u%04x", *p);
            } else {
                fputc(*p, f);
            }
        }
    }
    fputc('"', f);
}

static int sha256_file(const char* path, char out_hex[65]) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(f);
        return -1;
    }
    unsigned char buf[65536];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    int rc = -1;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        goto done;
    }
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0 && EVP_DigestUpdate(ctx, buf, n) != 1) {
            goto done;
        }
        if (n < sizeof(buf)) {
            if (ferror(f)) {
                goto done;
            }
            break;
        }
    }
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        goto done;
    }
    for (unsigned int i = 0; i < digest_len; i++) {
        sprintf(out_hex + i * 2, "%02x", digest[i]);
    }
    out_hex[digest_len * 2] = '\0';
    rc = 0;
done:
    EVP_MD_CTX_free(ctx);
    fclose(f);
    return rc;
}

static int sha256_bytes(const unsigned char* data, size_t n, unsigned char out[32]) {
    unsigned int digest_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return -1;
    }
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
             EVP_DigestUpdate(ctx, data, n) == 1 &&
             EVP_DigestFinal_ex(ctx, out, &digest_len) == 1 &&
             digest_len == 32;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

static int same_file_sha256(const char* a, const char* b) {
    char sa[65], sb[65];
    if (!path_exists(a) || !path_exists(b)) {
        return 0;
    }
    return sha256_file(a, sa) == 0 && sha256_file(b, sb) == 0 && strcmp(sa, sb) == 0;
}

static std::string read_whole_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return std::string();
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return std::string();
    }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return std::string();
    }
    std::string out((size_t)sz, '\0');
    if (fread(&out[0], 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return std::string();
    }
    fclose(f);
    return out;
}

static const char* json_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static const char* json_find_key(const char* text, const char* key) {
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(text, pat);
    if (!p) {
        return nullptr;
    }
    p += strlen(pat);
    p = json_skip_ws(p);
    if (*p != ':') {
        return nullptr;
    }
    return json_skip_ws(p + 1);
}

static std::string json_extract_object(const char* text, const char* key) {
    const char* v = json_find_key(text, key);
    if (!v || *v != '{') {
        return std::string();
    }
    const char* start = v;
    int depth = 0;
    do {
        if (*v == '{') {
            depth++;
        } else if (*v == '}') {
            depth--;
        } else if (*v == '\0') {
            return std::string();
        }
        v++;
    } while (depth > 0);
    return std::string(start, (size_t)(v - start));
}

static int json_get_string(const char* text, const char* key, char* out, size_t outsz) {
    const char* v = json_find_key(text, key);
    size_t i = 0;
    if (!v || *v != '"') {
        return -1;
    }
    v++;
    while (*v && *v != '"' && i + 1 < outsz) {
        if (*v == '\\' && *(v + 1)) {
            v++;
            switch (*v) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case 'r': out[i++] = '\r'; break;
            case '"': out[i++] = '"'; break;
            case '\\': out[i++] = '\\'; break;
            default: out[i++] = *v;
            }
            v++;
        } else {
            out[i++] = *v++;
        }
    }
    out[i] = '\0';
    return *v == '"' ? 0 : -1;
}

static int json_get_int64(const char* text, const char* key, long long* out) {
    const char* v = json_find_key(text, key);
    char* end = nullptr;
    if (!v) {
        return -1;
    }
    long long val = strtoll(v, &end, 10);
    if (end == v) {
        return -1;
    }
    *out = val;
    return 0;
}

static int json_get_double(const char* text, const char* key, double* out) {
    const char* v = json_find_key(text, key);
    char* end = nullptr;
    if (!v) {
        return -1;
    }
    double val = strtod(v, &end);
    if (end == v) {
        return -1;
    }
    *out = val;
    return 0;
}

static int json_get_bool(const char* text, const char* key, int* out) {
    const char* v = json_find_key(text, key);
    if (!v) {
        return -1;
    }
    if (strncmp(v, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static std::vector<float> load_f32(const char* path, size_t count, int dim) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        throw std::runtime_error(std::string("cannot open ") + path);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        throw std::runtime_error("fseek failed");
    }
    long sz = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        throw std::runtime_error("fseek failed");
    }
    size_t n = count * (size_t)dim;
    if (sz < 0 || (size_t)sz != n * sizeof(float)) {
        fclose(f);
        throw std::runtime_error(std::string(path) + " size mismatch");
    }
    std::vector<float> out(n);
    if (fread(out.data(), sizeof(float), n, f) != n) {
        fclose(f);
        throw std::runtime_error(std::string(path) + " short read");
    }
    fclose(f);
    if (!host_is_little_endian()) {
        for (float& x : out) {
            uint32_t v;
            memcpy(&v, &x, sizeof(v));
            v = swap_u32(v);
            memcpy(&x, &v, sizeof(v));
        }
    }
    return out;
}

static double row_norm(const float* row, int dim) {
    double s = 0.0;
    for (int i = 0; i < dim; i++) {
        s += (double)row[i] * (double)row[i];
    }
    return sqrt(s);
}

static void validate_vectors(const std::vector<float>& x, long long rows, int dim, const char* name) {
    for (long long r = 0; r < rows; r++) {
        const float* row = x.data() + r * dim;
        for (int d = 0; d < dim; d++) {
            if (!std::isfinite(row[d])) {
                throw std::runtime_error(std::string(name) + " contains non-finite vector");
            }
        }
        if (fabs(row_norm(row, dim) - 1.0) > NORM_TOL) {
            throw std::runtime_error(std::string(name) + " contains non-normalized vector");
        }
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "usage:\n"
            "  %s generate --cache-dir DIR --groundtruth-dir DIR --output-dir DIR [--dataset-id ID] [--top-k N] [--source-revision REV]\n"
            "  %s validate --cache-dir DIR --groundtruth-dir DIR --output-dir DIR [--dataset-id ID]\n",
            prog, prog);
}

static int parse_args(int argc, char** argv, CliArgs* args) {
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dataset-id") == 0 && i + 1 < argc) {
            args->dataset_id = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            args->cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--groundtruth-dir") == 0 && i + 1 < argc) {
            args->groundtruth_dir = argv[++i];
        } else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            args->output_dir = argv[++i];
        } else if (strcmp(argv[i], "--source-revision") == 0 && i + 1 < argc) {
            args->source_revision = argv[++i];
        } else if (strcmp(argv[i], "--ef-search-list") == 0 && i + 1 < argc) {
            args->ef_search_list = argv[++i];
        } else if ((strcmp(argv[i], "--topk") == 0 || strcmp(argv[i], "--top-k") == 0) && i + 1 < argc) {
            args->topk = atoi(argv[++i]);
        } else {
            fprintf(stderr, "[error] unknown argument: %s\n", argv[i]);
            return -1;
        }
    }
    if (!args->cache_dir || !args->groundtruth_dir || !args->output_dir) {
        fprintf(stderr, "[error] --cache-dir, --groundtruth-dir, and --output-dir are required\n");
        return -1;
    }
    if (args->topk <= 0) {
        fprintf(stderr, "[error] --top-k must be positive\n");
        return -1;
    }
    return 0;
}

static std::vector<int> parse_ef_list(const char* s) {
    std::vector<int> out;
    const char* p = s;
    while (*p) {
        char* end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p || v <= 0 || v > 1000000) {
            throw std::runtime_error("invalid --ef-search-list");
        }
        out.push_back((int)v);
        p = end;
        if (*p == ',') {
            p++;
        } else if (*p != '\0') {
            throw std::runtime_error("invalid --ef-search-list separator");
        }
    }
    if (out.empty()) {
        throw std::runtime_error("empty --ef-search-list");
    }
    return out;
}

static int verify_output_sha(const std::string& outputs, const char* key,
                             const char* dir, const char* filename, char out_sha[65]) {
    std::string block = json_extract_object(outputs.c_str(), key);
    char sha[80] = {0};
    char actual[65];
    char path[8192];
    if (block.empty()) {
        fprintf(stderr, "[error] manifest outputs.%s missing\n", key);
        return -1;
    }
    if (json_get_string(block.c_str(), "sha256", sha, sizeof(sha)) != 0 || strlen(sha) != 64) {
        fprintf(stderr, "[error] manifest outputs.%s.sha256 invalid\n", key);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    if (sha256_file(path, actual) != 0 || strcmp(actual, sha) != 0) {
        fprintf(stderr, "[error] %s sha256 mismatch\n", path);
        return -1;
    }
    memcpy(out_sha, sha, 65);
    return 0;
}

static int verify_inputs(const CliArgs& args, InputsInfo* out) {
    char path[8192];
    char dataset_id_val[256] = {0};
    char dtype[32] = {0}, byte_order[32] = {0}, metric[64] = {0};
    long long schema_version = 0;

    *out = InputsInfo{};
    snprintf(path, sizeof(path), "%s/manifest.json", args.cache_dir);
    std::string b1 = read_whole_file(path);
    if (b1.empty() || sha256_file(path, out->manifest_sha) != 0) {
        fprintf(stderr, "[error] cannot read/checksum B1 manifest\n");
        return -1;
    }
    if (json_get_int64(b1.c_str(), "schema_version", &schema_version) != 0 || schema_version != 1 ||
        json_get_string(b1.c_str(), "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0) {
        fprintf(stderr, "[error] B1 manifest identity invalid\n");
        return -1;
    }
    if (args.dataset_id && args.dataset_id[0] && strcmp(dataset_id_val, args.dataset_id) != 0) {
        fprintf(stderr, "[error] B1 manifest dataset_id mismatch\n");
        return -1;
    }
    out->dataset_id = dataset_id_val;
    std::string params = json_extract_object(b1.c_str(), "params");
    int normalized = 0;
    if (params.empty() ||
        json_get_int64(params.c_str(), "record_count", &out->record_count) != 0 || out->record_count <= 0 ||
        json_get_int64(params.c_str(), "query_count", &out->query_count) != 0 || out->query_count <= 0 ||
        json_get_int64(params.c_str(), "dim", &out->dim) != 0 || out->dim <= 0 ||
        json_get_string(params.c_str(), "dtype", dtype, sizeof(dtype)) != 0 || strcmp(dtype, "float32") != 0 ||
        json_get_string(params.c_str(), "byte_order", byte_order, sizeof(byte_order)) != 0 || strcmp(byte_order, "little") != 0 ||
        json_get_string(params.c_str(), "metric", metric, sizeof(metric)) != 0 || strcmp(metric, "ip_normalized") != 0 ||
        json_get_bool(params.c_str(), "normalized", &normalized) != 0 || !normalized) {
        fprintf(stderr, "[error] B1 params invalid\n");
        return -1;
    }
    std::string outputs = json_extract_object(b1.c_str(), "outputs");
    if (outputs.empty() ||
        verify_output_sha(outputs, "embeddings_f32", args.cache_dir, "embeddings.f32", out->emb_sha) != 0 ||
        verify_output_sha(outputs, "query_embeddings_f32", args.cache_dir, "query_embeddings.f32", out->qemb_sha) != 0 ||
        verify_output_sha(outputs, "records_jsonl", args.cache_dir, "records.jsonl", out->records_sha) != 0 ||
        verify_output_sha(outputs, "queries_jsonl", args.cache_dir, "queries.jsonl", out->queries_sha) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/groundtruth.manifest.json", args.groundtruth_dir);
    std::string gtm = read_whole_file(path);
    if (gtm.empty() || sha256_file(path, out->gt_manifest_sha) != 0) {
        fprintf(stderr, "[error] cannot read/checksum groundtruth manifest\n");
        return -1;
    }
    if (json_get_int64(gtm.c_str(), "schema_version", &schema_version) != 0 || schema_version != 1 ||
        json_get_string(gtm.c_str(), "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
        strcmp(dataset_id_val, out->dataset_id.c_str()) != 0) {
        fprintf(stderr, "[error] groundtruth manifest identity invalid\n");
        return -1;
    }
    std::string gtsrc = json_extract_object(gtm.c_str(), "source");
    char src_manifest_sha[80] = {0};
    if (gtsrc.empty() ||
        json_get_string(gtsrc.c_str(), "manifest_sha256", src_manifest_sha, sizeof(src_manifest_sha)) != 0 ||
        strcmp(src_manifest_sha, out->manifest_sha) != 0) {
        fprintf(stderr, "[error] groundtruth is not bound to this cache manifest\n");
        return -1;
    }
    std::string gtparams = json_extract_object(gtm.c_str(), "params");
    long long gt_rc = 0, gt_qc = 0, gt_dim = 0, gt_topk = 0;
    char gt_metric[64] = {0};
    if (gtparams.empty() ||
        json_get_int64(gtparams.c_str(), "record_count", &gt_rc) != 0 || gt_rc != out->record_count ||
        json_get_int64(gtparams.c_str(), "query_count", &gt_qc) != 0 || gt_qc != out->query_count ||
        json_get_int64(gtparams.c_str(), "dim", &gt_dim) != 0 || gt_dim != out->dim ||
        json_get_int64(gtparams.c_str(), "topk", &gt_topk) != 0 || gt_topk != args.topk ||
        json_get_string(gtparams.c_str(), "metric", gt_metric, sizeof(gt_metric)) != 0 ||
        strcmp(gt_metric, "ip_normalized") != 0) {
        fprintf(stderr, "[error] groundtruth params inconsistent with cache/topK\n");
        return -1;
    }
    std::string gtout = json_extract_object(gtm.c_str(), "outputs");
    if (gtout.empty() ||
        verify_output_sha(gtout, "groundtruth_jsonl", args.groundtruth_dir, "groundtruth.jsonl", out->gt_sha) != 0) {
        return -1;
    }
    out->topk = args.topk;
    return 0;
}

static void load_groundtruth(const char* dir, long long query_count, int topk,
                             long long record_count, std::vector<uint64_t>* ids,
                             std::vector<float>* scores) {
    char path[8192];
    snprintf(path, sizeof(path), "%s/groundtruth.jsonl", dir);
    FILE* f = fopen(path, "r");
    if (!f) {
        throw std::runtime_error("cannot open groundtruth.jsonl");
    }
    ids->assign((size_t)query_count * (size_t)topk, 0);
    scores->assign((size_t)query_count * (size_t)topk, 0.0f);
    std::vector<uint8_t> present((size_t)query_count, 0);
    char* line = nullptr;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) != -1) {
        (void)len;
        long long query_row = -1, line_topk = -1;
        if (json_get_int64(line, "query_row", &query_row) != 0 || query_row < 0 || query_row >= query_count ||
            json_get_int64(line, "topk", &line_topk) != 0 || line_topk != topk ||
            present[(size_t)query_row]) {
            free(line);
            fclose(f);
            throw std::runtime_error("bad groundtruth line");
        }
        const char* p = strstr(line, "\"neighbors\"");
        if (!p) {
            free(line);
            fclose(f);
            throw std::runtime_error("groundtruth missing neighbors");
        }
        for (int r = 0; r < topk; r++) {
            const char* rk = strstr(p, "\"rank\"");
            long long rank_v = -1, rid_v = -1;
            double score_v = 0.0;
            if (!rk || json_get_int64(rk, "rank", &rank_v) != 0 || rank_v != r + 1 ||
                json_get_int64(rk, "record_id", &rid_v) != 0 || rid_v < 0 || rid_v >= record_count ||
                json_get_double(rk, "score", &score_v) != 0 || !std::isfinite(score_v)) {
                free(line);
                fclose(f);
                throw std::runtime_error("bad groundtruth neighbor");
            }
            (*ids)[(size_t)query_row * (size_t)topk + (size_t)r] = (uint64_t)rid_v;
            (*scores)[(size_t)query_row * (size_t)topk + (size_t)r] = (float)score_v;
            p = rk + 6;
        }
        present[(size_t)query_row] = 1;
    }
    free(line);
    fclose(f);
    for (long long q = 0; q < query_count; q++) {
        if (!present[(size_t)q]) {
            throw std::runtime_error("groundtruth missing query_row");
        }
    }
}

static std::string record_external_id_from_json(const char* line) {
    long long id = -1;
    char buf[1024] = {0};
    if (json_get_string(line, "pid", buf, sizeof(buf)) == 0 && buf[0]) {
        return std::string(buf);
    }
    if (json_get_string(line, "external_id", buf, sizeof(buf)) == 0 && buf[0]) {
        return std::string(buf);
    }
    if (json_get_int64(line, "id", &id) == 0 && id >= 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%lld", id);
        return std::string(tmp);
    }
    throw std::runtime_error("records.jsonl missing pid/external_id/id");
}

static std::vector<uint64_t> generate_insertion_order(const char* cache_dir,
                                                      const char* source_revision,
                                                      long long record_count) {
    char path[8192];
    snprintf(path, sizeof(path), "%s/records.jsonl", cache_dir);
    FILE* f = fopen(path, "r");
    if (!f) {
        throw std::runtime_error("cannot open records.jsonl");
    }
    std::vector<InsertionItem> items;
    items.reserve((size_t)record_count);
    std::vector<uint8_t> seen((size_t)record_count, 0);
    char* line = nullptr;
    size_t cap = 0;
    ssize_t len;
    long long row = 0;
    while ((len = getline(&line, &cap, f)) != -1) {
        (void)len;
        long long id = -1;
        if (json_get_int64(line, "id", &id) != 0 || id < 0 || id >= record_count || id != row) {
            free(line);
            fclose(f);
            throw std::runtime_error("records.jsonl id/row mapping invalid");
        }
        if (seen[(size_t)id]) {
            free(line);
            fclose(f);
            throw std::runtime_error("duplicate records.jsonl id");
        }
        seen[(size_t)id] = 1;
        std::string external_id = record_external_id_from_json(line);
        std::string key = std::string(source_revision) + "\thnsw-insertion\t" + external_id;
        InsertionItem item;
        item.record_id = (uint64_t)id;
        if (sha256_bytes((const unsigned char*)key.data(), key.size(), item.hash) != 0) {
            free(line);
            fclose(f);
            throw std::runtime_error("sha256 failed");
        }
        items.push_back(item);
        row++;
    }
    free(line);
    fclose(f);
    if (row != record_count) {
        throw std::runtime_error("records.jsonl row count mismatch");
    }
    std::sort(items.begin(), items.end(), [](const InsertionItem& a, const InsertionItem& b) {
        int c = memcmp(a.hash, b.hash, sizeof(a.hash));
        if (c != 0) {
            return c < 0;
        }
        return a.record_id < b.record_id;
    });
    std::vector<uint64_t> order;
    order.reserve((size_t)record_count);
    std::fill(seen.begin(), seen.end(), 0);
    for (const InsertionItem& it : items) {
        if (it.record_id >= (uint64_t)record_count || seen[(size_t)it.record_id]) {
            throw std::runtime_error("insertion order is not a permutation");
        }
        seen[(size_t)it.record_id] = 1;
        order.push_back(it.record_id);
    }
    return order;
}

static void write_insertion_order(const char* path, const std::vector<uint64_t>& order) {
    FILE* f = fopen(path, "w");
    if (!f) {
        throw std::runtime_error("cannot write insertion_order.ids");
    }
    for (uint64_t id : order) {
        fprintf(f, "%" PRIu64 "\n", id);
    }
    int err = (fflush(f) != 0 || ferror(f));
    if (fclose(f) != 0) {
        err = 1;
    }
    if (err) {
        throw std::runtime_error("write insertion_order.ids failed");
    }
}

static void validate_insertion_order_file(const char* path, long long record_count) {
    FILE* f = fopen(path, "r");
    if (!f) {
        throw std::runtime_error("cannot open insertion_order.ids");
    }
    std::vector<uint8_t> seen((size_t)record_count, 0);
    char* line = nullptr;
    size_t cap = 0;
    ssize_t len;
    long long rows = 0;
    while ((len = getline(&line, &cap, f)) != -1) {
        (void)len;
        char* end = nullptr;
        unsigned long long id = strtoull(line, &end, 10);
        if (end == line || id >= (unsigned long long)record_count || seen[(size_t)id]) {
            free(line);
            fclose(f);
            throw std::runtime_error("invalid insertion_order.ids");
        }
        seen[(size_t)id] = 1;
        rows++;
    }
    free(line);
    fclose(f);
    if (rows != record_count) {
        throw std::runtime_error("insertion_order.ids row count mismatch");
    }
}

static uint64_t percentile(const std::vector<uint64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    long long idx = (long long)ceil(p / 100.0 * (double)sorted.size()) - 1;
    if (idx < 0) {
        idx = 0;
    }
    if ((size_t)idx >= sorted.size()) {
        idx = (long long)sorted.size() - 1;
    }
    return sorted[(size_t)idx];
}

static LatencyStats make_latency_stats(const std::vector<uint64_t>& samples) {
    LatencyStats st;
    if (samples.empty()) {
        return st;
    }
    std::vector<uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    for (uint64_t v : samples) {
        st.total_us += v;
    }
    st.avg_us = (double)st.total_us / (double)samples.size();
    st.p50_us = percentile(sorted, 50.0);
    st.p95_us = percentile(sorted, 95.0);
    st.p99_us = percentile(sorted, 99.0);
    st.min_us = sorted.front();
    st.max_us = sorted.back();
    st.qps = st.total_us ? (double)samples.size() * 1000000.0 / (double)st.total_us : 0.0;
    return st;
}

static int set_match_topk_u64(const std::vector<uint64_t>& ann, const uint64_t* gt, int topk) {
    std::vector<uint64_t> g(gt, gt + topk);
    std::vector<uint64_t> a = ann;
    std::sort(a.begin(), a.end());
    std::sort(g.begin(), g.end());
    return a == g;
}

static Quality compute_quality(const std::vector<std::vector<uint64_t>>& ann,
                               const std::vector<uint64_t>& gt_ids,
                               int topk) {
    Quality q;
    q.queries_checked = (long long)ann.size();
    double sum_r1 = 0.0, sum_r10 = 0.0, sum_top1 = 0.0, sum_exact = 0.0;
    double min_r10 = 1.0;
    for (size_t qi = 0; qi < ann.size(); qi++) {
        const uint64_t* gt = gt_ids.data() + qi * (size_t)topk;
        int inter = 0;
        for (int r = 0; r < topk; r++) {
            for (uint64_t id : ann[qi]) {
                if (id == gt[r]) {
                    inter++;
                    break;
                }
            }
        }
        int r1_found = 0;
        for (uint64_t id : ann[qi]) {
            if (id == gt[0]) {
                r1_found = 1;
                break;
            }
        }
        int top1 = (!ann[qi].empty() && ann[qi][0] == gt[0]) ? 1 : 0;
        int exact = (ann[qi].size() == (size_t)topk);
        for (int r = 0; exact && r < topk; r++) {
            if (ann[qi][(size_t)r] != gt[r]) {
                exact = 0;
            }
        }
        double r10 = (double)inter / (double)topk;
        sum_r10 += r10;
        sum_r1 += r1_found;
        sum_top1 += top1;
        sum_exact += exact;
        min_r10 = std::min(min_r10, r10);
    }
    if (!ann.empty()) {
        q.recall_at_1 = sum_r1 / (double)ann.size();
        q.recall_at_10 = sum_r10 / (double)ann.size();
        q.top1_match_rate = sum_top1 / (double)ann.size();
        q.topk_exact_match_rate = sum_exact / (double)ann.size();
        q.min_per_query_recall_at_10 = min_r10;
    }
    return q;
}

static void validate_ann_result(const std::vector<uint64_t>& ids,
                                const std::vector<float>& scores,
                                int topk,
                                long long record_count) {
    if (ids.size() != (size_t)topk || scores.size() != (size_t)topk) {
        throw std::runtime_error("wrong hit count");
    }
    for (int i = 0; i < topk; i++) {
        if (ids[(size_t)i] >= (uint64_t)record_count || !std::isfinite(scores[(size_t)i])) {
            throw std::runtime_error("invalid ANN hit");
        }
        if (i > 0 && scores[(size_t)i] > scores[(size_t)i - 1] + 1e-7f) {
            throw std::runtime_error("ANN scores not descending");
        }
        for (int j = 0; j < i; j++) {
            if (ids[(size_t)i] == ids[(size_t)j]) {
                throw std::runtime_error("duplicate ANN hit");
            }
        }
    }
}

static size_t serialized_index_bytes(const faiss::Index& index) {
    faiss::VectorIOWriter writer;
    faiss::write_index(&index, &writer);
    return writer.data.size();
}

static int run_flat(faiss::IndexFlatIP& flat,
                    const std::vector<float>& queries,
                    long long qcount,
                    int dim,
                    int topk,
                    long long record_count,
                    const std::vector<uint64_t>& gt_ids,
                    const std::vector<float>& gt_scores,
                    const char* out_path,
                    LatencyStats* latency,
                    int* strict_rank_match_all,
                    double* max_score_abs_diff) {
    FILE* out = fopen(out_path, "w");
    if (!out) {
        return -1;
    }
    std::vector<faiss::idx_t> labels((size_t)topk);
    std::vector<float> scores((size_t)topk);
    std::vector<uint64_t> samples;
    samples.reserve((size_t)qcount * MEASURED_ROUNDS);
    *strict_rank_match_all = 1;
    *max_score_abs_diff = 0.0;

    for (int w = 0; w < WARMUP_ROUNDS; w++) {
        for (long long q = 0; q < qcount; q++) {
            flat.search(1, queries.data() + q * dim, topk, scores.data(), labels.data());
        }
    }
    std::vector<std::vector<uint64_t>> first_round((size_t)qcount);
    std::vector<std::vector<float>> first_scores((size_t)qcount);
    for (int round = 0; round < MEASURED_ROUNDS; round++) {
        for (long long q = 0; q < qcount; q++) {
            uint64_t t0 = now_us();
            flat.search(1, queries.data() + q * dim, topk, scores.data(), labels.data());
            samples.push_back(now_us() - t0);
            if (round == 0) {
                first_round[(size_t)q].resize((size_t)topk);
                first_scores[(size_t)q].resize((size_t)topk);
                for (int j = 0; j < topk; j++) {
                    first_round[(size_t)q][(size_t)j] = (uint64_t)labels[(size_t)j];
                    first_scores[(size_t)q][(size_t)j] = scores[(size_t)j];
                }
            }
        }
    }
    for (long long q = 0; q < qcount; q++) {
        const uint64_t* gt = gt_ids.data() + (size_t)q * (size_t)topk;
        const float* gs = gt_scores.data() + (size_t)q * (size_t)topk;
        validate_ann_result(first_round[(size_t)q], first_scores[(size_t)q], topk, record_count);
        int strict = 1;
        double max_abs = 0.0;
        for (int j = 0; j < topk; j++) {
            if (first_round[(size_t)q][(size_t)j] != gt[j]) {
                strict = 0;
            }
            max_abs = std::max(max_abs, fabs((double)first_scores[(size_t)q][(size_t)j] - (double)gs[j]));
        }
        if (!strict || max_abs > FLAT_SCORE_TOL) {
            *strict_rank_match_all = 0;
        }
        *max_score_abs_diff = std::max(*max_score_abs_diff, max_abs);
        fprintf(out, "{\"query_row\":%lld,\"ef_search\":null,\"hit_count\":%d,\"ann\":[", q, topk);
        for (int j = 0; j < topk; j++) {
            fprintf(out, "%s%" PRIu64, j ? "," : "", first_round[(size_t)q][(size_t)j]);
        }
        fprintf(out, "],\"hits\":[");
        for (int j = 0; j < topk; j++) {
            fprintf(out, "%s{\"rank\":%d,\"record_id\":%" PRIu64 ",\"score\":%.9g}",
                    j ? "," : "", j + 1, first_round[(size_t)q][(size_t)j],
                    (double)first_scores[(size_t)q][(size_t)j]);
        }
        fprintf(out, "],\"strict_rank_match\":%s,\"max_score_abs_diff\":%.9g}\n",
                (strict && max_abs <= FLAT_SCORE_TOL) ? "true" : "false", max_abs);
    }
    int err = (fflush(out) != 0 || ferror(out));
    if (fclose(out) != 0) {
        err = 1;
    }
    *latency = make_latency_stats(samples);
    return err ? -1 : 0;
}

static std::vector<SweepResult> run_core_sweep(void* index,
                                               const std::vector<float>& queries,
                                               long long qcount,
                                               int dim,
                                               int topk,
                                               long long record_count,
                                               const std::vector<uint64_t>& gt_ids,
                                               const std::vector<int>& ef_list,
                                               const char* out_path) {
    FILE* out = fopen(out_path, "w");
    if (!out) {
        throw std::runtime_error("cannot open core per-query");
    }
    std::vector<SweepResult> results;
    std::vector<bench_core_hit_t> hits((size_t)topk);
    for (int ef : ef_list) {
        for (int w = 0; w < WARMUP_ROUNDS; w++) {
            for (long long q = 0; q < qcount; q++) {
                size_t hit_count = 0;
                if (bench_core_hnsw_v2_search(index, queries.data() + q * dim, (size_t)dim,
                                              (size_t)topk, (size_t)ef, hits.data(), &hit_count) != 0 ||
                    hit_count != (size_t)topk) {
                    fclose(out);
                    throw std::runtime_error("core_hnsw_v2 warmup search failed");
                }
            }
        }
        std::vector<uint64_t> samples;
        samples.reserve((size_t)qcount * MEASURED_ROUNDS);
        std::vector<std::vector<uint64_t>> first_round((size_t)qcount);
        std::vector<std::vector<float>> first_scores((size_t)qcount);
        for (int round = 0; round < MEASURED_ROUNDS; round++) {
            for (long long q = 0; q < qcount; q++) {
                size_t hit_count = 0;
                uint64_t t0 = now_us();
                int rc = bench_core_hnsw_v2_search(index, queries.data() + q * dim, (size_t)dim,
                                                   (size_t)topk, (size_t)ef, hits.data(), &hit_count);
                samples.push_back(now_us() - t0);
                if (rc != 0 || hit_count != (size_t)topk) {
                    fclose(out);
                    throw std::runtime_error("core_hnsw_v2 search failed");
                }
                if (round == 0) {
                    first_round[(size_t)q].resize((size_t)topk);
                    first_scores[(size_t)q].resize((size_t)topk);
                    for (int j = 0; j < topk; j++) {
                        first_round[(size_t)q][(size_t)j] = hits[(size_t)j].record_id;
                        first_scores[(size_t)q][(size_t)j] = hits[(size_t)j].score;
                    }
                }
            }
        }
        SweepResult res;
        res.ef_search = ef;
        res.latency = make_latency_stats(samples);
        res.quality = compute_quality(first_round, gt_ids, topk);
        for (long long q = 0; q < qcount; q++) {
            validate_ann_result(first_round[(size_t)q], first_scores[(size_t)q], topk, record_count);
            const uint64_t* gt = gt_ids.data() + (size_t)q * (size_t)topk;
            int set_match = set_match_topk_u64(first_round[(size_t)q], gt, topk);
            int exact = 1;
            for (int j = 0; j < topk; j++) {
                if (first_round[(size_t)q][(size_t)j] != gt[j]) {
                    exact = 0;
                }
            }
            fprintf(out, "{\"backend\":\"core_hnsw_v2\",\"ef_search\":%d,\"query_row\":%lld,\"hit_count\":%d,\"ann\":[",
                    ef, q, topk);
            for (int j = 0; j < topk; j++) {
                fprintf(out, "%s%" PRIu64, j ? "," : "", first_round[(size_t)q][(size_t)j]);
            }
            fprintf(out, "],\"hits\":[");
            for (int j = 0; j < topk; j++) {
                fprintf(out, "%s{\"rank\":%d,\"record_id\":%" PRIu64 ",\"score\":%.9g}",
                        j ? "," : "", j + 1, first_round[(size_t)q][(size_t)j],
                        (double)first_scores[(size_t)q][(size_t)j]);
            }
            double r10 = set_match ? 1.0 : (double)0.0;
            int inter = 0;
            for (int r = 0; r < topk; r++) {
                for (uint64_t id : first_round[(size_t)q]) {
                    if (id == gt[r]) {
                        inter++;
                        break;
                    }
                }
            }
            r10 = (double)inter / (double)topk;
            fprintf(out, "],\"recall_at_1\":%d,\"recall_at_10\":%.6f,\"top1_match\":%d,\"exact_match\":%d}\n",
                    std::find(first_round[(size_t)q].begin(), first_round[(size_t)q].end(), gt[0]) != first_round[(size_t)q].end() ? 1 : 0,
                    r10,
                    first_round[(size_t)q][0] == gt[0] ? 1 : 0,
                    exact);
        }
        results.push_back(res);
    }
    int err = (fflush(out) != 0 || ferror(out));
    if (fclose(out) != 0) {
        err = 1;
    }
    if (err) {
        throw std::runtime_error("write core per-query failed");
    }
    return results;
}

static std::vector<SweepResult> run_faiss_hnsw_sweep(faiss::IndexHNSWFlat& hnsw,
                                                     const std::vector<float>& queries,
                                                     const std::vector<uint64_t>& insertion_order,
                                                     long long qcount,
                                                     int dim,
                                                     int topk,
                                                     long long record_count,
                                                     const std::vector<uint64_t>& gt_ids,
                                                     const std::vector<int>& ef_list,
                                                     const char* out_path) {
    FILE* out = fopen(out_path, "w");
    if (!out) {
        throw std::runtime_error("cannot open faiss hnsw per-query");
    }
    std::vector<SweepResult> results;
    std::vector<faiss::idx_t> labels((size_t)topk);
    std::vector<float> scores((size_t)topk);
    for (int ef : ef_list) {
        hnsw.hnsw.efSearch = ef;
        for (int w = 0; w < WARMUP_ROUNDS; w++) {
            for (long long q = 0; q < qcount; q++) {
                hnsw.search(1, queries.data() + q * dim, topk, scores.data(), labels.data());
            }
        }
        std::vector<uint64_t> samples;
        samples.reserve((size_t)qcount * MEASURED_ROUNDS);
        std::vector<std::vector<uint64_t>> first_round((size_t)qcount);
        std::vector<std::vector<float>> first_scores((size_t)qcount);
        for (int round = 0; round < MEASURED_ROUNDS; round++) {
            for (long long q = 0; q < qcount; q++) {
                uint64_t t0 = now_us();
                hnsw.search(1, queries.data() + q * dim, topk, scores.data(), labels.data());
                samples.push_back(now_us() - t0);
                if (round == 0) {
                    first_round[(size_t)q].resize((size_t)topk);
                    first_scores[(size_t)q].resize((size_t)topk);
                    for (int j = 0; j < topk; j++) {
                        if (labels[(size_t)j] < 0 || labels[(size_t)j] >= (faiss::idx_t)record_count) {
                            fclose(out);
                            throw std::runtime_error("FAISS label out of range");
                        }
                        first_round[(size_t)q][(size_t)j] = insertion_order[(size_t)labels[(size_t)j]];
                        first_scores[(size_t)q][(size_t)j] = scores[(size_t)j];
                    }
                }
            }
        }
        SweepResult res;
        res.ef_search = ef;
        res.latency = make_latency_stats(samples);
        res.quality = compute_quality(first_round, gt_ids, topk);
        for (long long q = 0; q < qcount; q++) {
            validate_ann_result(first_round[(size_t)q], first_scores[(size_t)q], topk, record_count);
            const uint64_t* gt = gt_ids.data() + (size_t)q * (size_t)topk;
            int exact = 1;
            int inter = 0;
            for (int r = 0; r < topk; r++) {
                if (first_round[(size_t)q][(size_t)r] != gt[r]) {
                    exact = 0;
                }
                for (uint64_t id : first_round[(size_t)q]) {
                    if (id == gt[r]) {
                        inter++;
                        break;
                    }
                }
            }
            double r10 = (double)inter / (double)topk;
            fprintf(out, "{\"backend\":\"faiss_hnsw\",\"ef_search\":%d,\"query_row\":%lld,\"hit_count\":%d,\"ann\":[",
                    ef, q, topk);
            for (int j = 0; j < topk; j++) {
                fprintf(out, "%s%" PRIu64, j ? "," : "", first_round[(size_t)q][(size_t)j]);
            }
            fprintf(out, "],\"hits\":[");
            for (int j = 0; j < topk; j++) {
                fprintf(out, "%s{\"rank\":%d,\"record_id\":%" PRIu64 ",\"score\":%.9g}",
                        j ? "," : "", j + 1, first_round[(size_t)q][(size_t)j],
                        (double)first_scores[(size_t)q][(size_t)j]);
            }
            fprintf(out, "],\"recall_at_1\":%d,\"recall_at_10\":%.6f,\"top1_match\":%d,\"exact_match\":%d}\n",
                    std::find(first_round[(size_t)q].begin(), first_round[(size_t)q].end(), gt[0]) != first_round[(size_t)q].end() ? 1 : 0,
                    r10,
                    first_round[(size_t)q][0] == gt[0] ? 1 : 0,
                    exact);
        }
        results.push_back(res);
    }
    int err = (fflush(out) != 0 || ferror(out));
    if (fclose(out) != 0) {
        err = 1;
    }
    if (err) {
        throw std::runtime_error("write faiss hnsw per-query failed");
    }
    return results;
}

static void write_sweep_array(FILE* f, const std::vector<SweepResult>& rows) {
    fprintf(f, "[");
    for (size_t i = 0; i < rows.size(); i++) {
        const SweepResult& r = rows[i];
        fprintf(f,
                "%s{\"ef_search\":%d,\"recall_at_1\":%.6f,\"recall_at_10\":%.6f,"
                "\"exact_top1_match_rate\":%.6f,\"top1_match_rate\":%.6f,"
                "\"topk_exact_match_rate\":%.6f,\"min_per_query_recall_at_10\":%.6f,"
                "\"query_count\":%lld,\"failed_query_count\":%lld,"
                "\"search_total_us\":%" PRIu64 ",\"search_avg_us\":%.3f,"
                "\"p50_us\":%" PRIu64 ",\"p95_us\":%" PRIu64 ",\"p99_us\":%" PRIu64
                ",\"min_us\":%" PRIu64 ",\"max_us\":%" PRIu64 ",\"qps\":%.3f}",
                i ? "," : "", r.ef_search, r.quality.recall_at_1, r.quality.recall_at_10,
                r.quality.top1_match_rate, r.quality.top1_match_rate,
                r.quality.topk_exact_match_rate, r.quality.min_per_query_recall_at_10,
                r.quality.queries_checked, r.quality.failed_query_count,
                r.latency.total_us, r.latency.avg_us, r.latency.p50_us,
                r.latency.p95_us, r.latency.p99_us, r.latency.min_us,
                r.latency.max_us, r.latency.qps);
    }
    fprintf(f, "]");
}

static void write_result_json(const char* path,
                              const char* backend,
                              const InputsInfo& in,
                              const std::vector<SweepResult>& sweep,
                              uint64_t build_us,
                              long long memory_bytes,
                              const char* memory_source,
                              size_t serialized_bytes,
                              int actual_level0_capacity,
                              const char* per_query_path,
                              const char* per_query_sha,
                              long long per_query_size,
                              const char* insertion_sha,
                              const char* created_at) {
    FILE* f = fopen(path, "w");
    if (!f) {
        throw std::runtime_error("cannot write result json");
    }
    fprintf(f, "{\n  \"schema_version\": 1,\n  \"backend\": ");
    json_write_escaped(f, backend);
    fprintf(f, ",\n  \"dataset_id\": ");
    json_write_escaped(f, in.dataset_id.c_str());
    fprintf(f, ",\n  \"created_at_utc\": ");
    json_write_escaped(f, created_at);
    fprintf(f,
            ",\n  \"params\": {\"record_count\": %lld, \"query_count\": %lld, \"dim\": %lld, "
            "\"topk\": %lld, \"metric\": \"ip_normalized\", \"M\": %d, \"M0\": %d, "
            "\"efConstruction\": %d, \"efSearch_list\": [",
            in.record_count, in.query_count, in.dim, in.topk, HNSW_M, HNSW_M0, HNSW_EFC);
    for (size_t i = 0; i < sweep.size(); i++) {
        fprintf(f, "%s%d", i ? "," : "", sweep[i].ef_search);
    }
    fprintf(f, "], \"warmup_rounds\": %d, \"measured_rounds\": %d},\n", WARMUP_ROUNDS, MEASURED_ROUNDS);
    fprintf(f, "  \"insertion_order\": {\"path\": \"insertion_order.ids\", \"sha256\": ");
    json_write_escaped(f, insertion_sha);
    fprintf(f, "},\n  \"build\": {\"build_us\": %" PRIu64 ", \"build_avg_us_per_record\": %.3f},\n",
            build_us, in.record_count ? (double)build_us / (double)in.record_count : 0.0);
    fprintf(f,
            "  \"index_stats\": {\"memory_bytes\": %lld, \"memory_bytes_source\": ",
            memory_bytes);
    json_write_escaped(f, memory_source);
    fprintf(f,
            ", \"serialized_index_bytes\": %zu, \"serialized_index_bytes_source\": \"storage_estimate\", "
            "\"actual_level0_neighbor_capacity\": %d},\n",
            serialized_bytes, actual_level0_capacity);
    fprintf(f, "  \"sweep\": ");
    write_sweep_array(f, sweep);
    fprintf(f, ",\n  \"outputs\": {\"per_query_jsonl\": {\"path\": ");
    json_write_escaped(f, per_query_path);
    fprintf(f, ", \"sha256\": ");
    json_write_escaped(f, per_query_sha);
    fprintf(f, ", \"size_bytes\": %lld, \"rows\": %lld}}\n}\n",
            per_query_size, (long long)sweep.size() * in.query_count);
    int err = (fflush(f) != 0 || ferror(f));
    if (fclose(f) != 0) {
        err = 1;
    }
    if (err) {
        throw std::runtime_error("write result json failed");
    }
}

static void write_manifest_json(const char* path,
                                const CliArgs& args,
                                const InputsInfo& in,
                                const std::vector<int>& ef_list,
                                uint64_t cache_load_us,
                                const char* insertion_sha,
                                const char* core_result_sha,
                                const char* faiss_flat_result_sha,
                                const char* faiss_hnsw_result_sha,
                                const char* core_pq_sha,
                                const char* faiss_flat_pq_sha,
                                const char* faiss_hnsw_pq_sha,
                                const char* created_at,
                                const char* cpu_model,
                                long logical_cpus,
                                long peak_rss_before,
                                long peak_rss_after) {
    FILE* f = fopen(path, "w");
    if (!f) {
        throw std::runtime_error("cannot write benchmark manifest");
    }
    fprintf(f, "{\n  \"schema_version\": 1,\n  \"dataset_id\": ");
    json_write_escaped(f, in.dataset_id.c_str());
    fprintf(f, ",\n  \"created_at_utc\": ");
    json_write_escaped(f, created_at);
    fprintf(f, ",\n  \"generator\": {\"name\": \"generate_real_embedding_hnsw_faiss_sweep_benchmark\", \"version\": ");
    json_write_escaped(f, GEN_VERSION);
    fprintf(f, "},\n  \"source\": {\"cache_dir\": ");
    json_write_escaped(f, args.cache_dir);
    fprintf(f, ", \"groundtruth_dir\": ");
    json_write_escaped(f, args.groundtruth_dir);
    fprintf(f, ", \"manifest_sha256\": ");
    json_write_escaped(f, in.manifest_sha);
    fprintf(f, ", \"embeddings_sha256\": ");
    json_write_escaped(f, in.emb_sha);
    fprintf(f, ", \"query_embeddings_sha256\": ");
    json_write_escaped(f, in.qemb_sha);
    fprintf(f, ", \"groundtruth_jsonl_sha256\": ");
    json_write_escaped(f, in.gt_sha);
    fprintf(f, ", \"groundtruth_manifest_sha256\": ");
    json_write_escaped(f, in.gt_manifest_sha);
    fprintf(f, "},\n  \"insertion_order\": {\"algorithm\": ");
    json_write_escaped(f, INSERTION_ALGO);
    fprintf(f, ", \"source_revision\": ");
    json_write_escaped(f, args.source_revision);
    fprintf(f, ", \"path\": \"insertion_order.ids\", \"sha256\": ");
    json_write_escaped(f, insertion_sha);
    fprintf(f, "},\n  \"params\": {\"record_count\": %lld, \"query_count\": %lld, \"dim\": %lld, \"topk\": %d, "
               "\"M\": %d, \"M0\": %d, \"efConstruction\": %d, \"efSearch_list\": [",
            in.record_count, in.query_count, in.dim, args.topk, HNSW_M, HNSW_M0, HNSW_EFC);
    for (size_t i = 0; i < ef_list.size(); i++) {
        fprintf(f, "%s%d", i ? "," : "", ef_list[i]);
    }
    fprintf(f,
            "], \"warmup_rounds\": %d, \"measured_rounds\": %d, \"cache_load_us\": %" PRIu64 "},\n"
            "  \"environment\": {\"omp_num_threads\": \"1\", \"openblas_num_threads\": \"1\", ",
            WARMUP_ROUNDS, MEASURED_ROUNDS, cache_load_us);
    fprintf(f,
               "\"mkl_num_threads\": \"1\", \"faiss_thread_count\": 1, \"core_hnsw_v2_search_threads\": 1, "
               "\"cpu_model\": ");
    json_write_escaped(f, cpu_model);
    fprintf(f, ", \"logical_cpus\": %ld, \"compiler\": ", logical_cpus);
    json_write_escaped(f, __VERSION__);
    fprintf(f, ", \"optimization_flags\": \"-O2\", \"cpu_affinity\": \"not_set_by_runner\", "
               "\"peak_rss_before_kb\": %ld, \"peak_rss_after_kb\": %ld},\n",
            peak_rss_before, peak_rss_after);
    char faiss_ver[64];
    faiss_version_string(faiss_ver, sizeof(faiss_ver));
    fprintf(f, "  \"backend_versions\": {\"core_hnsw_v2\": \"kvs_hnsw_v2_index\", \"faiss\": ");
    json_write_escaped(f, faiss_ver);
    fprintf(f, "},\n");
    fprintf(f, "  \"outputs\": {\"core_hnsw_v2_results_json\": {\"path\": \"core_hnsw_v2_results.json\", \"sha256\": ");
    json_write_escaped(f, core_result_sha);
    fprintf(f, "}, \"faiss_flat_results_json\": {\"path\": \"faiss_flat_results.json\", \"sha256\": ");
    json_write_escaped(f, faiss_flat_result_sha);
    fprintf(f, "}, \"faiss_hnsw_results_json\": {\"path\": \"faiss_hnsw_results.json\", \"sha256\": ");
    json_write_escaped(f, faiss_hnsw_result_sha);
    fprintf(f, "}, \"core_hnsw_v2_per_query_jsonl\": {\"path\": \"core_hnsw_v2_per_query.jsonl\", \"sha256\": ");
    json_write_escaped(f, core_pq_sha);
    fprintf(f, "}, \"faiss_flat_per_query_jsonl\": {\"path\": \"faiss_flat_per_query.jsonl\", \"sha256\": ");
    json_write_escaped(f, faiss_flat_pq_sha);
    fprintf(f, "}, \"faiss_hnsw_per_query_jsonl\": {\"path\": \"faiss_hnsw_per_query.jsonl\", \"sha256\": ");
    json_write_escaped(f, faiss_hnsw_pq_sha);
    fprintf(f, "}},\n  \"validation\": {\"cache_valid\": true, \"groundtruth_valid\": true, "
               "\"faiss_flat_strict_parity\": true, \"qrels_used_for_ann_recall\": false}\n}\n");
    int err = (fflush(f) != 0 || ferror(f));
    if (fclose(f) != 0) {
        err = 1;
    }
    if (err) {
        throw std::runtime_error("write benchmark manifest failed");
    }
}

static std::string cpu_model_name() {
    FILE* f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        return "unknown";
    }
    char* line = nullptr;
    size_t cap = 0;
    std::string out = "unknown";
    while (getline(&line, &cap, f) != -1) {
        const char* key = "model name";
        if (strncmp(line, key, strlen(key)) == 0) {
            const char* colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') {
                    colon++;
                }
                out = colon;
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
                    out.pop_back();
                }
                break;
            }
        }
    }
    free(line);
    fclose(f);
    return out;
}

static int stat_size(const char* path, long long* out) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    *out = (long long)st.st_size;
    return 0;
}

static void remove_if_set(const char* p) {
    if (p && p[0]) {
        remove(p);
    }
}

static int cmd_generate(const CliArgs& args) {
    try {
        setenv("OMP_NUM_THREADS", "1", 1);
        setenv("OPENBLAS_NUM_THREADS", "1", 1);
        setenv("MKL_NUM_THREADS", "1", 1);
        omp_set_num_threads(1);

        std::vector<int> ef_list = parse_ef_list(args.ef_search_list);
        char parent[8192];
        dirname_of(args.output_dir, parent, sizeof(parent));
        if (mkdir_p(parent) != 0 || mkdir_p(args.output_dir) != 0) {
            throw std::runtime_error("failed to create output directory");
        }

        char insertion_path[8192], insertion_manifest_path[8192];
        char core_result_path[8192], core_pq_path[8192];
        char flat_result_path[8192], flat_pq_path[8192];
        char faiss_result_path[8192], faiss_pq_path[8192], manifest_path[8192];
        snprintf(insertion_path, sizeof(insertion_path), "%s/insertion_order.ids", args.output_dir);
        snprintf(insertion_manifest_path, sizeof(insertion_manifest_path), "%s/insertion_order.manifest.json", args.output_dir);
        snprintf(core_result_path, sizeof(core_result_path), "%s/core_hnsw_v2_results.json", args.output_dir);
        snprintf(core_pq_path, sizeof(core_pq_path), "%s/core_hnsw_v2_per_query.jsonl", args.output_dir);
        snprintf(flat_result_path, sizeof(flat_result_path), "%s/faiss_flat_results.json", args.output_dir);
        snprintf(flat_pq_path, sizeof(flat_pq_path), "%s/faiss_flat_per_query.jsonl", args.output_dir);
        snprintf(faiss_result_path, sizeof(faiss_result_path), "%s/faiss_hnsw_results.json", args.output_dir);
        snprintf(faiss_pq_path, sizeof(faiss_pq_path), "%s/faiss_hnsw_per_query.jsonl", args.output_dir);
        snprintf(manifest_path, sizeof(manifest_path), "%s/benchmark_manifest.json", args.output_dir);

        int had_existing = path_exists(insertion_path) || path_exists(core_pq_path) ||
                           path_exists(flat_pq_path) || path_exists(faiss_pq_path) ||
                           path_exists(manifest_path);

        InputsInfo in;
        if (verify_inputs(args, &in) != 0) {
            return 1;
        }
        if ((long long)args.topk > in.record_count) {
            throw std::runtime_error("topK exceeds record_count");
        }

        fprintf(stdout, "[inputs] record_count=%lld query_count=%lld dim=%lld topk=%d\n",
                in.record_count, in.query_count, in.dim, args.topk);

        char path[8192];
        uint64_t load_t0 = now_us();
        snprintf(path, sizeof(path), "%s/embeddings.f32", args.cache_dir);
        std::vector<float> base = load_f32(path, (size_t)in.record_count, (int)in.dim);
        snprintf(path, sizeof(path), "%s/query_embeddings.f32", args.cache_dir);
        std::vector<float> queries = load_f32(path, (size_t)in.query_count, (int)in.dim);
        uint64_t cache_load_us = now_us() - load_t0;
        validate_vectors(base, in.record_count, (int)in.dim, "embeddings.f32");
        validate_vectors(queries, in.query_count, (int)in.dim, "query_embeddings.f32");

        std::vector<uint64_t> gt_ids;
        std::vector<float> gt_scores;
        load_groundtruth(args.groundtruth_dir, in.query_count, args.topk, in.record_count, &gt_ids, &gt_scores);

        std::vector<uint64_t> insertion_order =
            generate_insertion_order(args.cache_dir, args.source_revision, in.record_count);

        char insertion_tmp[8192];
        snprintf(insertion_tmp, sizeof(insertion_tmp), "%s/.tmp.insertion_order.%d.ids", args.output_dir, (int)getpid());
        write_insertion_order(insertion_tmp, insertion_order);
        validate_insertion_order_file(insertion_tmp, in.record_count);
        char insertion_sha[65];
        if (sha256_file(insertion_tmp, insertion_sha) != 0) {
            throw std::runtime_error("cannot checksum insertion order");
        }

        char core_pq_tmp[8192], flat_pq_tmp[8192], faiss_pq_tmp[8192];
        char core_result_tmp[8192], flat_result_tmp[8192], faiss_result_tmp[8192];
        char insertion_manifest_tmp[8192], manifest_tmp[8192];
        snprintf(core_pq_tmp, sizeof(core_pq_tmp), "%s/.tmp.core_hnsw_v2_per_query.%d.jsonl", args.output_dir, (int)getpid());
        snprintf(flat_pq_tmp, sizeof(flat_pq_tmp), "%s/.tmp.faiss_flat_per_query.%d.jsonl", args.output_dir, (int)getpid());
        snprintf(faiss_pq_tmp, sizeof(faiss_pq_tmp), "%s/.tmp.faiss_hnsw_per_query.%d.jsonl", args.output_dir, (int)getpid());
        snprintf(core_result_tmp, sizeof(core_result_tmp), "%s/.tmp.core_hnsw_v2_results.%d.json", args.output_dir, (int)getpid());
        snprintf(flat_result_tmp, sizeof(flat_result_tmp), "%s/.tmp.faiss_flat_results.%d.json", args.output_dir, (int)getpid());
        snprintf(faiss_result_tmp, sizeof(faiss_result_tmp), "%s/.tmp.faiss_hnsw_results.%d.json", args.output_dir, (int)getpid());
        snprintf(insertion_manifest_tmp, sizeof(insertion_manifest_tmp), "%s/.tmp.insertion_order.manifest.%d.json", args.output_dir, (int)getpid());
        snprintf(manifest_tmp, sizeof(manifest_tmp), "%s/.tmp.benchmark_manifest.%d.json", args.output_dir, (int)getpid());

        long rss_before = peak_rss_kb();

        void* core_index = nullptr;
        bench_core_hnsw_v2_stats_t core_stats;
        memset(&core_stats, 0, sizeof(core_stats));
        uint64_t core_build_us = 0;
        if (bench_core_hnsw_v2_build((size_t)in.dim, (size_t)in.record_count, base.data(),
                                     insertion_order.data(),
                                     CORE_HNSW_SEED + (uint64_t)in.record_count,
                                     &core_index, &core_stats, &core_build_us) != 0) {
            throw std::runtime_error("core_hnsw_v2 build failed");
        }
        std::vector<SweepResult> core_sweep =
            run_core_sweep(core_index, queries, in.query_count, (int)in.dim, args.topk,
                           in.record_count, gt_ids, ef_list, core_pq_tmp);

        faiss::IndexFlatIP flat((int)in.dim);
        uint64_t flat_build_t0 = now_us();
        flat.add(in.record_count, base.data());
        uint64_t flat_build_us = now_us() - flat_build_t0;
        LatencyStats flat_latency;
        int flat_strict_rank = 0;
        double flat_max_score_diff = 0.0;
        if (run_flat(flat, queries, in.query_count, (int)in.dim, args.topk, in.record_count,
                     gt_ids, gt_scores, flat_pq_tmp, &flat_latency, &flat_strict_rank,
                     &flat_max_score_diff) != 0 || !flat_strict_rank) {
            bench_core_hnsw_v2_destroy(core_index);
            throw std::runtime_error("FAISS flat parity failed");
        }

        faiss::IndexHNSWFlat faiss_hnsw((int)in.dim, HNSW_M, faiss::METRIC_INNER_PRODUCT);
        faiss_hnsw.hnsw.rng = faiss::RandomGenerator(12345);
        faiss_hnsw.hnsw.efConstruction = HNSW_EFC;
        faiss_hnsw.hnsw.efSearch = ef_list.back();
        int actual_level0_capacity = faiss_hnsw.hnsw.nb_neighbors(0);
        std::vector<float> permuted((size_t)in.record_count * (size_t)in.dim);
        for (size_t pos = 0; pos < insertion_order.size(); pos++) {
            uint64_t rid = insertion_order[pos];
            memcpy(permuted.data() + pos * (size_t)in.dim,
                   base.data() + rid * (size_t)in.dim,
                   (size_t)in.dim * sizeof(float));
        }
        uint64_t faiss_build_t0 = now_us();
        faiss_hnsw.add(in.record_count, permuted.data());
        uint64_t faiss_build_us = now_us() - faiss_build_t0;
        std::vector<SweepResult> faiss_sweep =
            run_faiss_hnsw_sweep(faiss_hnsw, queries, insertion_order, in.query_count,
                                 (int)in.dim, args.topk, in.record_count, gt_ids,
                                 ef_list, faiss_pq_tmp);

        long rss_after = peak_rss_kb();

        char created_at[64];
        iso8601_utc_now(created_at, sizeof(created_at));
        char core_pq_sha[65], flat_pq_sha[65], faiss_pq_sha[65];
        long long core_pq_size = 0, flat_pq_size = 0, faiss_pq_size = 0;
        if (sha256_file(core_pq_tmp, core_pq_sha) != 0 ||
            sha256_file(flat_pq_tmp, flat_pq_sha) != 0 ||
            sha256_file(faiss_pq_tmp, faiss_pq_sha) != 0 ||
            stat_size(core_pq_tmp, &core_pq_size) != 0 ||
            stat_size(flat_pq_tmp, &flat_pq_size) != 0 ||
            stat_size(faiss_pq_tmp, &faiss_pq_size) != 0) {
            bench_core_hnsw_v2_destroy(core_index);
            throw std::runtime_error("cannot checksum per-query outputs");
        }

        write_result_json(core_result_tmp, "core_hnsw_v2", in, core_sweep, core_build_us,
                          (long long)core_stats.memory_bytes, "estimated", 0,
                          (int)core_stats.M0, "core_hnsw_v2_per_query.jsonl", core_pq_sha,
                          core_pq_size, insertion_sha, created_at);
        std::vector<SweepResult> flat_as_sweep(1);
        flat_as_sweep[0].ef_search = 0;
        flat_as_sweep[0].latency = flat_latency;
        flat_as_sweep[0].quality.recall_at_1 = 1.0;
        flat_as_sweep[0].quality.recall_at_10 = 1.0;
        flat_as_sweep[0].quality.top1_match_rate = 1.0;
        flat_as_sweep[0].quality.topk_exact_match_rate = 1.0;
        flat_as_sweep[0].quality.min_per_query_recall_at_10 = 1.0;
        flat_as_sweep[0].quality.queries_checked = in.query_count;
        write_result_json(flat_result_tmp, "faiss_index_flat_ip", in, flat_as_sweep, flat_build_us,
                          -1, "unsupported", serialized_index_bytes(flat), -1,
                          "faiss_flat_per_query.jsonl", flat_pq_sha, flat_pq_size,
                          insertion_sha, created_at);
        write_result_json(faiss_result_tmp, "faiss_index_hnsw_flat", in, faiss_sweep, faiss_build_us,
                          -1, "unsupported", serialized_index_bytes(faiss_hnsw),
                          actual_level0_capacity, "faiss_hnsw_per_query.jsonl",
                          faiss_pq_sha, faiss_pq_size, insertion_sha, created_at);

        char core_result_sha[65], flat_result_sha[65], faiss_result_sha[65];
        if (sha256_file(core_result_tmp, core_result_sha) != 0 ||
            sha256_file(flat_result_tmp, flat_result_sha) != 0 ||
            sha256_file(faiss_result_tmp, faiss_result_sha) != 0) {
            bench_core_hnsw_v2_destroy(core_index);
            throw std::runtime_error("cannot checksum result json");
        }

        FILE* im = fopen(insertion_manifest_tmp, "w");
        if (!im) {
            bench_core_hnsw_v2_destroy(core_index);
            throw std::runtime_error("cannot write insertion manifest");
        }
        fprintf(im, "{\n  \"schema_version\": 1,\n  \"algorithm\": ");
        json_write_escaped(im, INSERTION_ALGO);
        fprintf(im, ",\n  \"source_revision\": ");
        json_write_escaped(im, args.source_revision);
        fprintf(im, ",\n  \"record_count\": %lld,\n  \"path\": \"insertion_order.ids\",\n  \"sha256\": ", in.record_count);
        json_write_escaped(im, insertion_sha);
        fprintf(im, ",\n  \"qrels_used\": false\n}\n");
        int im_err = (fflush(im) != 0 || ferror(im));
        if (fclose(im) != 0) {
            im_err = 1;
        }
        if (im_err) {
            bench_core_hnsw_v2_destroy(core_index);
            throw std::runtime_error("write insertion manifest failed");
        }

        std::string cpu = cpu_model_name();
        long logical_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        write_manifest_json(manifest_tmp, args, in, ef_list, cache_load_us, insertion_sha,
                            core_result_sha, flat_result_sha, faiss_result_sha,
                            core_pq_sha, flat_pq_sha, faiss_pq_sha,
                            created_at, cpu.c_str(), logical_cpus,
                            rss_before, rss_after);

        bench_core_hnsw_v2_destroy(core_index);

        if (had_existing) {
            if (same_file_sha256(insertion_tmp, insertion_path) &&
                same_file_sha256(core_pq_tmp, core_pq_path) &&
                same_file_sha256(flat_pq_tmp, flat_pq_path) &&
                same_file_sha256(faiss_pq_tmp, faiss_pq_path)) {
                remove_if_set(insertion_tmp);
                remove_if_set(core_pq_tmp);
                remove_if_set(flat_pq_tmp);
                remove_if_set(faiss_pq_tmp);
                remove_if_set(core_result_tmp);
                remove_if_set(flat_result_tmp);
                remove_if_set(faiss_result_tmp);
                remove_if_set(insertion_manifest_tmp);
                remove_if_set(manifest_tmp);
                fprintf(stdout, "[publish] output_dir=%s status=unchanged core_outputs_identical=true\n", args.output_dir);
                return 0;
            }
            throw std::runtime_error("existing deterministic outputs differ");
        }

        if (rename(insertion_tmp, insertion_path) != 0 ||
            rename(insertion_manifest_tmp, insertion_manifest_path) != 0 ||
            rename(core_pq_tmp, core_pq_path) != 0 ||
            rename(core_result_tmp, core_result_path) != 0 ||
            rename(flat_pq_tmp, flat_pq_path) != 0 ||
            rename(flat_result_tmp, flat_result_path) != 0 ||
            rename(faiss_pq_tmp, faiss_pq_path) != 0 ||
            rename(faiss_result_tmp, faiss_result_path) != 0 ||
            rename(manifest_tmp, manifest_path) != 0) {
            throw std::runtime_error("failed to publish outputs");
        }

        fprintf(stdout, "[flat] strict_parity=yes max_score_abs_diff=%.9g build_us=%" PRIu64 " avg_us=%.3f p95=%" PRIu64 "\n",
                flat_max_score_diff, flat_build_us, flat_latency.avg_us, flat_latency.p95_us);
        for (const SweepResult& r : core_sweep) {
            fprintf(stdout, "[core_hnsw_v2] ef=%d recall@10=%.4f avg_us=%.3f p95=%" PRIu64 " qps=%.3f\n",
                    r.ef_search, r.quality.recall_at_10, r.latency.avg_us, r.latency.p95_us, r.latency.qps);
        }
        for (const SweepResult& r : faiss_sweep) {
            fprintf(stdout, "[faiss_hnsw] ef=%d recall@10=%.4f avg_us=%.3f p95=%" PRIu64 " qps=%.3f\n",
                    r.ef_search, r.quality.recall_at_10, r.latency.avg_us, r.latency.p95_us, r.latency.qps);
        }
        fprintf(stdout, "[publish] output_dir=%s insertion_sha=%s\n", args.output_dir, insertion_sha);
        (void)cache_load_us;
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "[error] %s\n", e.what());
        return 1;
    }
}

static int check_file_sha(const char* path, const char* expected) {
    char actual[65];
    if (sha256_file(path, actual) != 0 || strcmp(actual, expected) != 0) {
        fprintf(stderr, "[fail] sha256 mismatch: %s\n", path);
        return -1;
    }
    return 0;
}

static int cmd_validate(const CliArgs& args) {
    try {
        InputsInfo in;
        if (verify_inputs(args, &in) != 0) {
            return 1;
        }
        char path[8192];
        snprintf(path, sizeof(path), "%s/benchmark_manifest.json", args.output_dir);
        std::string m = read_whole_file(path);
        if (m.empty()) {
            fprintf(stderr, "[fail] cannot read benchmark_manifest.json\n");
            return 1;
        }
        std::string ins = json_extract_object(m.c_str(), "insertion_order");
        char ins_sha[80] = {0};
        if (ins.empty() || json_get_string(ins.c_str(), "sha256", ins_sha, sizeof(ins_sha)) != 0) {
            fprintf(stderr, "[fail] benchmark manifest insertion_order invalid\n");
            return 1;
        }
        snprintf(path, sizeof(path), "%s/insertion_order.ids", args.output_dir);
        validate_insertion_order_file(path, in.record_count);
        if (check_file_sha(path, ins_sha) != 0) {
            return 1;
        }
        std::string outputs = json_extract_object(m.c_str(), "outputs");
        struct Bind { const char* key; const char* file; } binds[] = {
            {"core_hnsw_v2_results_json", "core_hnsw_v2_results.json"},
            {"faiss_flat_results_json", "faiss_flat_results.json"},
            {"faiss_hnsw_results_json", "faiss_hnsw_results.json"},
        };
        for (const Bind& b : binds) {
            std::string block = json_extract_object(outputs.c_str(), b.key);
            char sha[80] = {0};
            if (block.empty() || json_get_string(block.c_str(), "sha256", sha, sizeof(sha)) != 0) {
                fprintf(stderr, "[fail] output block invalid: %s\n", b.key);
                return 1;
            }
            snprintf(path, sizeof(path), "%s/%s", args.output_dir, b.file);
            if (check_file_sha(path, sha) != 0) {
                return 1;
            }
        }
        for (const char* file : {"core_hnsw_v2_per_query.jsonl", "faiss_flat_per_query.jsonl", "faiss_hnsw_per_query.jsonl"}) {
            snprintf(path, sizeof(path), "%s/%s", args.output_dir, file);
            if (!path_exists(path)) {
                fprintf(stderr, "[fail] missing %s\n", file);
                return 1;
            }
        }
        fprintf(stdout, "[validate] PASS output_dir=%s record_count=%lld query_count=%lld topk=%lld\n",
                args.output_dir, in.record_count, in.query_count, in.topk);
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "[validate] FAIL %s\n", e.what());
        return 1;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }
    CliArgs args;
    if (strcmp(argv[1], "generate") == 0 || strcmp(argv[1], "validate") == 0) {
        if (parse_args(argc, argv, &args) != 0) {
            print_usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[1], "generate") == 0) {
            return cmd_generate(args);
        }
        return cmd_validate(args);
    }
    print_usage(argv[0]);
    return 2;
}
