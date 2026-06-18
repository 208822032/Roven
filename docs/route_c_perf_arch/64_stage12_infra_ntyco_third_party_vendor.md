# Stage 12 Infra: NtyCo third_party vendoring and build path cleanup

## Goal

Migrate the NtyCo coroutine library from a git submodule at the repository root into `third_party/NtyCo` as vendored, locally-controlled third-party source code. This ensures that the Stage 12 `pthread_once` race fix in `nty_socket.c` is permanently preserved and cannot be overwritten by a future `git submodule update`.

## Why

During Stage 12 Final TSAN gate closure, a data race was discovered in NtyCo's `init_hook` function: the global `socket_f` (and other function pointers) were written by the main thread and read by background threads without synchronization. The fix replaced the unsafe `if (!socket_f) init_hook()` pattern with a `pthread_once` guard.

Since this fix cannot be pushed upstream (NtyCo is an external project at `https://github.com/wangbojing/NtyCo.git`), the modified source must be vendored into the KVLite repository to prevent regression.

## NtyCo Original State

| Property | Value |
|----------|-------|
| Type | git submodule |
| URL | `https://github.com/wangbojing/NtyCo.git` |
| Commit | `72ab5fd04f0c228f464f160aaa521bb791b34aa5` (v1.0.4-55-g72ab5fd) |
| Location | `NtyCo/` (repository root) |
| Modified by Stage 12 | Yes — `NtyCo/core/nty_socket.c` (pthread_once fix) |

## Migration Steps Executed

1. **Backup**: Copied NtyCo source (`core/`, `Makefile`, `README.md`) to `/tmp/ntyco_vendor_backup/`.
2. **Deinit submodule**: `git submodule deinit -f NtyCo` — removed from `.git/config` and cleared working tree.
3. **Remove from index**: `git rm -f NtyCo` — removed from git index and `.gitmodules`.
4. **Clean internal state**: `rm -rf .git/modules/NtyCo` — removed internal submodule repository.
5. **Restore as vendored source**: Copied backup to `third_party/NtyCo/`.
6. **Update build system**: See below.
7. **Update .gitignore**: Added rules for `third_party/NtyCo/objs/` and `third_party/NtyCo/libntyco.a`.
8. **Verify**: Build + test suite pass.

## Directory Structure After Migration

```
third_party/
├── NtyCo/
│   ├── core/
│   │   ├── CMakeLists.txt
│   │   ├── Makefile
│   │   ├── nty_coroutine.c
│   │   ├── nty_coroutine.h
│   │   ├── nty_epoll.c
│   │   ├── nty_queue.h
│   │   ├── nty_schedule.c
│   │   ├── nty_socket.c      ← Stage 12 pthread_once fix HERE
│   │   └── nty_tree.h
│   ├── Makefile
│   └── README.md
├── libbpf/
└── onnxruntime/
```

Root `NtyCo/` directory: **removed**.

## Build Path Changes

| Item | Before | After |
|------|--------|-------|
| `NTYCO_DIR` | `NtyCo` | `third_party/NtyCo` |
| Include path | `-I ./NtyCo/core/` | `-I ./third_party/NtyCo/core/` |
| Library path | `-L ./NtyCo/` | `-L ./third_party/NtyCo/` |
| Library artifact | `NtyCo/libntyco.a` | `third_party/NtyCo/libntyco.a` |
| Object artifacts | `NtyCo/objs/*.o` | `third_party/NtyCo/objs/*.o` |
| Init check | `[ ! -f NtyCo/Makefile ]` → `git submodule update` | `[ ! -d third_party/NtyCo/core ]` → error with instructions |

## Source Reference Changes

No production source code changes were needed. The only NtyCo references in source are:

- `src/network/ntyco.c` — includes NtyCo headers via `#include "nty_coroutine.h"` (found through `-I` flag)
- `src/network/ntyco_defult.c` — same

Both continue to work because the include path is updated in CFLAGS.

## Test Script Updates

| File | Change |
|------|--------|
| `tests/rwlock_weakening_gate_stage11.sh` | Added `third_party/NtyCo/libntyco.a` to artifact check list (kept old `NtyCo/libntyco.a` for compatibility) |

## .gitignore Rules

```
/third_party/NtyCo/objs/       # Build object files
/third_party/NtyCo/libntyco.a  # Build library artifact
```

The existing `*.o` and `*.a` patterns also apply globally. The explicit entries ensure these paths are never accidentally tracked.

## Stage 12 pthread_once Race Fix Verification

The fix in `third_party/NtyCo/core/nty_socket.c`:

```c
static pthread_once_t g_hook_once = PTHREAD_ONCE_INIT;

static void init_hook_inner(void) {
    socket_f = (socket_t)dlsym(RTLD_NEXT, "socket");
    // ...all other dlsym calls...
}

int init_hook(void) {
    pthread_once(&g_hook_once, init_hook_inner);
    return 0;
}
```

All `if (!_f) init_hook()` checks were replaced with unconditional `init_hook()` calls (pthread_once ensures single execution).

**Confirmed present** in `third_party/NtyCo/core/nty_socket.c` via test script.

## Verification Results

| Test | Result |
|------|--------|
| `make embedding ENABLE_EBPF=0` | PASS |
| `ntyco_third_party_vendor_stage12_infra.sh` | PASS |
| `ntyco_executor_stage10_13.sh` | PASS |
| `ntyco_mixed_shutdown_stage12.sh` | PASS |
| `rwlock_weakening_gate_stage11.sh` | PASS |
| `shutdown_flag_atomic_stage12.sh` | PASS |
| `weak_rwlock_mixed_stress_stage12.sh` | PASS (real-lock + weak-lock, reactor) |
| `weak_rwlock_correctness_stage12.sh` | PASS (real-lock + weak-lock, all sub-tests) |

## Impact on STAGE13_READY

**No change.** STAGE13_READY remains valid. This is a build system/infra cleanup that does not modify Route C executor semantics, `g_kv_data_rwlock`, weak-lock logic, or any data path.

## Rollback

1. Restore `.gitmodules` with NtyCo submodule entry pointing to `https://github.com/wangbojing/NtyCo.git`
2. `git submodule update --init NtyCo`
3. Revert Makefile paths to `NtyCo/`
4. Revert `.gitignore` changes
5. Note: upstream NtyCo does NOT have the pthread_once fix; TSAN would regress

## Subsequent Notes

- **Do not** run `git submodule update --init` expecting it to populate NtyCo — there are no submodules in this repository after migration.
- **Do not** pull NtyCo from upstream — the vendored copy in `third_party/NtyCo` is the authoritative source.
- If updating NtyCo from upstream in the future, the pthread_once fix must be re-applied.
