# core_hnsw_v2 final architecture

## Backend positioning

Current vector backend roles:

| backend | role |
|---|---|
| `core_hnsw_v2` | default ANN backend |
| `core_flat` / `flat` | exact baseline, correctness oracle, fallback |
| `core_hnsw` | removed legacy backend; config string / enum remain only for compatibility and resolve to `core_flat` fallback |
| `hnsw` / `hnswlib_hnsw` | legacy hnswlib compatibility adapter when compiled |

`core_hnsw_v2` is now selected by default when `vector_index_type` is unset. `flat` remains explicitly selectable as `core_flat`. `core_hnsw` is accepted only for old configuration compatibility and resolves to `core_flat`; the legacy source directory has been deleted by CODE-HYGIENE-0.

## Manager / vector_core boundary

The manager owns user-visible vector command semantics:

- `VSET`, `VSEARCH`, `VDEL`, `VINDEX`, `VINFO`
- record store and key ownership
- scope, TTL, metadata, question, answer
- active/deleted filtering
- threshold filtering
- response assembly
- AOF/RDB/replication
- rebuild orchestration

`core_hnsw_v2` owns only index-local state:

- `record_id`
- vector bytes
- score
- record_id-to-slot map local to the index
- slot-to-record_id map
- deleted flags
- graph/search state
- memory/stat info

The v2 core does not access `g_vector_state`, manager `record_id_map`, scope, TTL, metadata, question, answer, or response formatting.

## Source of truth

The record store remains the source of truth. HNSW graphs are derived runtime state.

Persistence boundary:

- RDB/AOF save records.
- RDB/AOF do not save v2 HNSW graph topology.
- Load/replay reconstructs the active v2 graph from active records.
- A graph can be reset and rebuilt without changing record ownership or persistence format.

## Command paths

### VSET

```text
VSET
  -> manager stores/updates record
  -> manager passes record_id + vector to active backend
  -> core_hnsw_v2 adds vector into graph
```

Same-record update uses tombstone re-add semantics: the old slot is marked deleted and the updated vector is added as a new active slot for the same record id.

### VDEL

```text
VDEL
  -> manager marks record deleted
  -> core_hnsw_v2 mark_delete(record_id)
  -> v2 keeps tombstone graph slot for runtime safety
```

Deletes are tombstones. They are filtered by core search and still verified by manager resolve/filtering.

### VSEARCH

```text
VSEARCH
  -> core_hnsw_v2 search returns record_id + score
  -> manager resolves records
  -> manager applies active/deleted/scope/threshold filtering
  -> manager assembles unchanged VSEARCH response
```

The response does not expose internal slot ids or record ids beyond the existing user-facing payload semantics.

### VINDEX REBUILD

```text
VINDEX REBUILD
  -> manager scans active records
  -> reset derived graph
  -> reserve active capacity
  -> re-add active record_id + vector pairs
  -> deleted tombstones are removed from the rebuilt graph
```

After rebuild, v2 `deleted_count` should return to `0`.

### VINFO

`VINFO` exposes backend status and measured v2 memory/stat fields. `core_hnsw_v2_experimental_enabled` remains a compatibility/telemetry field and no longer gates v2 selection.

## Tombstone policy

Recommended operational policy:

```text
deleted_count / max(size, 1) >= 0.30
  -> recommend VINDEX REBUILD

deleted_count / max(size, 1) >= 0.50
  -> strong recommend rebuild / warn search degradation risk

after VINDEX REBUILD
  -> deleted_count should reset to 0
```

This is a policy recommendation, not an automatic rebuild trigger. No user command was added.

## Rebuild strategy

Current product path uses synchronous rebuild. This is correct and simple, but not final for large datasets.

Bulk import recommendation:

1. Import records first.
2. Avoid frequent rebuilds during the import loop.
3. Run one `VINDEX REBUILD` after import.
4. If background rebuild is added later, use the existing shadow rebuild/replay/swap design as the starting point.

Cold start risk:

- RDB/AOF restore must rebuild v2 graph from records.
- Large datasets make synchronous cold-start rebuild slow.
- Current 100K smoke build took about 1019s on this machine, so this is not yet a 100K+ production rebuild story.

## Scale/stability snapshot

Stage 9G3 index-only gate, dim=512:

| count | mode | build_us | memory_bytes | ef=128 recall | ef=256 recall | ef=512 recall | duplicate | overflow |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 10K | full, 50 queries | 61,308,701 | 64,104,384 | 0.8840 | 0.9860 | 1.0000 | 0 | 0 |
| 50K | sampled, 20 queries | 464,624,378 | 319,997,248 | 0.4600 | 0.6650 | 0.8600 | 0 | 0 |
| 100K | smoke, 5 queries | 1,034,482,127 | 639,994,400 | 0.3800 | 0.5400 | 0.7200 | 0 | 0 |

10K is the current validated default gate. 50K/100K data should be read as risk evidence, not as production SLA.

## Known limits

- Synchronous rebuild is slow at 50K/100K.
- Cold start rebuild is synchronous.
- High tombstone ratio requires rebuild policy discipline.
- 50K/100K sampled recall at ef<=512 is not production-ready.
- No background rebuild worker is enabled.
- No 1M production-scale validation has been completed.
- v2 graph is not persisted.

## Directions not recommended now

Do not continue feature expansion under Stage 9G:

- no reverse_prune cache/layout work
- no SIMD rewrite
- no background thread
- no resurrection of deleted legacy `core_hnsw`
- no 1M sprint
- no VSEARCH response change
- no RDB/AOF graph persistence

Future work should be bug fixes, documentation, interview review, and small controlled rechecks. If 50K/100K production readiness becomes a requirement, start with a new profiling/audit stage rather than directly refactoring or optimizing.
