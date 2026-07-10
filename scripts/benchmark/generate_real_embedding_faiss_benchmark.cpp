/*
 * B4-FAISS-index-only-baseline.
 *
 * Consumes the B1 real embedding cache and the B2 core_flat ground truth,
 * then runs two FAISS index-only baselines:
 *   - faiss::IndexFlatIP for exact parity sanity
 *   - faiss::IndexHNSWFlat for HNSW baseline
 *
 * This tool never calls ONNX, never regenerates ground truth, never touches
 * product backend selection, and never uses removed product backend names.
 */

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <inttypes.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include <omp.h>
#include <openssl/evp.h>

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/impl/io.h>
#include <faiss/index_io.h>

static const char* GEN_SOURCE_REL_PATH = "scripts/benchmark/generate_real_embedding_faiss_benchmark.cpp";
static const char* GEN_VERSION = "1";
static const char* DEFAULT_OUT_ROOT = "/tmp/kvlite_vector_benchmark_artifacts/real_embedding";
static const int DEFAULT_TOPK = 10;
static const int FAISS_HNSW_M = 48;
static const int FAISS_HNSW_EFC = 512;
static const int FAISS_HNSW_EFS = 1024;
static const double NORM_TOL = 1e-3;
static const double FLAT_SCORE_TOL = 1e-4;

struct CliArgs {
    const char* dataset_id = nullptr;
    const char* out_root = DEFAULT_OUT_ROOT;
    int topk = DEFAULT_TOPK;
};

struct InputsInfo {
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
    double qps = 0.0;
};

struct FlatQuality {
    int queries_checked = 0;
    int set_match_all = 1;
    int strict_rank_match_all = 1;
    double max_score_abs_diff = 0.0;
};

struct HnswQuality {
    double recall_at_1 = 0.0;
    double recall_at_10 = 0.0;
    double top1_match_rate = 0.0;
    double topk_exact_match_rate = 0.0;
    double min_per_query_recall_at_10 = 1.0;
    int queries_checked = 0;
};

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
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

static int parse_args(int argc, char** argv, CliArgs* args) {
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--dataset-id") == 0 && i + 1 < argc) {
            args->dataset_id = argv[++i];
        } else if (strcmp(argv[i], "--out-root") == 0 && i + 1 < argc) {
            args->out_root = argv[++i];
        } else if (strcmp(argv[i], "--topk") == 0 && i + 1 < argc) {
            args->topk = atoi(argv[++i]);
        } else {
            fprintf(stderr, "[error] unknown argument: %s\n", argv[i]);
            return -1;
        }
    }
    if (!args->dataset_id || args->dataset_id[0] == '\0') {
        fprintf(stderr, "[error] --dataset-id is required\n");
        return -1;
    }
    if (args->topk <= 0) {
        fprintf(stderr, "[error] --topk must be positive\n");
        return -1;
    }
    return 0;
}

static void print_usage(const char* prog) {
    fprintf(stderr,
            "usage:\n"
            "  %s generate --dataset-id <id> [--out-root DIR] [--topk N]\n"
            "  %s validate --dataset-id <id> [--out-root DIR]\n"
            "FAISS index-only benchmark. No ONNX, no ground-truth generation.\n",
            prog, prog);
}

