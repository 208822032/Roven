#include "kvs_log.h"
#include "kvs_log_writer.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    kvs_log_level_t min_level;
    int shutting_down;
    kvs_log_writer_t *writer;
} kvs_log_state_t;

static kvs_log_state_t g_log_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .min_level = KVS_LOG_LEVEL_INFO,
    .shutting_down = 0,
    .writer = NULL,
};

static const char *kvs_log_basename(const char *file) {
    const char *base = file;
    if (!file) return "unknown";
    for (const char *p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static size_t kvs_log_append(char *dst, size_t cap, size_t pos, const char *src) {
    if (!dst || !src || pos >= cap) return pos;
    while (*src && pos + 1 < cap) {
        dst[pos++] = *src++;
    }
    return pos;
}

static size_t kvs_log_append_escaped(char *dst, size_t cap, size_t pos, const char *src) {
    if (!dst || !src || pos >= cap) return pos;
    while (*src && pos + 2 < cap) {
        unsigned char ch = (unsigned char)*src++;
        switch (ch) {
            case '\\':
                if (pos + 2 >= cap) return pos;
                dst[pos++] = '\\';
                dst[pos++] = '\\';
                break;
            case '"':
                if (pos + 2 >= cap) return pos;
                dst[pos++] = '\\';
                dst[pos++] = '"';
                break;
            case '\n':
                if (pos + 2 >= cap) return pos;
                dst[pos++] = '\\';
                dst[pos++] = 'n';
                break;
            case '\r':
                if (pos + 2 >= cap) return pos;
                dst[pos++] = '\\';
                dst[pos++] = 'r';
                break;
            case '\t':
                if (pos + 2 >= cap) return pos;
                dst[pos++] = '\\';
                dst[pos++] = 't';
                break;
            default:
                dst[pos++] = (char)ch;
                break;
        }
    }
    return pos;
}

static void kvs_log_emit(kvs_log_level_t level,
                         const char *module,
                         const char *file,
                         int line,
                         int append_errno,
                         int saved_errno,
                         const char *fmt,
                         va_list ap) {
    if (!fmt) return;

    pthread_mutex_lock(&g_log_state.mutex);
    if (g_log_state.shutting_down) {
        pthread_mutex_unlock(&g_log_state.mutex);
        return;
    }

    kvs_log_level_t min_level = g_log_state.min_level;
    kvs_log_writer_t *writer = g_log_state.writer;
    pthread_mutex_unlock(&g_log_state.mutex);

    if (level > min_level) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    char body_buf[1024];
    va_list copy;
    va_copy(copy, ap);
    vsnprintf(body_buf, sizeof(body_buf), fmt, copy);
    va_end(copy);

    char line_buf[2048];
    size_t pos = 0;
    const char *module_name = (module && module[0]) ? module : "core";
    const char *file_name = kvs_log_basename(file);

    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, "ts=");
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, time_buf);

    unsigned int millis = (unsigned int)(ts.tv_nsec / 1000000L);
    char millis_buf[5];
    millis_buf[0] = '.';
    millis_buf[1] = (char)('0' + (millis / 100) % 10);
    millis_buf[2] = (char)('0' + (millis / 10) % 10);
    millis_buf[3] = (char)('0' + millis % 10);
    millis_buf[4] = '\0';
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, millis_buf);
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, " level=");
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, kvs_log_level_name(level));
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, " module=");
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, module_name);
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, " file=");
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, file_name);

    char line_no_buf[32];
    snprintf(line_no_buf, sizeof(line_no_buf), ":%d msg=\"", line);
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, line_no_buf);
    pos = kvs_log_append_escaped(line_buf, sizeof(line_buf), pos, body_buf);
    pos = kvs_log_append(line_buf, sizeof(line_buf), pos, "\"");

    if (append_errno) {
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf), " errno=%d err=\"", saved_errno);
        pos = kvs_log_append(line_buf, sizeof(line_buf), pos, err_buf);
        pos = kvs_log_append_escaped(line_buf, sizeof(line_buf), pos, strerror(saved_errno));
        pos = kvs_log_append(line_buf, sizeof(line_buf), pos, "\"");
    }

    if (pos + 2 < sizeof(line_buf)) {
        line_buf[pos++] = '\n';
        line_buf[pos] = '\0';
    } else {
        line_buf[sizeof(line_buf) - 2] = '\n';
        line_buf[sizeof(line_buf) - 1] = '\0';
    }

    if (writer) {
        (void)kvs_log_writer_emit(writer, level, line_buf, pos);
        return;
    }

    fputs(line_buf, stderr);
    fflush(stderr);
}

