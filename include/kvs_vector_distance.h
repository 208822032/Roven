#ifndef KVS_VECTOR_DISTANCE_H
#define KVS_VECTOR_DISTANCE_H

#include <stddef.h>

/* Forward-declared for metric type; full definition in kvs_vector.h public area. */
typedef enum kvs_vector_metric_type_e kvs_vector_metric_type_t;

/*
 * Distance computation backend.
 *
 * FALLBACK: scalar C loops, always available.
 * AVX2:     256-bit SIMD via AVX2 intrinsics, available on modern x86_64 CPUs.
 */
typedef enum kvs_vector_distance_backend_e {
    KVS_VECTOR_DISTANCE_BACKEND_FALLBACK = 0,
    KVS_VECTOR_DISTANCE_BACKEND_AVX2     = 1,
} kvs_vector_distance_backend_t;

/*
 * Initialise the distance backend.
 *
 * Reads g_cfg.vector_distance_backend:
 *   auto     → AVX2 when the CPU supports it, otherwise fallback.
 *   fallback → always scalar C.
 *   avx2     → AVX2 if supported; warn and fall back otherwise.
 *
 * Returns KVS_VECTOR_OK.
 */
int kvs_vector_distance_init(void);

/* Human-readable name of the active backend. */
const char *kvs_vector_distance_backend_name(void);

/* Active backend enum. */
kvs_vector_distance_backend_t kvs_vector_distance_backend_current(void);

/********** Unified distance entry points **********/

float kvs_vector_distance_dot(
    const float *lhs,
    const float *rhs,
    size_t dim
);

float kvs_vector_distance_squared_l2(
    const float *lhs,
    const float *rhs,
    size_t dim
);

float kvs_vector_distance_l2_score(
    const float *lhs,
    const float *rhs,
    size_t dim
);

float kvs_vector_distance_cosine_score(
    const float *lhs,
    const float *rhs,
    size_t dim
);

float kvs_vector_distance_metric_score(
    const float *lhs,
    const float *rhs,
    size_t dim,
    kvs_vector_metric_type_t metric
);

/********** AVX2 kernel declarations (called through function pointers) **********/

float kvs_vector_distance_dot_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
);

float kvs_vector_distance_squared_l2_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
);

float kvs_vector_distance_cosine_avx2(
    const float *lhs,
    const float *rhs,
    size_t dim
);

#endif /* KVS_VECTOR_DISTANCE_H */
