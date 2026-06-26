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

/* ------------------------------------------------------------------ */
/*  AVX2 dot product                                                   */
/* ------------------------------------------------------------------ */
float kvs_vector_distance_dot_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
) {
    size_t i = 0;
    float sum = 0.0f;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a = _mm256_loadu_ps(lhs + i);
        __m256 b = _mm256_loadu_ps(rhs + i);
        __m256 prod = _mm256_mul_ps(a, b);
        float tmp[8];

        _mm256_storeu_ps(tmp, prod);
        sum += tmp[0];
        sum += tmp[1];
        sum += tmp[2];
        sum += tmp[3];
        sum += tmp[4];
        sum += tmp[5];
        sum += tmp[6];
        sum += tmp[7];
    }

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
    float dist = 0.0f;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a   = _mm256_loadu_ps(lhs + i);
        __m256 b   = _mm256_loadu_ps(rhs + i);
        __m256 diff = _mm256_sub_ps(a, b);
        __m256 sq = _mm256_mul_ps(diff, diff);
        float tmp[8];

        _mm256_storeu_ps(tmp, sq);
        dist += tmp[0];
        dist += tmp[1];
        dist += tmp[2];
        dist += tmp[3];
        dist += tmp[4];
        dist += tmp[5];
        dist += tmp[6];
        dist += tmp[7];
    }

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
    float dot      = 0.0f;
    float lhs_norm = 0.0f;
    float rhs_norm = 0.0f;

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a = _mm256_loadu_ps(lhs + i);
        __m256 b = _mm256_loadu_ps(rhs + i);
        __m256 dot_v = _mm256_mul_ps(a, b);
        __m256 lhs_v = _mm256_mul_ps(a, a);
        __m256 rhs_v = _mm256_mul_ps(b, b);
        float tmp_dot[8], tmp_l2[8], tmp_r2[8];

        _mm256_storeu_ps(tmp_dot, dot_v);
        _mm256_storeu_ps(tmp_l2, lhs_v);
        _mm256_storeu_ps(tmp_r2, rhs_v);

        dot      += tmp_dot[0];
        lhs_norm += tmp_l2[0];
        rhs_norm += tmp_r2[0];
        dot      += tmp_dot[1];
        lhs_norm += tmp_l2[1];
        rhs_norm += tmp_r2[1];
        dot      += tmp_dot[2];
        lhs_norm += tmp_l2[2];
        rhs_norm += tmp_r2[2];
        dot      += tmp_dot[3];
        lhs_norm += tmp_l2[3];
        rhs_norm += tmp_r2[3];
        dot      += tmp_dot[4];
        lhs_norm += tmp_l2[4];
        rhs_norm += tmp_r2[4];
        dot      += tmp_dot[5];
        lhs_norm += tmp_l2[5];
        rhs_norm += tmp_r2[5];
        dot      += tmp_dot[6];
        lhs_norm += tmp_l2[6];
        rhs_norm += tmp_r2[6];
        dot      += tmp_dot[7];
        lhs_norm += tmp_l2[7];
        rhs_norm += tmp_r2[7];
    }

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
