#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "ebpf_sync_shared.h"

// // 与用户态 sync_mode.h 中的 struct net_sync_event 保持字段一致
// struct net_sync_event {
//     __u64 ts_ns;
//     __u32 pid;
//     __u32 len;
//     __u16 sport;
//     __u16 dport;
//     char  data[KVS_MAX_RESPONSE_SIZE];
// };

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24); // 16MB
} sync_events SEC(".maps");

// 可选：配置同步端口（主节点 sync_port），用户态写入
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u16);
} sync_conf SEC(".maps");

// kprobe: tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(handle_tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size)
{
    struct net_sync_event *e;
    __u16 dport = 0;
    __u16 sport = 0;
    __u16 cfg_port = 0;
    __u32 key = 0;

    // 先打印一条，确认 kprobe 触发
    // bpf_printk("handle_tcp_sendmsg enter, size=%llu\n", (unsigned long long)size);

    // 从 sock 读目的端口（网络字节序）
    bpf_core_read(&dport, sizeof(dport), &sk->__sk_common.skc_dport);
    bpf_core_read(&sport, sizeof(sport), &sk->__sk_common.skc_num);

    // 将 dport 转为主机字节序下的端口号
    dport = __builtin_bswap16(dport);

    // 读取配置的同步端口
    __u16 *pport = bpf_map_lookup_elem(&sync_conf, &key);
    if (!pport)
        return 0;
    cfg_port = *pport;

    // 只关注发送到 sync_port 的流量
    if (dport != cfg_port)
        return 0;

    // bpf_printk("tcp_sendmsg dport=%u cfg_port=%u\n", dport, cfg_port);

    // 根据数据长度解析 iovecs
    struct iovec *iov = NULL;
    size_t iovlen = 0;

    bpf_core_read(&iov, sizeof(iov), &msg->msg_iter.iov);
    bpf_core_read(&iovlen, sizeof(iovlen), &msg->msg_iter.count);
    if (!iov || iovlen == 0)
        return 0;

    //从 iov 中取出iov_base 和 单段长度iov_len
    void* base = NULL;
    size_t seg_len = 0;
    bpf_core_read(&base, sizeof(base), &iov->iov_base);
    bpf_core_read(&seg_len, sizeof(seg_len), &iov->iov_len);
    if (!base || seg_len == 0)
        return 0;

    e = bpf_ringbuf_reserve(&sync_events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->ts_ns = bpf_ktime_get_ns();
    e->pid   = bpf_get_current_pid_tgid() >> 32;
    e->sport = sport;
    e->dport = dport;

    //真实发送的字节数 = min(size, seg_len, sizeof(e->data))
    __u64 sz = size;
    __u64 s1 = seg_len;
    __u32 max_copy = sizeof(e->data);
    __u64 min1 = sz < s1 ? sz : s1;
    __u64 min2 = min1 < max_copy ? min1 : max_copy;
    __u32 copy_len = (__u32)min2;
    e->len = copy_len;

    if(copy_len > 0){
        bpf_core_read_user(e->data, copy_len, base);
    }

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";//GPL 许可证