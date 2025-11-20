//用户态eBPF加载与守护进程
#include "sync_mode.h"

#if (SYNC_MODE == SYNC_MODE_EBPF)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "include/distributed.h"
#include "include/resp_protocol.h"

//全局BPF对象
static struct bpf_object* bpf_obj = NULL;
static struct bpf_link* uprobe_link = NULL;
static pthread_t daemon_thread;
static volatile int daemon_running = 0;
static struct ring_buffer* rb = NULL;

extern distributed_config_t global_dist_config;

// ✅ 添加 libbpf 调试回调
static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
    return vfprintf(stderr, format, args);
}

//Ringbuf 回调处理
static int handle_sync_event(void* ctx, void* data, size_t size){
    
    if(size != sizeof(struct ebpf_sync_event)) return 0;

    struct ebpf_sync_event* evt = (struct ebpf_sync_event *)data;

    // ✅ 添加调试输出
    // fprintf(stderr, "[EBPF_SYNC] Event: pid=%u cmd='%s' key='%s' val='%s'\n",
            // evt->pid, evt->cmd, evt->key, evt->value);

    //构造RESP命令
    char resp_buffer[2048];
    int len = 0;

    if(evt->value[0] != '\0'){
        // 三参数命令
        len = snprintf(resp_buffer, sizeof(resp_buffer),
            "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(evt->cmd), evt->cmd,
            strlen(evt->key), evt->key,
            strlen(evt->value), evt->value);
    }else{
        // 两参数命令
        len = snprintf(resp_buffer, sizeof(resp_buffer),
            "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(evt->cmd), evt->cmd,
            strlen(evt->key), evt->key);
    }

    //分发到所有节点
    if(global_dist_config.role != ROLE_MASTER) return 0;

    pthread_mutex_lock(&global_dist_config.slave_mutex);
    for(int i = 0; i < global_dist_config.slave_count; i++){
        slave_info_t* slave = &global_dist_config.slaves[i];
        if(send(slave->sockfd, resp_buffer, len, MSG_NOSIGNAL) < 0){
            fprintf(stderr, "[EBPF_SYNC] Failed to send to slave %d: %s\n", 
                    i, strerror(errno));
        }
    }
    pthread_mutex_unlock(&global_dist_config.slave_mutex);

    return 0;
}

//守护线程轮询 ringbuf
void* ebpf_sync_daemon(void* arg){

    printf("[EBPF_SYNC] Daemon started\n");

    while(daemon_running){//轮询
        int err = ring_buffer__poll(rb, 1);// 优化到1ms延迟
        if(err < 0 && err != -EINTR){
            fprintf(stderr, "[EBPF_SYNC] ring_buffer__poll error: %d\n", err);
            break;
        }

    }

    printf("[EBPF_SYNC] Daemon stopped\n");
    return NULL;

}

static int attach_uprobe(struct bpf_program *prog, const char *exe_path) {
    // 偏移方式（需要 -no-pie 或使用 nm 获取地址）
    // nm kvstore | grep ebpf_emit_sync_event_internal
    off_t func_off = 0x14441; // 替换成你的实际地址
    uprobe_link = bpf_program__attach_uprobe(prog, false, -1, exe_path, func_off);
    if (libbpf_get_error(uprobe_link)) {
        fprintf(stderr,"[EBPF_SYNC] offset attach failed err=%ld\n",
                libbpf_get_error(uprobe_link));
        return -1;
    }
    printf("[EBPF_SYNC] attached by offset 0x%lx\n",(long)func_off);
    return 0;
}

//初始化 eBPF
int ebpf_sync_init(int sync_port){
    (void)sync_port;

    // ✅ 启用 libbpf 调试
    // libbpf_set_print(libbpf_print_fn);

    //加载BPF对象文件
    bpf_obj = bpf_object__open_file("ebpf_sync.bpf.o", NULL);
    if(libbpf_get_error(bpf_obj)){
        fprintf(stderr, "[EBPF_SYNC] Failed to open BPF object\n");
        return -1;
    }

    if(bpf_object__load(bpf_obj)){
        fprintf(stderr, "[EBPF_SYNC] Failed to load BPF object\n");
        bpf_object__close(bpf_obj);
        return -1;
    }

    //附加 uprobe 到 ebpf_emit_sync_event_internal 函数
    struct bpf_program* prog = bpf_object__find_program_by_name(bpf_obj, "trace_sync_event");

    if(!prog){
        fprintf(stderr, "[EBPF_SYNC] Failed to find BPF program\n");
        bpf_object__close(bpf_obj);
        return -1;
    }

    //获取当前进程路径
    char exe_path[256];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if(len == -1){
        perror("readlink");
        bpf_object__close(bpf_obj);
        return -1;
    }
    exe_path[len] = '\0';

    // //获取符号偏移
    // //nm kvstore | grep ebpf_emit_sync_event_internal
    // off_t func_off = 0x1438c;


    // uprobe_link = bpf_program__attach_uprobe(prog, 
    //                                         false, // retprobe
    //                                         -1,    // pid (-1表示所有进程，但实际会被binary_path限制) 
    //                                         exe_path,  // 二进制路径
    //                                         func_off);        // 函数偏移(需要通过nm获取符号地址) 

    // if(libbpf_get_error(uprobe_link)){
    //     fprintf(stderr, "[EBPF_SYNC] Failed to attach uprobe\n");
    //     bpf_object__close(bpf_obj);
    //     return -1;
    // }

    if(attach_uprobe(prog, exe_path) < 0){
        bpf_object__close(bpf_obj);
        bpf_obj = NULL;
        return -1;
    }

    //创建ringbuf
    int map_fd = bpf_object__find_map_fd_by_name(bpf_obj, "sync_events");
    if (map_fd < 0) {
        fprintf(stderr, "[EBPF_SYNC] Failed to find ringbuf map\n");
        bpf_link__destroy(uprobe_link);
        bpf_object__close(bpf_obj);
        return -1;
    }

    rb = ring_buffer__new(map_fd, handle_sync_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "[EBPF_SYNC] Failed to create ringbuf\n");
        bpf_link__destroy(uprobe_link);
        bpf_object__close(bpf_obj);
        return -1;
    }

    //启动守护线程
    daemon_running = 1;
    if(pthread_create(&daemon_thread, NULL, ebpf_sync_daemon, NULL) != 0){
        fprintf(stderr, "[EBPF_SYNC] Failed to create daemon thread\n");
        ring_buffer__free(rb);
        bpf_link__destroy(uprobe_link);
        bpf_object__close(bpf_obj);
        return -1;
    }

    printf("[EBPF_SYNC] eBPF sync initialized successfully\n");
    return 0;

}

void ebpf_sync_cleanup(void)
{
    daemon_running = 0;
    pthread_join(daemon_thread, NULL);
    ring_buffer__free(rb);
    bpf_link__destroy(uprobe_link);
    bpf_object__close(bpf_obj);
    printf("[EBPF_SYNC] Cleaned up\n");
}

// 用户态埋点函数 (会被 uprobe 监控)
__attribute__((noinline))
void ebpf_emit_sync_event_internal(char **tokens, int count)
{
    // 空函数，仅作为 uprobe 挂载点
    // 实际参数传递由 eBPF 从栈/寄存器读取
    (void)tokens;
    (void)count;
}

// 对外接口
int ebpf_emit_sync_event(char **tokens, int count)
{
    if (count <= 0 || !tokens) return -1;
    ebpf_emit_sync_event_internal(tokens, count);
    return 0;
}

#endif