int kvs_log_init(const char *log_path, kvs_log_level_t min_level, int async_enabled) {
    kvs_log_writer_t *writer = NULL;
    int init_ret = kvs_log_writer_init(&writer, log_path, async_enabled);

    pthread_mutex_lock(&g_log_state.mutex);
    kvs_log_writer_t *old_writer = g_log_state.writer;
    g_log_state.writer = writer;
    g_log_state.min_level = min_level;
    g_log_state.shutting_down = 0;
    pthread_mutex_unlock(&g_log_state.mutex);

    if (old_writer) {
        kvs_log_writer_shutdown(old_writer);
    }

    return init_ret;
}

void kvs_log_shutdown(void) {
    pthread_mutex_lock(&g_log_state.mutex);
    g_log_state.shutting_down = 1;
    kvs_log_writer_t *writer = g_log_state.writer;
    g_log_state.writer = NULL;
    pthread_mutex_unlock(&g_log_state.mutex);

    if (writer) {
        kvs_log_writer_shutdown(writer);
    }

    pthread_mutex_lock(&g_log_state.mutex);
    g_log_state.shutting_down = 0;
    pthread_mutex_unlock(&g_log_state.mutex);
}

void kvs_log_set_level(kvs_log_level_t min_level) {
    pthread_mutex_lock(&g_log_state.mutex);
    g_log_state.min_level = min_level;
    pthread_mutex_unlock(&g_log_state.mutex);
}

kvs_log_level_t kvs_log_get_level(void) {
    pthread_mutex_lock(&g_log_state.mutex);
    kvs_log_level_t level = g_log_state.min_level;
    pthread_mutex_unlock(&g_log_state.mutex);
    return level;
}

int kvs_log_is_async(void) {
    pthread_mutex_lock(&g_log_state.mutex);
    kvs_log_writer_t *writer = g_log_state.writer;
    pthread_mutex_unlock(&g_log_state.mutex);
    return kvs_log_writer_is_async(writer);
}

size_t kvs_log_get_dropped_count(void) {
    pthread_mutex_lock(&g_log_state.mutex);
    kvs_log_writer_t *writer = g_log_state.writer;
    pthread_mutex_unlock(&g_log_state.mutex);
    return kvs_log_writer_dropped_count(writer);
}

const char *kvs_log_level_name(kvs_log_level_t level) {
    switch (level) {
        case KVS_LOG_LEVEL_ERROR: return "ERROR";
        case KVS_LOG_LEVEL_WARN:  return "WARN";
        case KVS_LOG_LEVEL_INFO:  return "INFO";
        case KVS_LOG_LEVEL_DEBUG: return "DEBUG";
        default: return "UNKNOWN";
    }
}

void kvs_log_write(kvs_log_level_t level,
                   const char *module,
                   const char *file,
                   int line,
                   const char *fmt,
                   ...) {
    va_list ap;
    va_start(ap, fmt);
    kvs_log_emit(level, module, file, line, 0, 0, fmt, ap);
    va_end(ap);
}

void kvs_log_write_errno(kvs_log_level_t level,
                         const char *module,
                         const char *file,
                         int line,
                         const char *fmt,
                         ...) {
    int saved_errno = errno;
    va_list ap;
    va_start(ap, fmt);
    kvs_log_emit(level, module, file, line, 1, saved_errno, fmt, ap);
    va_end(ap);
}