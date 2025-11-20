#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

// struct sync_event{
//     uint64_t ts_ns;
//     uint32_t pid;
//     uint32_t argc;
//     char cmd[16];
//     char key[128];
//     char value[256];
// };

struct ebpf_sync_event {
    __u64 ts_ns;
    __u32 pid;
    __u32 argc;
    char cmd[16];
    char key[128];
    char value[256];
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24); // 16MB
} sync_events SEC(".maps");

// struct {
//     __uint(type, BPF_MAP_TYPE_HASH);
//     __uint(max_entries, 10240);
//     __type(key, __u32);
//     __type(value, __u64);
// } event_filter SEC(".maps");

#if 0
// uprobe 挂载到 ebpf_emit_sync_event_internal (用户态埋点)
SEC("uprobe/ebpf_emit_sync_event_internal")
int BPF_KPROBE(trace_sync_event, char** tokens, int count){
    
    if(count <= 0 || count > 10) return 0;

    //预留 ring buffer 空间
    struct sync_event* e = bpf_ringbuf_reserve(&sync_events, sizeof(*e), 0);
    if(!e){
        return 0;
    }

    e->ts_ns = bpf_ktime_get_ns();//获取时间戳
    e->pid = bpf_get_current_pid_tgid() >> 32;//获取进程ID
    e->argc = count;

    //读取用户态指针
    char* cmd_ptr = NULL;
    bpf_probe_read_user(&cmd_ptr, sizeof(cmd_ptr), &tokens[0]);
    if(cmd_ptr){
        bpf_probe_read_user_str(&e->cmd, sizeof(e->cmd), cmd_ptr);
    }

    if(count > 1){
        char* key_ptr = NULL;
        bpf_probe_read_user(&key_ptr, sizeof(key_ptr), &tokens[1]);
        if(key_ptr){
            bpf_probe_read_user_str(&e->key, sizeof(e->key), key_ptr);
        }
    }

    if(count > 2){
        char* value_ptr = NULL;
        bpf_probe_read_user(&value_ptr, sizeof(value_ptr), &tokens[2]);
        if(value_ptr){
            bpf_probe_read_user_str(&e->value, sizeof(e->value), value_ptr);
        }
    }

    //提交事件到 ring buffer
    bpf_ringbuf_submit(e, 0);
    return 0;
}
#else
// x86_64 函数参数顺序：rdi,rsi,rdx,rcx,r8,r9
// 第1参数 tokens(char**)，第2参数 count(int)
// SEC("uprobe")
SEC("uprobe//proc/self/exe:ebpf_emit_sync_event_internal")
int trace_sync_event(struct pt_regs *ctx)
{
    struct ebpf_sync_event *e;
    char **tokens;
    int count;

    tokens = (char **)ctx->di;   // PT_REGS_PARM1 等价
    count  = (int)ctx->si;       // PT_REGS_PARM2

    if (count <= 0 || !tokens)
        return 0;

    e = bpf_ringbuf_reserve(&sync_events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->ts_ns = bpf_ktime_get_ns();
    e->pid   = bpf_get_current_pid_tgid() >> 32;
    e->argc  = count;

    char *p = NULL;
    // cmd
    bpf_probe_read_user(&p, sizeof(p), &tokens[0]);
    if (p) bpf_probe_read_user_str(e->cmd, sizeof(e->cmd), p);

    // key
    if (count > 1) {
        p = NULL;
        bpf_probe_read_user(&p, sizeof(p), &tokens[1]);
        if (p) bpf_probe_read_user_str(e->key, sizeof(e->key), p);
    }

    // value
    if (count > 2) {
        p = NULL;
        bpf_probe_read_user(&p, sizeof(p), &tokens[2]);
        if (p) bpf_probe_read_user_str(e->value, sizeof(e->value), p);
    }

    // 调试输出（可选，查看 /sys/kernel/debug/tracing/trace）
    // bpf_printk("ebpf_sync evt cmd=%s key=%s\n", e->cmd, e->key);

    bpf_ringbuf_submit(e, 0);

    
    return 0;
}
#endif

char LICENSE[] SEC("license") = "GPL";//GPL 许可证