# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test Commands

```bash
# First time setup
git submodule init && git submodule update
make

# Build target binary
make kvstore

# Build all (binary + tests + eBPF)
make all

# Clean
make clean

# Run a single test binary (most tests take host/port args)
./build/tests/bin/testcase 127.0.0.1 9300 5            # basic pipeline test
./build/tests/bin/write_qps_test 127.0.0.1 9300 5 100000  # write benchmark
./build/tests/bin/vector_testcase 127.0.0.1 9300       # vector CRUD test
./build/tests/bin/vector_import_bench 127.0.0.1 9300   # vector import benchmark

# Start the server
./kvstore --config config/master.conf     # master
./kvstore --config config/slave.conf      # slave (if replication needed)

# Benchmark suite (KVStore vs Redis)
KV_HOST=127.0.0.1 KV_PORT=9300 REDIS_HOST=127.0.0.1 REDIS_PORT=6379 \
  C=50 N=100000 scripts/bench_all.sh

# TTL regression tests
bash tests/ttl/test_ttl_all_backends.sh 127.0.0.1 9300
```

## Architecture

KVLite is a lightweight in-memory KV store written in C with multi-data-structure support, TTL, persistence, master-slave replication (TCP + eBPF), and a built-in vector semantic search module for AI Chat session caching.

### Request Flow

```
Client (RESP) → Network Layer (reactor/proactor/NtyCo) → RESP Parser
  → Command Dispatch (kvs_operation.c command_table)
  → Storage Engine (array | rbtree | hash | skiptable)
  → AOF append (if enabled) + master_sync_command (if master)
  → RESP reply to client
```

### Key Modules

| Module | Path | Role |
|--------|------|------|
| Core | `src/core/` | Entry point (`kvstore.c`), command dispatch (`kvs_operation.c`), config, TTL background scanner, globals |
| Storage | `src/storage/` | Four data structures: `kvs_array`, `kvs_rbtree`, `kvs_hash`, `kvs_skiptable`; memory pool (`kvs_mempool`) with pool/system/jemalloc backends |
| Network | `src/network/` | Three backends: reactor (epoll), proactor (io_uring), NtyCo (coroutines); RESP protocol parser |
| Persistence | `src/persistence/` | AOF append/rewrite/replay, RDB save/load. Vector persistence: RDB saves vector records and embeddings in a dedicated vector section; AOF append/replay uses existing VSET/VDEL commands; AOF rewrite emits active vector records as VSET commands; HNSW graph structure is not persisted, indexes are rebuilt after loading records |
| Distributed | `src/distributed/` | TCP master-slave: full sync + incremental replication |
| eBPF | `src/eBPF/` | Kernel probe (`ebpf_kern.c`) + userspace daemon (`ebpf_syncd.c`) for capturing master traffic via kprobe/tcp_sendmsg and forwarding to slaves |
| Vector | `src/vector/` | Vector store with flat + HNSW + SQ8 Flat ANN index, built-in hash embedding |
| Utils | `src/utils/` | Lock-free ring message queue, netbuf, tx_batcher, master-slave binary protocol codec, logging |

### Headers

- `include/kvs_api.h` — single include that pulls in all public headers
- `include/kvs_config.h` — all compile-time flags (`ENABLE_ARRAY`, `ENABLE_AOF`, etc.), config struct, and enum types
- `include/kvs_types.h` — command enum, data structure enum, TTL helpers (`kv_now_ms`, `kv_is_expired`)
- `include/kvs_command.h` — `command_context_t`, `command_info` struct, handler function declarations
- `include/kvs_vector.h` — public vector API + (under `KVS_VECTOR_INTERNAL` guard) all internal vector types and functions
### Vector Module Architecture

```
V-family commands (VSET/VGET/VSEARCH/VDEL/VINFO/VINDEX REBUILD)
  → kvs_operation.c handlers
  → src/vector/kvs_vector.c (public API)
  → src/vector/kvs_vector_embedding.c (built-in hash embedding)
  → src/vector/kvs_vector_index.c (flat + HNSW + SQ8 ANN index + topK heap)
```

- **Embedding provider**: `hash` (local deterministic hash embedding, 64-dim). Config key: `embedding_provider`.
- **Index abstraction**: vector search uses a FAISS-like `kvs_vector_index_t` + `kvs_vector_index_ops_t` dispatch layer. Flat, HNSW, and SQ8 Flat share the same add/search/rebuild/info interface.
- **Index types**: `flat` (exact), `hnsw` (default, ANN), `sq8_flat` (experimental compressed). Config key: `vector_index_type`.
- **SQ8 Flat**: SQ8 scalar-quantized flat index prototype. Keeps record float embeddings unchanged, stores only int8 codes + per-vector absmax scale in the index. Rebuilds codes from records after restart.
- **Metric types**: `ip`, `l2`, and `cosine`. Config key: `vector_metric`. Default is `ip` to preserve the previous dot-product behavior.
- **Distance backend**: vector distance calculation uses a unified backend with `auto`, `fallback`, and `avx2` modes. Config key: `vector_distance_backend` (default `auto`). The `auto` mode selects AVX2 when available and falls back to scalar C otherwise. Only `src/vector/kvs_vector_distance_avx2.c` is compiled with `-mavx2`; all other source files use the standard CFLAGS. The active backend is reported in VINFO via `distance_backend` field.
- **Scoped HNSW search**: when scope is non-empty, HNSW expands candidate collection by `vector_scope_overfetch_factor` before post-filtering by scope. Default is `4`; valid range is `1~32`; `1` disables overfetch.
KVLite is positioned as a lightweight AI Chat session cache with built-in semantic retrieval.

### Config System

Priority: defaults (`kvs_config_init_default`) → config file → CLI args (applied twice so CLI always wins). Config files are simple `key = value` format. See `config/kvstore.conf` for the default single-node config and `config/master.conf` / `config/slave.conf` for replication setups.

### TTL Semantics

Each key stores `expire_at_ms` (absolute UTC ms). Lazy deletion on read paths (GET/EXIST/RANGE/SORT/FULLDUMP), plus a lightweight background scanner thread. Overwrite operations (SET/HSET/etc.) clear existing TTL. `EXPIRE`/`TTL`/`PERSIST` commands are supported across all four data structures.