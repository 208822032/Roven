//内核程序 抓取发给master的数据 
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

/*
clang -O2 -g -target bpf \
  -D__TARGET_ARCH_x86 \
  -I. \
  -c ebpf_kern.c -o ebpf_kern.o
*/
//eBPF将复制流当成一个纯字节流 只保证顺序 不保证边界

#define CHUNK_SIZE 2048

struct event {
    __u32 pid;
    __u32 len;  //捕获到事件的长度
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
    __u32 chunk_idx; //当前tcp_sendmsg调用的分片索引
    __u8 data[CHUNK_SIZE]; //固定大小
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24); // 16MB
} events SEC(".maps");

//配置map 通过用户态写入 master的ip和port
struct config_t {
    __u32 master_ip;
    __u16 master_port;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct config_t);
} config_map SEC(".maps");

static __always_inline int target_master(struct sock *sk){

    __u32 key = 0;
    struct config_t* cfg = bpf_map_lookup_elem(&config_map, &key);
    if(!cfg) return 0;

    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 dport = BPF_CORE_READ(sk, __sk_common.skc_dport);

    //只抓取发给 master 的数据包
    if(daddr != cfg->master_ip) return 0;
    if(bpf_ntohs(dport) != cfg->master_port) return 0;

    return 1;
}

SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(kprobe_tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size)
{
    if (!sk || !msg) return 0;
    if (!target_master(sk)) return 0;
    if (size == 0) return 0;

    // 强约束：先只处理小包（mode6-9）
    if (size > CHUNK_SIZE) return 0;

    struct iov_iter iter = {};
    if (bpf_core_read(&iter, sizeof(iter), &msg->msg_iter) < 0)
        return 0;

    struct iovec *iov = NULL;
    if (bpf_core_read(&iov, sizeof(iov), &iter.iov) < 0 || !iov)
        return 0;

    __u64 off64 = 0;
    (void)bpf_core_read(&off64, sizeof(off64), &iter.iov_offset);

    // 再强约束：只处理 offset=0（避免 verifier 走到减法/比较的复杂路径）
    if (off64 != 0) return 0;

    struct iovec seg0 = {};
    if (bpf_core_read(&seg0, sizeof(seg0), &iov[0]) < 0)
        return 0;

    void *base = NULL;
    __u64 seg_len64 = 0;
    if (bpf_core_read(&base, sizeof(base), &seg0.iov_base) < 0)
        return 0;
    if (bpf_core_read(&seg_len64, sizeof(seg_len64), &seg0.iov_len) < 0)
        return 0;

    if (!base) return 0;
    if (seg_len64 == 0) return 0;

    // len：verifier 友好化（0..1023 + 修正 0->1024）
    __u32 len = (__u32)size;
    len &= (CHUNK_SIZE - 1);
    if (len == 0) len = CHUNK_SIZE;

    // seg0 必须覆盖 len
    if (seg_len64 < (__u64)len) return 0;

    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e) return 0;

    e->pid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    e->len = len;

    e->saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    e->daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    e->sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_num));
    e->dport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));
    e->chunk_idx = 0;

    if (bpf_probe_read_user(e->data, len, base) < 0) {
        bpf_ringbuf_discard(e, 0);
        return 0;
    }

    bpf_ringbuf_submit(e, 0);
    return 0;
}
