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

clang -O2 -g -target bpf 
    -D__TARGET_ARCH_x86 
    -I. 
    -fno-unroll-loops 
    -c ebpf_kern.c -o ebpf_kern.o
*/

//eBPF将复制流当成一个纯字节流 只保证顺序 不保证边界

#define CHUNK_SIZE 4096

#define MAX_CHUNKS 32
#define MAX_MIRROR_BYTES (CHUNK_SIZE * MAX_CHUNKS) // 最大镜像字节数

#define MAX_IOV_SEGS  32    //iov 最大段数上限

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

// struct stats_t {
//     __u64 hook_calls;     // 命中过 target_master 的 tcp_sendmsg 调用次数
//     __u64 hook_bytes;     // 这些调用请求发送的总字节数(size累加)
//     __u64 mirror_events;  // 实际 reserve 成功并 submit 的 ringbuf event 数
//     __u64 mirror_bytes;   // 实际镜像出去的字节数(按 frag_len 累加)
//     __u64 ringbuf_drops;  // ringbuf_reserve 失败次数(至少粒度是“分片”)
// };

// struct {
//     __uint(type, BPF_MAP_TYPE_ARRAY);
//     __uint(max_entries, 1);
//     __type(key, __u32);
//     __type(value, struct stats_t);
// } stats_map SEC(".maps");

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

//读取 iov_iter 的第 idx 段 iovec
static __always_inline int read_iov_seg(
    const struct iovec* iov,
    __u32 idx,
    void** base,
    __u64* len
){
    struct iovec seg = {};
    //按idx读取 iov[idx]
    if(bpf_core_read(&seg, sizeof(seg), &iov[idx]) < 0) return -1;

    // seg.iov_base / seg.iov_len 本身在内核是 “用户指针 + 长度”
    // 这里读取出来用于 bpf_probe_read_user
    if(bpf_core_read(base, sizeof(*base), &seg.iov_base) < 0) return -1;
    if(bpf_core_read(len, sizeof(*len), &seg.iov_len) < 0) return -1;

    if(!*base || *len == 0) return -1;

    return 0;
}

//把 iov_iter 当成「多段 buffer + offset 的游标」来读
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(kprobe_tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size){

    if(!sk || !msg) return 0;
    if(!target_master(sk)) return 0;

    if(size == 0) return 0;

    // __u32 idx = 0;
    // struct stats_t *st = bpf_map_lookup_elem(&stats_map, &idx);
    // if (!st)
    //     return 0;

    // 1) 内核真正处理的“被钩住的 tcp_sendmsg 调用”
    // st->hook_calls++;
    // st->hook_bytes += size;

    //将一次tcp_sendmsg 视为流的一段
    //最大只复制 MAX_MIRROR_BYTES 字节 避免ringbuf中单次事件过大
    // if(size > CHUNK_SIZE){
    //     // st->ringbuf_drops++;
    //     return 0;
    // }
    // 本次最多镜像 MAX_MIRROR_BYTES，避免用户态/环形缓冲压力过大
    __u32 to_copy = (__u32)size;
    if(to_copy > MAX_MIRROR_BYTES){
        to_copy = MAX_MIRROR_BYTES;
    }

    //读取用户空间数据起始地址
    struct iov_iter iter = {};
    if(bpf_core_read(&iter, sizeof(iter), &msg->msg_iter) < 0) return 0;
    struct iovec* iov = NULL;
    if(bpf_core_read(&iov, sizeof(iov), &iter.iov) < 0 || !iov) return 0;

    //获取 iov_iter 的 nr_segs 和 off
    __u64 nr_segs64 = 0, off64 = 0;
    bpf_core_read(&nr_segs64, sizeof(nr_segs64), &iter.nr_segs);
    bpf_core_read(&off64, sizeof(off64), &iter.iov_offset);

    __u32 nr_segs = (__u32)nr_segs64;
    if(nr_segs == 0) return 0;
    if(nr_segs > MAX_IOV_SEGS)
        nr_segs = MAX_IOV_SEGS; //验证器安全上限

    //准备连接信息（必须在下面用到）
    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_num));
    __u16 dport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));

    //定位起始阶段
    __u32 seg = 0;
    __u64 seg_off = off64;
    void* base = NULL;
    __u64 seg_len = 0;

    // 依次跳过前面的段，直到找到包含 offset 的段
