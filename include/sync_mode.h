#ifndef SYNC_MODE_H
#define SYNC_MODE_H

#include <stdint.h>
#include <stddef.h>

//定义同步模式
#define SYNC_MODE_TCP  0 // 通过 TCP 同步
#define SYNC_MODE_EBPF 1 // 通过 eBPF 同步

#ifndef SYNC_MODE
#define SYNC_MODE SYNC_MODE_EBPF
#endif

//用户态共享事件结构
struct ebpf_sync_event{
    uint64_t ts_ns;
    uint32_t pid;
    uint32_t argc;
    char cmd[16];
    char key[128];
    char value[256];
};

#if (SYNC_MODE == SYNC_MODE_EBPF)

//ebpf模式接口
int ebpf_sync_init(int sync_port);
void ebpf_sync_cleanup(void);
int ebpf_emit_sync_event(char** tokens, int count);


//守护进程接口
void* ebpf_sync_daemon(void* arg);

#endif

#endif