static int verify_output_sha(const std::string& outputs, const char* key,
                             const char* dataset_dir, const char* filename,
                             char out_sha[65]) {
    std::string block = json_extract_object(outputs.c_str(), key);
    char sha[80] = {0};
    char actual[65];
    char path[1200];
    if (block.empty()) {
        fprintf(stderr, "[error] manifest outputs.%s missing\n", key);
        return -1;
    }
    if (json_get_string(block.c_str(), "sha256", sha, sizeof(sha)) != 0 || strlen(sha) != 64) {
        fprintf(stderr, "[error] manifest outputs.%s.sha256 invalid\n", key);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", dataset_dir, filename);
    if (sha256_file(path, actual) != 0 || strcmp(actual, sha) != 0) {
        fprintf(stderr, "[error] %s sha256 mismatch\n", path);
        return -1;
    }
    memcpy(out_sha, sha, 65);
    return 0;
}

static int verify_inputs(const char* dataset_dir, const char* dataset_id, int topk, InputsInfo* out) {
    char path[1200];
    char dataset_id_val[256] = {0};
    char dtype[32] = {0}, byte_order[32] = {0}, metric[64] = {0};
    long long schema_version = 0;

    *out = InputsInfo{};
    snprintf(path, sizeof(path), "%s/manifest.json", dataset_dir);
    std::string b1 = read_whole_file(path);
    if (b1.empty() || sha256_file(path, out->manifest_sha) != 0) {
        fprintf(stderr, "[error] cannot read/checksum B1 manifest\n");
        return -1;
    }
    if (json_get_int64(b1.c_str(), "schema_version", &schema_version) != 0 || schema_version != 1 ||
        json_get_string(b1.c_str(), "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
        strcmp(dataset_id_val, dataset_id) != 0) {
        fprintf(stderr, "[error] B1 manifest identity invalid\n");
        return -1;
    }
    std::string params = json_extract_object(b1.c_str(), "params");
    if (params.empty() ||
        json_get_int64(params.c_str(), "record_count", &out->record_count) != 0 || out->record_count <= 0 ||
        json_get_int64(params.c_str(), "query_count", &out->query_count) != 0 || out->query_count <= 0 ||
        json_get_int64(params.c_str(), "dim", &out->dim) != 0 || out->dim <= 0 ||
        json_get_string(params.c_str(), "dtype", dtype, sizeof(dtype)) != 0 || strcmp(dtype, "float32") != 0 ||
        json_get_string(params.c_str(), "byte_order", byte_order, sizeof(byte_order)) != 0 || strcmp(byte_order, "little") != 0 ||
        json_get_string(params.c_str(), "metric", metric, sizeof(metric)) != 0 || strcmp(metric, "ip_normalized") != 0) {
        fprintf(stderr, "[error] B1 params invalid\n");
        return -1;
    }
    int normalized = 0;
    if (json_get_bool(params.c_str(), "normalized", &normalized) != 0 || !normalized) {
        fprintf(stderr, "[error] B1 normalized != true\n");
        return -1;
    }
    std::string outputs = json_extract_object(b1.c_str(), "outputs");
    if (outputs.empty() ||
        verify_output_sha(outputs, "embeddings_f32", dataset_dir, "embeddings.f32", out->emb_sha) != 0 ||
        verify_output_sha(outputs, "query_embeddings_f32", dataset_dir, "query_embeddings.f32", out->qemb_sha) != 0 ||
        verify_output_sha(outputs, "records_jsonl", dataset_dir, "records.jsonl", out->records_sha) != 0 ||
        verify_output_sha(outputs, "queries_jsonl", dataset_dir, "queries.jsonl", out->queries_sha) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/groundtruth.manifest.json", dataset_dir);
    std::string gtm = read_whole_file(path);
    if (gtm.empty() || sha256_file(path, out->gt_manifest_sha) != 0) {
        fprintf(stderr, "[error] cannot read/checksum B2 groundtruth manifest\n");
        return -1;
    }
    if (json_get_int64(gtm.c_str(), "schema_version", &schema_version) != 0 || schema_version != 1 ||
        json_get_string(gtm.c_str(), "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
        strcmp(dataset_id_val, dataset_id) != 0) {
        fprintf(stderr, "[error] B2 groundtruth manifest identity invalid\n");
        return -1;
    }
    std::string gtsrc = json_extract_object(gtm.c_str(), "source");
    char src_manifest_sha[80] = {0};
    if (gtsrc.empty() ||
        json_get_string(gtsrc.c_str(), "manifest_sha256", src_manifest_sha, sizeof(src_manifest_sha)) != 0 ||
        strcmp(src_manifest_sha, out->manifest_sha) != 0) {
        fprintf(stderr, "[error] B2 ground truth is not bound to this B1 manifest\n");
        return -1;
    }
    std::string gtparams = json_extract_object(gtm.c_str(), "params");
    long long gt_rc = 0, gt_qc = 0, gt_dim = 0, gt_topk = 0;
    char gt_metric[64] = {0};
    if (gtparams.empty() ||
        json_get_int64(gtparams.c_str(), "record_count", &gt_rc) != 0 || gt_rc != out->record_count ||
        json_get_int64(gtparams.c_str(), "query_count", &gt_qc) != 0 || gt_qc != out->query_count ||
        json_get_int64(gtparams.c_str(), "dim", &gt_dim) != 0 || gt_dim != out->dim ||
        json_get_int64(gtparams.c_str(), "topk", &gt_topk) != 0 || gt_topk != topk ||
        json_get_string(gtparams.c_str(), "metric", gt_metric, sizeof(gt_metric)) != 0 ||
        strcmp(gt_metric, "ip_normalized") != 0) {
        fprintf(stderr, "[error] B2 params inconsistent with B1/topk\n");
        return -1;
    }
    std::string gtout = json_extract_object(gtm.c_str(), "outputs");
    if (gtout.empty() ||
        verify_output_sha(gtout, "groundtruth_jsonl", dataset_dir, "groundtruth.jsonl", out->gt_sha) != 0) {
        return -1;
    }
    out->topk = topk;
    return 0;
}

static void load_groundtruth(const char* dataset_dir, long long query_count, int topk,
                             long long record_count, std::vector<uint64_t>* ids,
                             std::vector<float>* scores) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/groundtruth.jsonl", dataset_dir);
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
    long long lineno = 0;
    while ((len = getline(&line, &cap, f)) != -1) {
        (void)len;
        long long query_row = -1, line_topk = -1;
        lineno++;
        if (json_get_int64(line, "query_row", &query_row) != 0 || query_row < 0 || query_row >= query_count ||
            json_get_int64(line, "topk", &line_topk) != 0 || line_topk != topk) {
            free(line);
            fclose(f);
            throw std::runtime_error("bad groundtruth line");
        }
        if (present[(size_t)query_row]) {
            free(line);
            fclose(f);
            throw std::runtime_error("duplicate groundtruth query_row");
        }
        const char* p = strstr(line, "\"neighbors\"");
        if (!p) {
            free(line);
            fclose(f);
            throw std::runtime_error("groundtruth missing neighbors");
        }
        for (int r = 0; r < topk; r++) {
            const char* rk = strstr(p, "\"rank\"");
            if (!rk) {
                free(line);
                fclose(f);
                throw std::runtime_error("groundtruth too few neighbors");
            }
            long long rank_v = -1, rid_v = -1;
            double score_v = 0.0;
            if (json_get_int64(rk, "rank", &rank_v) != 0 || rank_v != r + 1 ||
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
    std::vector<uint64_t> sorted = samples;
    for (uint64_t v : samples) {
        st.total_us += v;
    }
    std::sort(sorted.begin(), sorted.end());
    if (!samples.empty()) {
        st.avg_us = (double)st.total_us / (double)samples.size();
        st.p50_us = percentile(sorted, 50.0);
        st.p95_us = percentile(sorted, 95.0);
        st.p99_us = percentile(sorted, 99.0);
        st.qps = st.total_us ? (double)samples.size() * 1000000.0 / (double)st.total_us : 0.0;
    }
    return st;
}

static size_t serialized_index_bytes(const faiss::Index& index) {
    faiss::VectorIOWriter writer;
    faiss::write_index(&index, &writer);
    return writer.data.size();
}

static void write_hit_array(FILE* f, const faiss::idx_t* labels, const float* scores, int topk) {
    fprintf(f, "\"ann\":[");
    for (int j = 0; j < topk; j++) {
        fprintf(f, "%s%lld", j == 0 ? "" : ",", (long long)labels[j]);
    }
    fprintf(f, "],\"hits\":[");
    for (int j = 0; j < topk; j++) {
        fprintf(f, "%s{\"rank\":%d,\"record_id\":%lld,\"score\":%.9g}",
                j == 0 ? "" : ",", j + 1, (long long)labels[j], (double)scores[j]);
    }
    fprintf(f, "]");
}

static void validate_faiss_hits(const faiss::idx_t* labels, const float* scores,
                                int topk, long long record_count) {
    for (int j = 0; j < topk; j++) {
        if (labels[j] < 0 || labels[j] >= record_count || !std::isfinite(scores[j])) {
            throw std::runtime_error("FAISS returned invalid label/score");
        }
        if (j > 0 && scores[j] > scores[j - 1] + 1e-7f) {
            throw std::runtime_error("FAISS scores are not descending");
        }
        for (int k = 0; k < j; k++) {
            if (labels[k] == labels[j]) {
                throw std::runtime_error("FAISS returned duplicate label");
            }
        }
    }
}

static int set_match_topk(const faiss::idx_t* labels, const uint64_t* gt, int topk) {
    std::vector<long long> a, b;
    a.reserve((size_t)topk);
    b.reserve((size_t)topk);
    for (int i = 0; i < topk; i++) {
        a.push_back((long long)labels[i]);
        b.push_back((long long)gt[i]);
    }
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

static int run_flat(faiss::IndexFlatIP& flat, const std::vector<float>& queries,
                    long long qcount, int dim, int topk, long long record_count,
                    const std::vector<uint64_t>& gt_ids, const std::vector<float>& gt_scores,
                    const char* out_tmp, LatencyStats* latency, FlatQuality* quality) {
    FILE* out = fopen(out_tmp, "w");
    if (!out) {
        fprintf(stderr, "[error] cannot open flat per-query temp\n");
        return -1;
    }
    std::vector<faiss::idx_t> labels((size_t)topk);
    std::vector<float> scores((size_t)topk);
    std::vector<uint64_t> samples((size_t)qcount);
    *quality = FlatQuality{};
    quality->set_match_all = 1;
    quality->strict_rank_match_all = 1;

    for (long long q = 0; q < qcount; q++) {
        const float* qv = queries.data() + q * dim;
        const uint64_t* gt = gt_ids.data() + (size_t)q * (size_t)topk;
        const float* gs = gt_scores.data() + (size_t)q * (size_t)topk;
        uint64_t t0 = now_us();
        flat.search(1, qv, topk, scores.data(), labels.data());
        samples[(size_t)q] = now_us() - t0;
        validate_faiss_hits(labels.data(), scores.data(), topk, record_count);

        int set_match = set_match_topk(labels.data(), gt, topk);
        int strict_match = 1;
        double max_abs = 0.0;
        for (int j = 0; j < topk; j++) {
            if ((uint64_t)labels[j] != gt[j]) {
                strict_match = 0;
            }
            double diff = fabs((double)scores[j] - (double)gs[j]);
            if (diff > max_abs) {
                max_abs = diff;
            }
        }
        if (!set_match) {
            quality->set_match_all = 0;
        }
        if (!strict_match || max_abs > FLAT_SCORE_TOL) {
            quality->strict_rank_match_all = 0;
        }
        if (max_abs > quality->max_score_abs_diff) {
            quality->max_score_abs_diff = max_abs;
        }

        char qid[32];
        snprintf(qid, sizeof(qid), "q%06lld", q);
        fprintf(out, "{\"query_row\":%lld,\"query_id\":", q);
        json_write_escaped(out, qid);
        fprintf(out, ",\"hit_count\":%d,", topk);
        write_hit_array(out, labels.data(), scores.data(), topk);
        fprintf(out, ",\"record_id_set_match\":%s,\"strict_rank_match\":%s,\"max_score_abs_diff\":%.9g}\n",
                set_match ? "true" : "false",
                (strict_match && max_abs <= FLAT_SCORE_TOL) ? "true" : "false",
                max_abs);
        if (ferror(out)) {
            fclose(out);
            return -1;
        }
        quality->queries_checked++;
    }
    int err = (fflush(out) != 0 || ferror(out));
    if (fclose(out) != 0) {
        err = 1;
    }
    if (err) {
        return -1;
    }
    *latency = make_latency_stats(samples);
    return 0;
}

static int run_hnsw(faiss::IndexHNSWFlat& hnsw, const std::vector<float>& queries,
                    long long qcount, int dim, int topk, long long record_count,
                    const std::vector<uint64_t>& gt_ids, const char* out_tmp,
                    LatencyStats* latency, HnswQuality* quality) {
    FILE* out = fopen(out_tmp, "w");
    if (!out) {
        fprintf(stderr, "[error] cannot open hnsw per-query temp\n");
        return -1;
    }
    std::vector<faiss::idx_t> labels((size_t)topk);
    std::vector<float> scores((size_t)topk);
    std::vector<uint64_t> samples((size_t)qcount);
    double sum_r1 = 0.0, sum_r10 = 0.0, sum_top1 = 0.0, sum_exact = 0.0;
    double min_r10 = 1.0;

    for (long long q = 0; q < qcount; q++) {
        const float* qv = queries.data() + q * dim;
        const uint64_t* gt = gt_ids.data() + (size_t)q * (size_t)topk;
        uint64_t t0 = now_us();
        hnsw.search(1, qv, topk, scores.data(), labels.data());
        samples[(size_t)q] = now_us() - t0;
        validate_faiss_hits(labels.data(), scores.data(), topk, record_count);

        int inter = 0, r1_found = 0;
        for (int r = 0; r < topk; r++) {
            for (int j = 0; j < topk; j++) {
                if ((uint64_t)labels[j] == gt[r]) {
                    inter++;
                    break;
                }
            }
        }
        for (int j = 0; j < topk; j++) {
            if ((uint64_t)labels[j] == gt[0]) {
                r1_found = 1;
                break;
            }
        }
        int top1 = ((uint64_t)labels[0] == gt[0]) ? 1 : 0;
        int exact = 1;
        for (int j = 0; j < topk; j++) {
            if ((uint64_t)labels[j] != gt[j]) {
                exact = 0;
                break;
            }
        }
        double r10 = (double)inter / (double)topk;
        sum_r1 += r1_found;
        sum_r10 += r10;
        sum_top1 += top1;
        sum_exact += exact;
        if (r10 < min_r10) {
            min_r10 = r10;
        }

        char qid[32];
        snprintf(qid, sizeof(qid), "q%06lld", q);
        fprintf(out, "{\"query_row\":%lld,\"query_id\":", q);
        json_write_escaped(out, qid);
        fprintf(out, ",\"hit_count\":%d,", topk);
        write_hit_array(out, labels.data(), scores.data(), topk);
        fprintf(out, ",\"recall_at_1\":%d,\"recall_at_10\":%.6f,\"top1_match\":%d,\"exact_match\":%d}\n",
                r1_found, r10, top1, exact);
        if (ferror(out)) {
            fclose(out);
            return -1;
        }
        quality->queries_checked++;
    }
    int err = (fflush(out) != 0 || ferror(out));
    if (fclose(out) != 0) {
        err = 1;
    }
    if (err) {
        return -1;
    }

    quality->recall_at_1 = sum_r1 / (double)qcount;
    quality->recall_at_10 = sum_r10 / (double)qcount;
    quality->top1_match_rate = sum_top1 / (double)qcount;
    quality->topk_exact_match_rate = sum_exact / (double)qcount;
    quality->min_per_query_recall_at_10 = min_r10;
    *latency = make_latency_stats(samples);
    return 0;
}

static int stat_size(const char* path, long long* out) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    *out = (long long)st.st_size;
    return 0;
}

static void remove_if_set(const char* path) {
    if (path && path[0]) {
        remove(path);
    }
}

static int cmd_generate(const CliArgs& args) {
    uint64_t total_start = now_us();
    char dataset_dir[1024];
    char result_path[1200], hnsw_path[1200], flat_path[1200];
    char result_tmp[1200], hnsw_tmp[1200], flat_tmp[1200];
    snprintf(dataset_dir, sizeof(dataset_dir), "%s/%s", args.out_root, args.dataset_id);
    snprintf(result_path, sizeof(result_path), "%s/faiss_benchmark_result.json", dataset_dir);
    snprintf(hnsw_path, sizeof(hnsw_path), "%s/faiss_hnsw_per_query.jsonl", dataset_dir);
    snprintf(flat_path, sizeof(flat_path), "%s/faiss_flat_per_query.jsonl", dataset_dir);
    snprintf(result_tmp, sizeof(result_tmp), "%s/.tmp.faiss_benchmark_result.%d.json", dataset_dir, (int)getpid());
    snprintf(hnsw_tmp, sizeof(hnsw_tmp), "%s/.tmp.faiss_hnsw_per_query.%d.jsonl", dataset_dir, (int)getpid());
    snprintf(flat_tmp, sizeof(flat_tmp), "%s/.tmp.faiss_flat_per_query.%d.jsonl", dataset_dir, (int)getpid());

    if (!path_exists(dataset_dir)) {
        fprintf(stderr, "[error] dataset directory does not exist: %s\n", dataset_dir);
        return 1;
    }
    if (path_exists(result_path) || path_exists(hnsw_path) || path_exists(flat_path)) {
        fprintf(stderr, "[error] B4 result already exists in %s (refusing to overwrite)\n", dataset_dir);
        return 1;
    }

    omp_set_num_threads(1);
    const int thread_count = 1;

    InputsInfo in;
    if (verify_inputs(dataset_dir, args.dataset_id, args.topk, &in) != 0) {
        return 1;
    }
    if ((long long)args.topk > in.record_count) {
        fprintf(stderr, "[error] topk exceeds record_count\n");
        return 1;
    }
    fprintf(stdout, "[inputs] record_count=%lld query_count=%lld dim=%lld topk=%d (B1 cache + B2 ground truth verified)\n",
            in.record_count, in.query_count, in.dim, args.topk);

    char path[1200];
    uint64_t t0 = now_us();
    snprintf(path, sizeof(path), "%s/embeddings.f32", dataset_dir);
    std::vector<float> base = load_f32(path, (size_t)in.record_count, (int)in.dim);
    snprintf(path, sizeof(path), "%s/query_embeddings.f32", dataset_dir);
    std::vector<float> queries = load_f32(path, (size_t)in.query_count, (int)in.dim);
    uint64_t cache_load_us = now_us() - t0;
    validate_vectors(base, in.record_count, (int)in.dim, "embeddings.f32");
    validate_vectors(queries, in.query_count, (int)in.dim, "query_embeddings.f32");

    std::vector<uint64_t> gt_ids;
    std::vector<float> gt_scores;
    load_groundtruth(dataset_dir, in.query_count, args.topk, in.record_count, &gt_ids, &gt_scores);

    faiss::IndexFlatIP flat((int)in.dim);
    t0 = now_us();
    flat.add(in.record_count, base.data());
    uint64_t flat_build_us = now_us() - t0;

    faiss::IndexHNSWFlat hnsw((int)in.dim, FAISS_HNSW_M, faiss::METRIC_INNER_PRODUCT);
    hnsw.hnsw.efConstruction = FAISS_HNSW_EFC;
    hnsw.hnsw.efSearch = FAISS_HNSW_EFS;
    int actual_level0_capacity = hnsw.hnsw.nb_neighbors(0);
    t0 = now_us();
    hnsw.add(in.record_count, base.data());
    uint64_t hnsw_build_us = now_us() - t0;

    LatencyStats flat_latency, hnsw_latency;
    FlatQuality flat_quality;
    HnswQuality hnsw_quality;
    if (run_flat(flat, queries, in.query_count, (int)in.dim, args.topk, in.record_count,
                 gt_ids, gt_scores, flat_tmp, &flat_latency, &flat_quality) != 0) {
        remove_if_set(flat_tmp);
        return 1;
    }
    if (run_hnsw(hnsw, queries, in.query_count, (int)in.dim, args.topk, in.record_count,
                 gt_ids, hnsw_tmp, &hnsw_latency, &hnsw_quality) != 0) {
        remove_if_set(flat_tmp);
        remove_if_set(hnsw_tmp);
        return 1;
    }

    char flat_sha[65], hnsw_sha[65], source_sha[65];
    long long flat_size = 0, hnsw_size = 0;
    if (sha256_file(flat_tmp, flat_sha) != 0 || sha256_file(hnsw_tmp, hnsw_sha) != 0 ||
        stat_size(flat_tmp, &flat_size) != 0 || stat_size(hnsw_tmp, &hnsw_size) != 0) {
        remove_if_set(flat_tmp);
        remove_if_set(hnsw_tmp);
        return 1;
    }
    int have_source_sha = sha256_file(GEN_SOURCE_REL_PATH, source_sha) == 0;
    size_t flat_serialized = serialized_index_bytes(flat);
    size_t hnsw_serialized = serialized_index_bytes(hnsw);

    char created_at[64];
    iso8601_utc_now(created_at, sizeof(created_at));
    FILE* mf = fopen(result_tmp, "w");
    if (!mf) {
        remove_if_set(flat_tmp);
        remove_if_set(hnsw_tmp);
        return 1;
    }
    uint64_t total_us = now_us() - total_start;
    fprintf(mf, "{\n");
    fprintf(mf, "  \"schema_version\": 1,\n");
    fprintf(mf, "  \"dataset_id\": ");
    json_write_escaped(mf, args.dataset_id);
    fprintf(mf, ",\n  \"backend\": \"faiss_index_only\",\n  \"created_at_utc\": ");
    json_write_escaped(mf, created_at);
    fprintf(mf, ",\n  \"generator\": {\"name\": \"generate_real_embedding_faiss_benchmark\", \"version\": ");
    json_write_escaped(mf, GEN_VERSION);
    fprintf(mf, ", \"source_sha256\": ");
    if (have_source_sha) {
        json_write_escaped(mf, source_sha);
    } else {
        fprintf(mf, "null");
    }
    fprintf(mf, "},\n");
    fprintf(mf, "  \"source\": {\"manifest_sha256\": ");
    json_write_escaped(mf, in.manifest_sha);
    fprintf(mf, ", \"embeddings_sha256\": ");
    json_write_escaped(mf, in.emb_sha);
    fprintf(mf, ", \"query_embeddings_sha256\": ");
    json_write_escaped(mf, in.qemb_sha);
    fprintf(mf, ", \"records_jsonl_sha256\": ");
    json_write_escaped(mf, in.records_sha);
    fprintf(mf, ", \"queries_jsonl_sha256\": ");
    json_write_escaped(mf, in.queries_sha);
    fprintf(mf, ", \"groundtruth_jsonl_sha256\": ");
    json_write_escaped(mf, in.gt_sha);
    fprintf(mf, ", \"groundtruth_manifest_sha256\": ");
    json_write_escaped(mf, in.gt_manifest_sha);
    fprintf(mf, "},\n");
    fprintf(mf,
            "  \"params\": {\"dim\": %lld, \"metric\": \"ip_normalized\", \"faiss_metric\": \"METRIC_INNER_PRODUCT\", "
            "\"count\": %lld, \"query_count\": %lld, \"topk\": %d, \"thread_count\": %d, "
            "\"hnsw\": {\"index\": \"IndexHNSWFlat\", \"requested_M\": %d, \"actual_level0_neighbor_capacity\": %d, "
            "\"efConstruction\": %d, \"efSearch\": %d}, \"flat\": {\"index\": \"IndexFlatIP\"}},\n",
            in.dim, in.record_count, in.query_count, args.topk, thread_count,
            FAISS_HNSW_M, actual_level0_capacity, FAISS_HNSW_EFC, FAISS_HNSW_EFS);
    fprintf(mf,
            "  \"flat_quality\": {\"record_id_set_match_all\": %s, \"strict_rank_match_all\": %s, "
            "\"score_tolerance\": %.9g, \"max_score_abs_diff\": %.9g, \"queries_checked\": %d},\n",
            flat_quality.set_match_all ? "true" : "false",
            flat_quality.strict_rank_match_all ? "true" : "false",
            FLAT_SCORE_TOL, flat_quality.max_score_abs_diff, flat_quality.queries_checked);
    fprintf(mf,
            "  \"hnsw_quality\": {\"recall_at_1\": %.6f, \"recall_at_10\": %.6f, \"top1_match_rate\": %.6f, "
            "\"topk_exact_match_rate\": %.6f, \"min_per_query_recall_at_10\": %.6f, \"queries_checked\": %d},\n",
            hnsw_quality.recall_at_1, hnsw_quality.recall_at_10, hnsw_quality.top1_match_rate,
            hnsw_quality.topk_exact_match_rate, hnsw_quality.min_per_query_recall_at_10,
            hnsw_quality.queries_checked);
    fprintf(mf,
            "  \"performance\": {\"cache_load_us\": %" PRIu64 ", \"flat_build_us\": %" PRIu64
            ", \"flat_search_total_us\": %" PRIu64 ", \"flat_search_avg_us\": %.3f, "
            "\"flat_search_p50_us\": %" PRIu64 ", \"flat_search_p95_us\": %" PRIu64
            ", \"flat_search_p99_us\": %" PRIu64 ", \"flat_qps\": %.3f, "
            "\"hnsw_build_us\": %" PRIu64 ", \"hnsw_search_total_us\": %" PRIu64
            ", \"hnsw_search_avg_us\": %.3f, \"hnsw_search_p50_us\": %" PRIu64
            ", \"hnsw_search_p95_us\": %" PRIu64 ", \"hnsw_search_p99_us\": %" PRIu64
            ", \"hnsw_qps\": %.3f},\n",
            cache_load_us, flat_build_us, flat_latency.total_us, flat_latency.avg_us,
            flat_latency.p50_us, flat_latency.p95_us, flat_latency.p99_us, flat_latency.qps,
            hnsw_build_us, hnsw_latency.total_us, hnsw_latency.avg_us,
            hnsw_latency.p50_us, hnsw_latency.p95_us, hnsw_latency.p99_us, hnsw_latency.qps);
    fprintf(mf,
            "  \"index_stats\": {\"flat_ntotal\": %lld, \"hnsw_ntotal\": %lld, \"requested_M\": %d, "
            "\"actual_level0_neighbor_capacity\": %d, \"efConstruction\": %d, \"efSearch\": %d, "
            "\"thread_count\": %d, \"flat_serialized_index_bytes\": %zu, \"hnsw_serialized_index_bytes\": %zu, "
            "\"storage_metric_source\": \"faiss_serialized_index_size\", \"storage_metric_kind\": \"storage_estimate\", "
            "\"memory_bytes\": null, \"memory_bytes_source\": \"unsupported\"},\n",
            (long long)flat.ntotal, (long long)hnsw.ntotal, FAISS_HNSW_M,
            actual_level0_capacity, FAISS_HNSW_EFC, FAISS_HNSW_EFS, thread_count,
            flat_serialized, hnsw_serialized);
    fprintf(mf, "  \"outputs\": {\"faiss_hnsw_per_query_jsonl\": {\"path\": \"faiss_hnsw_per_query.jsonl\", \"sha256\": ");
    json_write_escaped(mf, hnsw_sha);
    fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %lld}, \"faiss_flat_per_query_jsonl\": {\"path\": \"faiss_flat_per_query.jsonl\", \"sha256\": ",
            hnsw_size, in.query_count);
    json_write_escaped(mf, flat_sha);
    fprintf(mf, ", \"size_bytes\": %lld, \"rows\": %lld}},\n", flat_size, in.query_count);
    fprintf(mf, "  \"timing_us\": {\"total_us\": %" PRIu64 "}\n", total_us);
    fprintf(mf, "}\n");
    int mf_err = (fflush(mf) != 0 || ferror(mf));
    if (fclose(mf) != 0) {
        mf_err = 1;
    }
    if (mf_err) {
        remove_if_set(flat_tmp);
        remove_if_set(hnsw_tmp);
        remove_if_set(result_tmp);
        return 1;
    }

    if (rename(flat_tmp, flat_path) != 0 ||
        rename(hnsw_tmp, hnsw_path) != 0 ||
        rename(result_tmp, result_path) != 0) {
        fprintf(stderr, "[error] failed to publish FAISS result: %s\n", strerror(errno));
        remove_if_set(flat_tmp);
        remove_if_set(hnsw_tmp);
        remove_if_set(result_tmp);
        remove_if_set(flat_path);
        remove_if_set(hnsw_path);
        remove_if_set(result_path);
        return 1;
    }

    fprintf(stdout, "[flat] exact_set=%s strict_rank=%s max_score_abs_diff=%.9g build_us=%" PRIu64
                    " search_avg_us=%.3f p95=%" PRIu64 "\n",
            flat_quality.set_match_all ? "yes" : "no",
            flat_quality.strict_rank_match_all ? "yes" : "no",
            flat_quality.max_score_abs_diff, flat_build_us, flat_latency.avg_us, flat_latency.p95_us);
    fprintf(stdout, "[hnsw] recall@1=%.4f recall@10=%.4f top1=%.4f exact=%.4f min_recall@10=%.4f "
                    "build_us=%" PRIu64 " search_avg_us=%.3f p95=%" PRIu64 "\n",
            hnsw_quality.recall_at_1, hnsw_quality.recall_at_10, hnsw_quality.top1_match_rate,
            hnsw_quality.topk_exact_match_rate, hnsw_quality.min_per_query_recall_at_10,
            hnsw_build_us, hnsw_latency.avg_us, hnsw_latency.p95_us);
    fprintf(stdout, "[publish] dataset_id=%s result=%s\n", args.dataset_id, result_path);
    return 0;
}

static int check_file_sha(const char* dataset_dir, const char* filename, const char* expect_sha) {
    char path[1200], actual[65];
    snprintf(path, sizeof(path), "%s/%s", dataset_dir, filename);
    if (sha256_file(path, actual) != 0 || strcmp(actual, expect_sha) != 0) {
        fprintf(stderr, "[fail] %s sha256 mismatch\n", filename);
        return -1;
    }
    return 0;
}

static long long count_lines(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    long long lines = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            lines++;
        }
    }
    fclose(f);
    return lines;
}

static int validate_output_block(const char* dataset_dir, const std::string& outputs,
                                 const char* key, const char* filename, long long query_count) {
    std::string block = json_extract_object(outputs.c_str(), key);
    char sha[80] = {0};
    long long size = -1, rows = -1;
    char path[1200];
    char actual[65];
    if (block.empty() ||
        json_get_string(block.c_str(), "sha256", sha, sizeof(sha)) != 0 || strlen(sha) != 64 ||
        json_get_int64(block.c_str(), "size_bytes", &size) != 0 || size <= 0 ||
        json_get_int64(block.c_str(), "rows", &rows) != 0 || rows != query_count) {
        fprintf(stderr, "[fail] outputs.%s invalid\n", key);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", dataset_dir, filename);
    long long actual_size = 0;
    if (stat_size(path, &actual_size) != 0 || actual_size != size ||
        sha256_file(path, actual) != 0 || strcmp(actual, sha) != 0 ||
        count_lines(path) != query_count) {
        fprintf(stderr, "[fail] %s output verification failed\n", filename);
        return -1;
    }
    return 0;
}

static int cmd_validate(const CliArgs& args) {
    char dataset_dir[1024], result_path[1200];
    snprintf(dataset_dir, sizeof(dataset_dir), "%s/%s", args.out_root, args.dataset_id);
    snprintf(result_path, sizeof(result_path), "%s/faiss_benchmark_result.json", dataset_dir);
    std::string res = read_whole_file(result_path);
    if (res.empty()) {
        fprintf(stderr, "[fail] cannot read FAISS result manifest\n");
        return 1;
    }
    int ok = 1;
    long long schema_version = 0, count = 0, qcount = 0, dim = 0, topk = 0, thread_count = 0;
    char dataset_id_val[256] = {0}, backend[64] = {0}, metric[64] = {0};
    if (json_get_int64(res.c_str(), "schema_version", &schema_version) != 0 || schema_version != 1 ||
        json_get_string(res.c_str(), "dataset_id", dataset_id_val, sizeof(dataset_id_val)) != 0 ||
        strcmp(dataset_id_val, args.dataset_id) != 0 ||
        json_get_string(res.c_str(), "backend", backend, sizeof(backend)) != 0 ||
        strcmp(backend, "faiss_index_only") != 0) {
        fprintf(stderr, "[fail] result identity invalid\n");
        ok = 0;
    }
    std::string params = json_extract_object(res.c_str(), "params");
    if (params.empty() ||
        json_get_int64(params.c_str(), "count", &count) != 0 || count <= 0 ||
        json_get_int64(params.c_str(), "query_count", &qcount) != 0 || qcount <= 0 ||
        json_get_int64(params.c_str(), "dim", &dim) != 0 || dim <= 0 ||
        json_get_int64(params.c_str(), "topk", &topk) != 0 || topk <= 0 ||
        json_get_int64(params.c_str(), "thread_count", &thread_count) != 0 || thread_count != 1 ||
        json_get_string(params.c_str(), "metric", metric, sizeof(metric)) != 0 ||
        strcmp(metric, "ip_normalized") != 0) {
        fprintf(stderr, "[fail] params invalid\n");
        ok = 0;
    }

    std::string source = json_extract_object(res.c_str(), "source");
    if (source.empty()) {
        fprintf(stderr, "[fail] missing source block\n");
        ok = 0;
    } else {
        struct Binding { const char* key; const char* file; };
        Binding binds[] = {
            {"manifest_sha256", "manifest.json"},
            {"embeddings_sha256", "embeddings.f32"},
            {"query_embeddings_sha256", "query_embeddings.f32"},
            {"records_jsonl_sha256", "records.jsonl"},
            {"queries_jsonl_sha256", "queries.jsonl"},
            {"groundtruth_jsonl_sha256", "groundtruth.jsonl"},
            {"groundtruth_manifest_sha256", "groundtruth.manifest.json"},
        };
        for (const auto& b : binds) {
            char sha[80] = {0};
            if (json_get_string(source.c_str(), b.key, sha, sizeof(sha)) != 0 || strlen(sha) != 64 ||
                check_file_sha(dataset_dir, b.file, sha) != 0) {
                ok = 0;
            }
        }
    }

    std::string fq = json_extract_object(res.c_str(), "flat_quality");
    int set_match = 0, strict_rank = 0;
    long long flat_q = 0;
    double max_diff = -1.0;
    if (fq.empty() ||
        json_get_bool(fq.c_str(), "record_id_set_match_all", &set_match) != 0 || !set_match ||
        json_get_bool(fq.c_str(), "strict_rank_match_all", &strict_rank) != 0 || !strict_rank ||
        json_get_double(fq.c_str(), "max_score_abs_diff", &max_diff) != 0 || max_diff < 0.0 || max_diff > FLAT_SCORE_TOL ||
        json_get_int64(fq.c_str(), "queries_checked", &flat_q) != 0 || flat_q != qcount) {
        fprintf(stderr, "[fail] flat_quality invalid\n");
        ok = 0;
    }

    std::string hq = json_extract_object(res.c_str(), "hnsw_quality");
    double vals[5] = {-1, -1, -1, -1, -1};
    long long hq_checked = 0;
    if (hq.empty() ||
        json_get_double(hq.c_str(), "recall_at_1", &vals[0]) != 0 ||
        json_get_double(hq.c_str(), "recall_at_10", &vals[1]) != 0 ||
        json_get_double(hq.c_str(), "top1_match_rate", &vals[2]) != 0 ||
        json_get_double(hq.c_str(), "topk_exact_match_rate", &vals[3]) != 0 ||
        json_get_double(hq.c_str(), "min_per_query_recall_at_10", &vals[4]) != 0 ||
        json_get_int64(hq.c_str(), "queries_checked", &hq_checked) != 0 || hq_checked != qcount) {
        fprintf(stderr, "[fail] hnsw_quality invalid\n");
        ok = 0;
    } else {
        for (double v : vals) {
            if (!(v >= 0.0 && v <= 1.0)) {
                fprintf(stderr, "[fail] hnsw quality out of range\n");
                ok = 0;
            }
        }
    }

    std::string perf = json_extract_object(res.c_str(), "performance");
    double flat_avg = -1, hnsw_avg = -1, flat_qps = -1, hnsw_qps = -1;
    if (perf.empty() ||
        json_get_double(perf.c_str(), "flat_search_avg_us", &flat_avg) != 0 || flat_avg <= 0 ||
        json_get_double(perf.c_str(), "hnsw_search_avg_us", &hnsw_avg) != 0 || hnsw_avg <= 0 ||
        json_get_double(perf.c_str(), "flat_qps", &flat_qps) != 0 || flat_qps <= 0 ||
        json_get_double(perf.c_str(), "hnsw_qps", &hnsw_qps) != 0 || hnsw_qps <= 0) {
        fprintf(stderr, "[fail] performance invalid\n");
        ok = 0;
    }

    std::string stats = json_extract_object(res.c_str(), "index_stats");
    long long flat_ntotal = 0, hnsw_ntotal = 0, level0 = 0;
    char mem_source[64] = {0}, storage_kind[64] = {0};
    if (stats.empty() ||
        json_get_int64(stats.c_str(), "flat_ntotal", &flat_ntotal) != 0 || flat_ntotal != count ||
        json_get_int64(stats.c_str(), "hnsw_ntotal", &hnsw_ntotal) != 0 || hnsw_ntotal != count ||
        json_get_int64(stats.c_str(), "actual_level0_neighbor_capacity", &level0) != 0 || level0 <= 0 ||
        json_get_string(stats.c_str(), "memory_bytes_source", mem_source, sizeof(mem_source)) != 0 ||
        strcmp(mem_source, "unsupported") != 0 ||
        json_get_string(stats.c_str(), "storage_metric_kind", storage_kind, sizeof(storage_kind)) != 0 ||
        strcmp(storage_kind, "storage_estimate") != 0) {
        fprintf(stderr, "[fail] index_stats invalid\n");
        ok = 0;
    }

    std::string outputs = json_extract_object(res.c_str(), "outputs");
    if (outputs.empty() ||
        validate_output_block(dataset_dir, outputs, "faiss_hnsw_per_query_jsonl", "faiss_hnsw_per_query.jsonl", qcount) != 0 ||
        validate_output_block(dataset_dir, outputs, "faiss_flat_per_query_jsonl", "faiss_flat_per_query.jsonl", qcount) != 0) {
        ok = 0;
    }

    if (ok) {
        fprintf(stdout, "[validate] PASS dataset_id=%s backend=faiss_index_only query_count=%lld topk=%lld hnsw_recall_at_10=%.4f\n",
                args.dataset_id, qcount, topk, vals[1]);
        return 0;
    }
    fprintf(stderr, "[validate] FAIL dataset_id=%s\n", args.dataset_id);
    return 1;
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
    } else {
        fprintf(stderr, "[error] unknown subcommand: %s\n", argv[1]);
        print_usage(argv[0]);
        return 2;
    }

    try {
        if (strcmp(argv[1], "generate") == 0) {
            return cmd_generate(args);
        }
        return cmd_validate(args);
    } catch (const std::exception& e) {
        fprintf(stderr, "[error] %s\n", e.what());
        return 1;
    }
}