#pragma clang loop unroll(disable)
    for(int i = 0;i < MAX_IOV_SEGS;i++){
        if(seg >= nr_segs) break;
        
        if(read_iov_seg(iov, seg, &base, &seg_len) < 0)
            return 0;

        if(seg_off < seg_len) break;   
    
        seg_off -= seg_len;
        seg++;
    }

    if(seg >= nr_segs || seg_off >= seg_len) return 0;

    //分片拷贝 每个chunk 最多 CHUNK_SIZE
    __u32 remaining = to_copy;
    __u32 chunk_idx = 0;

#pragma clang loop unroll(disable)
    for(int i = 0;i < MAX_CHUNKS;i++){
        if(remaining == 0 || seg >= nr_segs) break;

        __u64 avail = seg_len - seg_off;
        if(avail == 0){
            //当前段用完 读下一段
            seg++;
            seg_off = 0;
            if(seg >= nr_segs) break;
            if(read_iov_seg(iov, seg, &base, &seg_len) < 0) break;
            avail = seg_len;
        }

        __u32 frag_len = remaining;
        if(frag_len > CHUNK_SIZE)
            frag_len = CHUNK_SIZE;
        if((__u64)frag_len > avail)
            frag_len = (__u32)avail;

        frag_len &= (CHUNK_SIZE - 1);
        if (frag_len == 0)
            frag_len = CHUNK_SIZE;

        struct event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
        if(!e) break;

        e->pid = pid;
        e->saddr = saddr;
        e->daddr = daddr;
        e->sport = sport;
        e->dport = dport;
        e->chunk_idx = chunk_idx++;
        e->len = frag_len;

        if (bpf_probe_read_user(e->data, frag_len,
            (void *)((__u64)base + seg_off)) < 0) {
            bpf_ringbuf_discard(e, 0);
            break;
        }

        bpf_ringbuf_submit(e, 0);

        remaining -= frag_len;
        seg_off += frag_len;
    }


    // if(!iov) return 0;

    // if(bpf_core_read(&base, sizeof(base), &iov->iov_base) < 0) return 0;

    // if (!base) return 0;



    // //准备连接信息
    // __u32 pid = bpf_get_current_pid_tgid() >> 32;
    // __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    // __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    // __u16 sport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_num));
    // __u16 dport = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_dport));

    // //分片复制
    // __u32 remaining = to_copy;
    // __u32 offset = 0;
    // __u32 chunk_idx = 0;

    // __u32 offset32 = 0;


// #pragma unroll 32
//     for(int i = 0;i < MAX_CHUNKS; i++){
//         if(remaining == 0)
//             break;
        
//         __u32 frag_len = remaining;
//         if(frag_len > CHUNK_SIZE)
//             frag_len = CHUNK_SIZE;
//         // 再做一道收紧，帮助 verifier
//         frag_len &= (CHUNK_SIZE - 1);
//         if (frag_len == 0)
//             frag_len = CHUNK_SIZE;

//         //为本分片分配一个变长ringbuf事件
//         struct event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
//         if(!e){
//             // 2) ringbuf reserve 失败：记录一次 drop（按“分片”计）
//             // st->ringbuf_drops++;
//             return 0;
//         }

//         // 3) 实际镜像成功的 event/bytes
//         // st->mirror_events++;
//         // st->mirror_bytes += frag_len;

//         e->pid = pid;
//         e->saddr = saddr;
//         e->daddr = daddr;
//         e->sport = sport;
//         e->dport = dport;
//         e->chunk_idx = chunk_idx;
//         e->len = frag_len;

//         // __u32 offset32 = offset & (MAX_MIRROR_BYTES - 1);
//         // __u64 addr = (__u64)base + offset32;
//         __u64 addr = (__u64)base + offset;

//         //从用户空间复制数据
//         //base + offset 视为一个连续缓冲区
//         long ret = bpf_probe_read_user(e->data, frag_len, (void *)addr);
//         if(ret < 0){
//             bpf_ringbuf_discard(e, 0);
//             return 0;
//         }

//         bpf_ringbuf_submit(e, 0);

//         remaining -= frag_len;
//         offset += frag_len;
//         // chunk_idx++;
//     }

    return 0;
}
