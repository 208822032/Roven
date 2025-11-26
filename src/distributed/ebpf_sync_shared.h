#ifndef EBPF_SYNC_SHARED_H
#define EBPF_SYNC_SHARED_H

/*
 * 统一 eBPF 同步事件结构体。
 *
 * 规则：
 *   - BPF 端：Makefile 用 clang -DEBPF_KVS_MAX_RESPONSE_SIZE=2048 注入宏；
 *   - 用户态：在自己的 .c 文件里包含 kvs_config.h，再
 *       #define EBPF_KVS_MAX_RESPONSE_SIZE KVS_MAX_RESPONSE_SIZE
 *     或者在编译参数里注入；
 *   - 本头文件不再 include kvs_config.h。
 */

#ifndef EBPF_KVS_MAX_RESPONSE_SIZE
# error "EBPF_KVS_MAX_RESPONSE_SIZE must be defined before including ebpf_sync_shared.h"
#endif

// struct net_sync_event {
//     __u64 ts_ns;
//     __u32 pid;
//     __u32 len;
//     __u16 sport;
//     __u16 dport;
//     char  data[EBPF_KVS_MAX_RESPONSE_SIZE];
// };

//使用柔性数组
struct net_sync_event {
    __u64 ts_ns;
    __u32 pid;
    __u32 len;
    __u16 sport;
    __u16 dport;
    char  data[];
};

#endif // EBPF_SYNC_SHARED_H
