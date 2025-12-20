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

#define CHUNK_SIZE 4096
#define MAX_CHUNKS 64
#define MAX_TOTAL (MAX_CHUNKS * CHUNK_SIZE) //单次最多抓 64KB

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

// kprobe 入口缓存：避免 tcp_recvmsg 返回时 iov_iter 被推进导致拿不到原始 base
struct recv_args_t {
    struct sock* sk;
    void* base;    // iov[0].iov_base (user pointer)
    __u64 seg_len; // iov[0].iov_len
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct config_t);
} config_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16384);
    __type(key, __u64);              // pid_tgid
    __type(value, struct recv_args_t);
} recv_args_map SEC(".maps");

static __always_inline int target_master(struct sock *sk){

    __u32 key = 0;
    struct config_t* cfg = bpf_map_lookup_elem(&config_map, &key);
    if(!cfg) return 0;

    // tcp_recvmsg：本地地址/端口
    __u32 laddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    __u16 lport = BPF_CORE_READ(sk, __sk_common.skc_num); // host order

    //只抓取发给 master 的数据包
    if(cfg->master_ip != 0 && laddr != cfg->master_ip) return 0;
    if(lport != cfg->master_port) return 0;
    return 1;
}

SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(kprobe_tcp_recvmsg, struct sock *sk, struct msghdr *msg, size_t len)
{
    if (!sk || !msg) return 0;
    if (!target_master(sk)) return 0;
    if (len == 0) return 0;

    // 强约束：先只处理小包（mode6-9）
    // if (len > CHUNK_SIZE) return 0;

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

    if (!base || seg_len64 == 0) return 0;

    //缓存给 kretprobe 使用
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    struct recv_args_t args = {};
    args.sk = sk;
    args.base = base;
    args.seg_len = seg_len64;

    bpf_map_update_elem(&recv_args_map, &pid_tgid, &args, BPF_ANY);
    return 0;
}

SEC("kretprobe/tcp_recvmsg")
int BPF_KRETPROBE(kretprobe_tcp_recvmsg, int ret){

    __u64 pid_tgid = bpf_get_current_pid_tgid();
    struct recv_args_t* args = bpf_map_lookup_elem(&recv_args_map, &pid_tgid);
    if(!args) return 0;

    struct sock* sk = args->sk;
    void* base = args->base;
    __u64 seg_len64 = args->seg_len;

    //用完删除
    bpf_map_delete_elem(&recv_args_map, &pid_tgid);

    if (!sk || !base || seg_len64 == 0) return 0;

    if (!target_master(sk)) return 0;

    if(ret <= 0) return 0; //没有收到数据

    //超过最大限制 则丢弃
    if(ret > (int)MAX_TOTAL) return 0;

    if(seg_len64 < (__u64)ret) return 0; //实际拷贝长度不足

    __u32 total = (__u32)ret;

    //小包路径
    if(total <= (__u32)CHUNK_SIZE){
        __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
        __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
        __u16 sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));
        __u16 dport = BPF_CORE_READ(sk, __sk_common.skc_num); // host order

        struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
        if (!e) return 0;

        e->pid = (__u32)(pid_tgid >> 32);
        e->len = total;
        e->saddr = saddr;
        e->daddr = daddr;
        e->sport = sport;
        e->dport = dport;
        e->chunk_idx = 0;

        __u32 size = total;
        //强制 size 到 1..CHUNK_SIZE
        size &= (CHUNK_SIZE - 1);
        if (size == 0)
            size = CHUNK_SIZE;

        // 直接从 base 读取 total
        if(bpf_probe_read_user(e->data, size, base) < 0){
            bpf_ringbuf_discard(e, 0);
            return 0;
        }
        // bpf_printk("ebpf path=SMALL total=%u\n", total);
        bpf_ringbuf_submit(e, 0);
        return 0;
    }

#pragma unroll
    for (int i = 0; i < MAX_CHUNKS; i++) {

        __u32 off = (__u32)i * ( __u32)CHUNK_SIZE;
        if (off >= total)
            break;

        // __u32 remaining = total - off;

        __u32 chunk_len = total - off;
        if (chunk_len > CHUNK_SIZE)
            chunk_len = CHUNK_SIZE;

        // verifier 友好：把 chunk_len 强制到 1..CHUNK_SIZE（CHUNK_SIZE 必须是 2^n）
        chunk_len &= (CHUNK_SIZE - 1);
        if (chunk_len == 0)
            chunk_len = CHUNK_SIZE;

        struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
        if (!e)
            return 0;

        e->pid = (__u32)(pid_tgid >> 32);
        e->len = chunk_len;

        e->saddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
        e->daddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
        e->sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));
        e->dport = BPF_CORE_READ(sk, __sk_common.skc_num); // host order
        e->chunk_idx = (__u32)i;

        // 从 base+off 读取 chunk_len
        const void *src = (const void *)((const char *)base + off);
        if (bpf_probe_read_user(e->data, chunk_len, src) < 0) {
            bpf_ringbuf_discard(e, 0);
            return 0;
        }
        // if(i == 0)
        //     bpf_printk("ebpf path=LARGE total=%u\n", total);
        bpf_ringbuf_submit(e, 0);
    }

    return 0;
}
