#pragma once
#include <stdio.h>
#include <time.h>
#include <stdint.h>

// 同步模式
#define AOF_FSYNC_NO       0
#define AOF_FSYNC_EVERYSEC 1
#define AOF_FSYNC_ALWAYS   2

// AOF 缓冲区与重写相关常量（可在实现中调整）
#define AOF_BUFFER_SIZE         (1024 * 1024 * 64)
#define AOF_REWRITE_MIN_SIZE    (1024 * 1024 * 128)
#define AOF_REWRITE_PERCENTAGE  50
#define AOF_REWRITE_MIN_GROWTH  (1024 * 1024 * 64)

// 现有实现的对外 API（保持兼容）。后续可演进为不透明句柄。
typedef struct aof_state_s aof_state_t; // 前置声明以隐藏内部字段

int aof_init(const char *filename, int fsync_mode);
void aof_append(int argc, char **argv);
void aof_fsync(int force);
void aof_rewrite(void);
int aof_load(const char *filename);
void aof_free(void);
void aof_background_fsync(void *arg);
void aof_rewrite_background(void);
int aof_rewrite_tmpfile(void);
int aof_rewrite_buffer_write(int fd);
void aof_rewrite_append_command(int argc, char **argv);
void aof_stop_rewrite(void);
int should_buffer_rewrite_command(void);
//只读接口
int aof_is_loading(void);

// 过渡期可保留：外部可能引用全局状态（建议后续移除）
extern aof_state_t aof_state;

