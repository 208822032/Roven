#define KVS_VECTOR_INTERNAL
#include "kvs_vector.h"
#include "kvs_vector_distance.h"
#include <math.h>
#include <strings.h>

/*
 * Function pointer type for the three binary distance kernels.
 */
typedef float (*kvs_vector_distance_binary_fn)(
    const float *lhs,
    const float *rhs,
    size_t dim
);

/* Active kernels — set once by kvs_vector_distance_init(). */
static kvs_vector_distance_binary_fn g_distance_dot        = NULL;
static kvs_vector_distance_binary_fn g_distance_squared_l2 = NULL;
static kvs_vector_distance_binary_fn g_distance_cosine     = NULL;
static kvs_vector_distance_backend_t  g_distance_backend   = KVS_VECTOR_DISTANCE_BACKEND_FALLBACK;

/********** Fallback scalar C kernels **********/

static float kvs_vector_distance_dot_fallback(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    size_t i;
    float score = 0.0f;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (i = 0; i < dim; ++i) {
        score += lhs[i] * rhs[i];
    }

    return score;
}

static float kvs_vector_distance_squared_l2_fallback(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    size_t i;
    float dist = 0.0f;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (i = 0; i < dim; ++i) {
        float d = lhs[i] - rhs[i];
        dist += d * d;
    }

    return dist;
}

static float kvs_vector_distance_cosine_fallback(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    size_t i;
    float dot       = 0.0f;
    float lhs_norm  = 0.0f;
    float rhs_norm  = 0.0f;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (i = 0; i < dim; ++i) {
        dot      += lhs[i] * rhs[i];
        lhs_norm += lhs[i] * lhs[i];
        rhs_norm += rhs[i] * rhs[i];
    }

    if (lhs_norm <= 0.0f || rhs_norm <= 0.0f) {
        return 0.0f;
    }

    return dot / (sqrtf(lhs_norm) * sqrtf(rhs_norm));
}

/********** CPU feature detection **********/

static int kvs_vector_cpu_supports_avx2(void)
{
#if defined(__x86_64__) || defined(__i386__)
    /*
     * __builtin_cpu_supports is available on GCC 4.8+ and Clang 3.8+.
     * It requires a preceding __builtin_cpu_init() call.
     */
# if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") ? 1 : 0;
# endif
#endif
    return 0;
}

/********** Initialisation and backend selection **********/

int kvs_vector_distance_init(void)
{
    int want_avx2     = 0;
    int supports_avx2 = kvs_vector_cpu_supports_avx2();

    /* Default: fallback. */
    g_distance_dot        = kvs_vector_distance_dot_fallback;
    g_distance_squared_l2 = kvs_vector_distance_squared_l2_fallback;
    g_distance_cosine     = kvs_vector_distance_cosine_fallback;
    g_distance_backend    = KVS_VECTOR_DISTANCE_BACKEND_FALLBACK;

    if (strcasecmp(g_cfg.vector_distance_backend, "fallback") == 0) {
        KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE, "distance backend forced to fallback");
        return KVS_VECTOR_OK;
    }

    if (strcasecmp(g_cfg.vector_distance_backend, "avx2") == 0) {
        want_avx2 = 1;
        if (!supports_avx2) {
            KVS_LOG_WARN(KVS_VECTOR_LOG_MODULE,
                         "distance backend configured as avx2 but CPU does not support it, falling back");
        }
    } else {
        /* "auto" or unrecognised → auto-detect. */
        want_avx2 = 1;
    }

    if (want_avx2 && supports_avx2) {
        g_distance_dot        = kvs_vector_distance_dot_avx2;
        g_distance_squared_l2 = kvs_vector_distance_squared_l2_avx2;
        g_distance_cosine     = kvs_vector_distance_cosine_avx2;
        g_distance_backend    = KVS_VECTOR_DISTANCE_BACKEND_AVX2;
        KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE, "distance backend selected: avx2");
    } else {
        KVS_LOG_INFO(KVS_VECTOR_LOG_MODULE, "distance backend selected: fallback");
    }

    return KVS_VECTOR_OK;
}

const char *kvs_vector_distance_backend_name(void)
{
    switch (g_distance_backend) {
        case KVS_VECTOR_DISTANCE_BACKEND_AVX2:
            return "avx2";
        case KVS_VECTOR_DISTANCE_BACKEND_FALLBACK:
        default:
            return "fallback";
    }
}

kvs_vector_distance_backend_t kvs_vector_distance_backend_current(void)
{
    return g_distance_backend;
}

/********** Unified public entry points **********/

float kvs_vector_distance_dot(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    if (!g_distance_dot) {
        kvs_vector_distance_init();
    }
    return g_distance_dot(lhs, rhs, dim);
}

float kvs_vector_distance_squared_l2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    if (!g_distance_squared_l2) {
        kvs_vector_distance_init();
    }
    return g_distance_squared_l2(lhs, rhs, dim);
}

float kvs_vector_distance_l2_score(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    float dist = kvs_vector_distance_squared_l2(lhs, rhs, dim);
    return 1.0f / (1.0f + dist);
}

float kvs_vector_distance_cosine_score(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    if (!g_distance_cosine) {
        kvs_vector_distance_init();
    }
    return g_distance_cosine(lhs, rhs, dim);
}

float kvs_vector_distance_metric_score(
    const float *lhs,
    const float *rhs,
    size_t dim,
    kvs_vector_metric_type_t metric
) {
    switch (metric) {
        case KVS_VECTOR_METRIC_L2:
            return kvs_vector_distance_l2_score(lhs, rhs, dim);
        case KVS_VECTOR_METRIC_COSINE:
            return kvs_vector_distance_cosine_score(lhs, rhs, dim);
        case KVS_VECTOR_METRIC_IP:
        default:
            return kvs_vector_distance_dot(lhs, rhs, dim);
    }
}
