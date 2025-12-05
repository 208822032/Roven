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

#define MAX_PAYLOAD 65535
#define MAX_PAYLOAD_MASK  (MAX_PAYLOAD - 1)   // 4095

//eBPF将复制流当成一个纯字节流 只保证顺序 不保证边界

#define CHUNK_SIZE 2048
#define MAX_CHUNKS 32
#define MAX_MIRROR_BYTES (CHUNK_SIZE * MAX_CHUNKS)

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
int BPF_KPROBE(kprobe_tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size){

    if(!sk || !msg) return 0;
    if(!target_master(sk)) return 0;

    if(size == 0) return 0;

    //将一次tcp_sendmsg 视为流的一段
    //最大只复制 MAX_MIRROR_BYTES 字节 避免ringbuf中单次事件过大
    __u32 to_copy = (__u32)size;
    if(to_copy > MAX_MIRROR_BYTES){
        to_copy = MAX_MIRROR_BYTES;
    }

    //读取用户空间数据起始地址
    struct iov_iter iter = {};
    struct iovec* iov = NULL;
    void* base = NULL;

    bpf_core_read(&iter, sizeof(iter), &msg->msg_iter);
    bpf_core_read(&iov, sizeof(iov), &iter.iov);
    if(!iov) return 0;

    bpf_core_read(&base, sizeof(base), &iov->iov_base);
    if(!base) return 0;

    //准备连接信息
    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_num));
    __u16 dport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));

    //分片复制
    __u32 remaining = to_copy;
    __u32 offset = 0;
    __u32 chunk_idx = 0;

    //分片复制 最多 MAX_CHUNKS 片 每片 CHUNK_SIZE 字节
    __u32 offset32 = 0;

#pragma unroll 32
    for(int i = 0;i < MAX_CHUNKS; i++){
        if(remaining == 0)
            break;
        
        __u32 frag_len = remaining;
        if(frag_len > CHUNK_SIZE)
            frag_len = CHUNK_SIZE;
        // 再做一道收紧，帮助 verifier
        frag_len &= (CHUNK_SIZE - 1);
        if (frag_len == 0)
            break;

        //为本分片分配一个变长ringbuf事件
        struct event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
        if(!e){
            //分配失败 直接返回
            return 0;
        }

        e->pid = pid;
        e->len = frag_len;
        e->saddr = saddr;
        e->daddr = daddr;
        e->sport = sport;
        e->dport = dport;

        __u32 offset32 = offset & (MAX_MIRROR_BYTES - 1);
        __u64 addr = (__u64)base + offset32;

        //从用户空间复制数据
        //base + offset 视为一个连续缓冲区
        bpf_probe_read_user(e->data, frag_len, (void* )(addr));

        bpf_ringbuf_submit(e, 0);

        remaining -= frag_len;
        offset += frag_len;
    }

    return 0;
}
