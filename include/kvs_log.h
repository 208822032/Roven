#pragma once

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVS_LOG_LEVEL_ERROR = 0,
    KVS_LOG_LEVEL_WARN  = 1,
    KVS_LOG_LEVEL_INFO  = 2,
    KVS_LOG_LEVEL_DEBUG = 3,
} kvs_log_level_t;

int kvs_log_init(const char *log_path, kvs_log_level_t min_level, int async_enabled);
void kvs_log_shutdown(void);
void kvs_log_set_level(kvs_log_level_t min_level);
kvs_log_level_t kvs_log_get_level(void);
int kvs_log_is_async(void);
size_t kvs_log_get_dropped_count(void);
const char *kvs_log_level_name(kvs_log_level_t level);
void kvs_log_write(kvs_log_level_t level,
                   const char *module,
                   const char *file,
                   int line,
                   const char *fmt,
                   ...);
void kvs_log_write_errno(kvs_log_level_t level,
                         const char *module,
                         const char *file,
                         int line,
                         const char *fmt,
                         ...);

#define KVS_LOG_ERROR(module, fmt, ...) \
    kvs_log_write(KVS_LOG_LEVEL_ERROR, module, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define KVS_LOG_WARN(module, fmt, ...) \
    kvs_log_write(KVS_LOG_LEVEL_WARN, module, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define KVS_LOG_INFO(module, fmt, ...) \
    kvs_log_write(KVS_LOG_LEVEL_INFO, module, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define KVS_LOG_DEBUG(module, fmt, ...) \
    kvs_log_write(KVS_LOG_LEVEL_DEBUG, module, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define KVS_LOG_ERROR_ERRNO(module, fmt, ...) \
    kvs_log_write_errno(KVS_LOG_LEVEL_ERROR, module, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif