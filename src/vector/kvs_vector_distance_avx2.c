#include <stddef.h>
#include <math.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "kvs_vector_distance.h"

/*
 * When this file is compiled with -mavx2, __AVX2__ is predefined.
 * On non-x86 architectures we compile an empty stub that returns 0.0f.
 */

#if defined(__AVX2__) && (defined(__x86_64__) || defined(__i386__))

static float kvs_vector_distance_hsum256_ps(__m256 v)
{
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 sum = _mm_add_ps(lo, hi);

    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return _mm_cvtss_f32(sum);
}

/* ------------------------------------------------------------------ */
/*  AVX2 dot product                                                   */
/* ------------------------------------------------------------------ */
float kvs_vector_distance_dot_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    size_t i = 0;
    __m256 sum_v = _mm256_setzero_ps();
    float sum;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a = _mm256_loadu_ps(lhs + i);
        __m256 b = _mm256_loadu_ps(rhs + i);
        __m256 prod = _mm256_mul_ps(a, b);

        sum_v = _mm256_add_ps(sum_v, prod);
    }

    sum = kvs_vector_distance_hsum256_ps(sum_v);
    for (; i < dim; ++i) {
        sum += lhs[i] * rhs[i];
    }

    return sum;
}

/* ------------------------------------------------------------------ */
/*  AVX2 squared L2 distance                                           */
/* ------------------------------------------------------------------ */
float kvs_vector_distance_squared_l2_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    size_t i = 0;
    __m256 dist_v = _mm256_setzero_ps();
    float dist;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a   = _mm256_loadu_ps(lhs + i);
        __m256 b   = _mm256_loadu_ps(rhs + i);
        __m256 diff = _mm256_sub_ps(a, b);
        __m256 sq = _mm256_mul_ps(diff, diff);

        dist_v = _mm256_add_ps(dist_v, sq);
    }

    dist = kvs_vector_distance_hsum256_ps(dist_v);
    for (; i < dim; ++i) {
        float d = lhs[i] - rhs[i];
        dist += d * d;
    }

    return dist;
}

/* ------------------------------------------------------------------ */
/*  AVX2 cosine score (dot + both norms in one pass)                   */
/* ------------------------------------------------------------------ */
float kvs_vector_distance_cosine_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    size_t i = 0;
    __m256 dot_v = _mm256_setzero_ps();
    __m256 lhs_norm_v = _mm256_setzero_ps();
    __m256 rhs_norm_v = _mm256_setzero_ps();
    float dot;
    float lhs_norm;
    float rhs_norm;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a = _mm256_loadu_ps(lhs + i);
        __m256 b = _mm256_loadu_ps(rhs + i);
        __m256 prod = _mm256_mul_ps(a, b);
        __m256 lhs_sq = _mm256_mul_ps(a, a);
        __m256 rhs_sq = _mm256_mul_ps(b, b);

        dot_v = _mm256_add_ps(dot_v, prod);
        lhs_norm_v = _mm256_add_ps(lhs_norm_v, lhs_sq);
        rhs_norm_v = _mm256_add_ps(rhs_norm_v, rhs_sq);
    }

    dot = kvs_vector_distance_hsum256_ps(dot_v);
    lhs_norm = kvs_vector_distance_hsum256_ps(lhs_norm_v);
    rhs_norm = kvs_vector_distance_hsum256_ps(rhs_norm_v);
    for (; i < dim; ++i) {
        dot      += lhs[i] * rhs[i];
        lhs_norm += lhs[i] * lhs[i];
        rhs_norm += rhs[i] * rhs[i];
    }

    if (lhs_norm <= 0.0f || rhs_norm <= 0.0f) {
        return 0.0f;
    }

    return dot / (sqrtf(lhs_norm) * sqrtf(rhs_norm));
}

#else /* !__AVX2__ or non-x86 */

/* ------------------------------------------------------------------ */
/*  Stub implementations — forward to nothing (never called, as        */
/*  the runtime selector will always choose fallback on non-x86).      */
/* ------------------------------------------------------------------ */

float kvs_vector_distance_dot_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    (void)lhs; (void)rhs; (void)dim;
    return 0.0f;
}

float kvs_vector_distance_squared_l2_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    (void)lhs; (void)rhs; (void)dim;
    return 0.0f;
}

float kvs_vector_distance_cosine_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    (void)lhs; (void)rhs; (void)dim;
    return 0.0f;
}

#endif /* __AVX2__ && x86 */
