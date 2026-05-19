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
    __m256 acc = _mm256_setzero_ps();

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a = _mm256_loadu_ps(lhs + i);
        __m256 b = _mm256_loadu_ps(rhs + i);
        acc = _mm256_add_ps(acc, _mm256_mul_ps(a, b));
    }

    /* Horizontal sum of 8 floats */
    {
        float tmp[8];
        float sum;
        _mm256_storeu_ps(tmp, acc);
        sum = tmp[0] + tmp[1] + tmp[2] + tmp[3] +
              tmp[4] + tmp[5] + tmp[6] + tmp[7];
        /* Tail */
        for (; i < dim; ++i) {
            sum += lhs[i] * rhs[i];
        }
        return sum;
    }
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
    __m256 acc = _mm256_setzero_ps();

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a   = _mm256_loadu_ps(lhs + i);
        __m256 b   = _mm256_loadu_ps(rhs + i);
        __m256 diff = _mm256_sub_ps(a, b);
        acc = _mm256_add_ps(acc, _mm256_mul_ps(diff, diff));
    }

    {
        float tmp[8];
        float dist;
        _mm256_storeu_ps(tmp, acc);
        dist = tmp[0] + tmp[1] + tmp[2] + tmp[3] +
               tmp[4] + tmp[5] + tmp[6] + tmp[7];
        for (; i < dim; ++i) {
            float d = lhs[i] - rhs[i];
            dist += d * d;
        }
        return dist;
    }
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
    __m256 acc_dot  = _mm256_setzero_ps();
    __m256 acc_lhs2 = _mm256_setzero_ps();
    __m256 acc_rhs2 = _mm256_setzero_ps();

    if (!lhs || !rhs || dim == 0) {
        return 0.0f;
    }

    for (; i + 8 <= dim; i += 8) {
        __m256 a = _mm256_loadu_ps(lhs + i);
        __m256 b = _mm256_loadu_ps(rhs + i);

        acc_dot  = _mm256_add_ps(acc_dot,  _mm256_mul_ps(a, b));
        acc_lhs2 = _mm256_add_ps(acc_lhs2, _mm256_mul_ps(a, a));
        acc_rhs2 = _mm256_add_ps(acc_rhs2, _mm256_mul_ps(b, b));
    }

    {
        float tmp_dot[8], tmp_l2[8], tmp_r2[8];
        float dot      = 0.0f;
        float lhs_norm = 0.0f;
        float rhs_norm = 0.0f;

        _mm256_storeu_ps(tmp_dot,  acc_dot);
        _mm256_storeu_ps(tmp_l2,   acc_lhs2);
        _mm256_storeu_ps(tmp_r2,   acc_rhs2);

        dot      = tmp_dot[0]  + tmp_dot[1]  + tmp_dot[2]  + tmp_dot[3] +
                   tmp_dot[4]  + tmp_dot[5]  + tmp_dot[6]  + tmp_dot[7];
        lhs_norm = tmp_l2[0]   + tmp_l2[1]   + tmp_l2[2]   + tmp_l2[3] +
                   tmp_l2[4]   + tmp_l2[5]   + tmp_l2[6]   + tmp_l2[7];
        rhs_norm = tmp_r2[0]   + tmp_r2[1]   + tmp_r2[2]   + tmp_r2[3] +
                   tmp_r2[4]   + tmp_r2[5]   + tmp_r2[6]   + tmp_r2[7];

        /* Tail */
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
