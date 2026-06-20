# VSEARCH Query Result Cache Design

## 1. Proposed Cache Key

Recommended key:

```text
normalized_query_hash + topK + scope + model_id + index_generation
```

Where:

- `normalized_query_hash`: normalized query text hash
- `topK`: result cardinality affects serialization
- `scope`: different scope filters must not collide
- `model_id`: embedding model changes invalidate semantics
- `index_generation`: vector index mutation epoch

## 2. Proposed Value

Cached value should be final search result materialization:

```text
record ids + scores + answer payload snapshot
```

Returning only IDs is unsafe unless the lookup step revalidates every record.

## 3. Why This Is Not P0

The current measurements show embedding dominates cold-query latency. Exact embedding cache already pushes hot `VSEARCH_TEXT` close to vector-only search cost. Query result cache adds higher consistency risk for smaller incremental gain.

## 4. Consistency Risks

### 4.1 VSET After Cache Fill

A new write can create a better match, but old cached results may miss it.

### 4.2 VDEL After Cache Fill

Deleted records may still be returned unless every cached result is revalidated.

### 4.3 TTL Expiry

Expired answers may remain visible until cache eviction.

### 4.4 Scope Drift

If scope definition changes or scope parsing differs, stale results may cross boundaries.

## 5. Required Guardrails

Any future implementation should require:

1. `index_generation` or `vector_store_epoch` in key
2. short TTL
3. best-effort result revalidation before response build
4. bounded memory with LRU eviction

## 6. Recommended TTL

If implemented later:

- default TTL should be short, such as `1000ms` to `5000ms`
- longer TTL is unsafe without stronger invalidation

## 7. Invalidation Strategy

Preferred strategy:

- bump `vector_store_epoch` on any vector-visible write or delete
- bind cache entries to the epoch captured at insert time

This is simpler than reverse-index invalidation by query term.

## 8. Recommended Config Surface

Not for immediate implementation, but the likely shape is:

```text
vsearch_result_cache_enabled = no
vsearch_result_cache_capacity = 10000
vsearch_result_cache_ttl_ms = 2000
```

## 9. Recommendation

Do not implement production query result cache in the next step.

Priority remains:

1. exact embedding cache rollout
2. ONNX thread tuning
3. micro-batch embedding

Query result cache should only be reconsidered after:

- hot embedding cache is stable,
- cold-query throughput is still insufficient,
- and `index_generation` invalidation is available.
