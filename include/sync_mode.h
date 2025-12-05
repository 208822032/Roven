#ifndef SYNC_MODE_H
#define SYNC_MODE_H

#include <stdint.h>
#include <stddef.h>
#include "kvs_config.h"

//定义同步模式
#define SYNC_MODE_TCP  0 // 通过 TCP 同步
#define SYNC_MODE_EBPF 1 // 通过 eBPF 同步

#ifndef SYNC_MODE
#define SYNC_MODE SYNC_MODE_EBPF
#endif

// 用户态共享事件结构：从 tcp_sendmsg 中抓到的一段 payload
// struct net_sync_event {
//     uint64_t ts_ns;
//     uint32_t pid;
//     uint32_t len;
//     uint16_t sport;
//     uint16_t dport;
//     char data[KVS_MAX_RESPONSE_SIZE];
// };

// #if (SYNC_MODE == SYNC_MODE_EBPF)

// eBPF 模式接口：初始化/清理
// int ebpf_sync_init(int sync_port);
// void ebpf_sync_cleanup(void);

// #endif

#endif