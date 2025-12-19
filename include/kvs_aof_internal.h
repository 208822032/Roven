#pragma once
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>

#include "kvs_config.h"
#include "kvs_aof.h"

// 内部可见的完整 AOF 状态结构（对外隐藏字段）
typedef struct aof_state_s {
    FILE *fp;                  // AOF文件指针
    char *filename;            // AOF文件名
    char *temp_filename;       // 临时文件名(用于重写)
    char *buf;                 // AOF缓冲区
    size_t buf_len;            // 缓冲区当前长度
    size_t buf_size;           // 缓冲区总大小
    int fsync_mode;            // 同步模式
    off_t current_size;        // 当前AOF文件大小
    off_t rewrite_base_size;   // 重写基准大小
    time_t last_fsync;         // 上次同步时间
    pthread_mutex_t mutex;     // 缓冲区互斥锁
    pthread_cond_t cond;       // 条件变量(用于后台线程)
    pthread_t bgthread;        // 后台线程ID
    volatile int stop;         // 停止标志

    int loading; //正在AOF load/replay
    int pause_rewrite; //暂停rewrite触发

    // 重写相关字段
    int rewrite_in_progress;   // 重写进行中标志
    pid_t rewrite_child_pid;   // 重写子进程ID
    char *rewrite_buf;         // 重写缓冲区
    size_t rewrite_buf_len;    // 重写缓冲区当前长度
    size_t rewrite_buf_size;   // 重写缓冲区总大小
    pthread_mutex_t rewrite_mutex; // 重写缓冲区互斥锁
    int aof_fd;                // AOF文件描述符用于同步

    // COW相关字段
    int cow_enabled;                  // 是否启用COW
    pthread_rwlock_t data_rwlock;     // 数据读写锁
    volatile int cow_snapshot_ready;  // COW快照准备就绪标志
    time_t snapshot_timestamp;        // 快照时间戳

    // 初始化标志，防止重复释放
    int initialized;
} aof_state_t;

// 内部模块共享的全局状态（实现文件负责定义）
extern aof_state_t aof_state;
