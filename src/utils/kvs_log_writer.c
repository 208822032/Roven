#include "kvs_log_writer.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KVS_LOG_QUEUE_CAPACITY 256
#define KVS_LOG_LINE_CAP 2048
#define KVS_LOG_FLUSH_BATCH 64

typedef struct {
    kvs_log_level_t level;
    size_t len;
    char line[KVS_LOG_LINE_CAP];
} kvs_log_record_t;

struct kvs_log_writer {
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    pthread_mutex_t stream_mutex;
    pthread_t thread;
    int thread_started;
    int async_requested;
    int async_active;
    int stopping;

    FILE *stream;
    int owns_stream;

    kvs_log_record_t *records;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    size_t dropped_low_priority;
    size_t direct_fallback_count;
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

static void kvs_log_writer_close_stream(struct kvs_log_writer *writer) {
    if (!writer || !writer->stream) return;
    fflush(writer->stream);
    if (writer->owns_stream) {
        fclose(writer->stream);
    }
    writer->stream = NULL;
    writer->owns_stream = 0;
}

static int kvs_log_writer_write_locked(struct kvs_log_writer *writer,
                                       const char *line,
                                       size_t line_len) {
    if (!writer || !writer->stream || !line || line_len == 0) {
        return 0;
    }

    if (fwrite(line, 1, line_len, writer->stream) != line_len) {
        return -1;
    }
    return fflush(writer->stream);
}

static void kvs_log_writer_emit_summary(struct kvs_log_writer *writer,
                                        const char *file,
                                        int line) {
    if (!writer || writer->dropped_low_priority == 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    char summary[512];
    int written = snprintf(summary,
                           sizeof(summary),
                           "ts=%s.%03ld level=WARN module=log file=%s:%d msg=\"dropped low-priority log events\" dropped=%zu direct_fallback=%zu\n",
                           time_buf,
                           (long)(ts.tv_nsec / 1000000),
                           kvs_log_basename(file),
                           line,
                           writer->dropped_low_priority,
                           writer->direct_fallback_count);
    if (written <= 0) return;
    size_t summary_len = (size_t)written;
    if (summary_len >= sizeof(summary)) {
        summary_len = sizeof(summary) - 1;
    }

    pthread_mutex_lock(&writer->stream_mutex);
    kvs_log_writer_write_locked(writer, summary, summary_len);
    pthread_mutex_unlock(&writer->stream_mutex);
}

static void *kvs_log_writer_thread_main(void *arg) {
    struct kvs_log_writer *writer = (struct kvs_log_writer *)arg;
    kvs_log_record_t batch[KVS_LOG_FLUSH_BATCH];

    for (;;) {
        size_t batch_count = 0;

        pthread_mutex_lock(&writer->queue_mutex);
        while (writer->count == 0 && !writer->stopping) {
            pthread_cond_wait(&writer->queue_cond, &writer->queue_mutex);
        }

        if (writer->count == 0 && writer->stopping) {
            pthread_mutex_unlock(&writer->queue_mutex);
            break;
        }

        while (writer->count > 0 && batch_count < KVS_LOG_FLUSH_BATCH) {
            batch[batch_count++] = writer->records[writer->head];
            writer->head = (writer->head + 1) % writer->capacity;
            writer->count--;
        }
        pthread_mutex_unlock(&writer->queue_mutex);

        pthread_mutex_lock(&writer->stream_mutex);
        for (size_t i = 0; i < batch_count; ++i) {
            (void)batch[i].level;
            (void)kvs_log_writer_write_locked(writer, batch[i].line, batch[i].len);
        }
        pthread_mutex_unlock(&writer->stream_mutex);
    }

    return NULL;
}

static FILE *kvs_log_writer_open_stream(const char *log_path, int *owns_stream, int *open_failed) {
    if (owns_stream) *owns_stream = 0;
    if (open_failed) *open_failed = 0;

    if (!log_path || log_path[0] == '\0') {
        return stderr;
    }

    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        if (open_failed) *open_failed = 1;
        return stderr;
    }

    if (owns_stream) *owns_stream = 1;
    return fp;
}

int kvs_log_writer_init(kvs_log_writer_t **writer_out, const char *log_path, int async_enabled) {
    if (!writer_out) return -1;
    *writer_out = NULL;

    kvs_log_writer_t *writer = (kvs_log_writer_t *)calloc(1, sizeof(*writer));
    if (!writer) return -1;

    if (pthread_mutex_init(&writer->queue_mutex, NULL) != 0) {
        free(writer);
        return -1;
    }
    if (pthread_cond_init(&writer->queue_cond, NULL) != 0) {
        pthread_mutex_destroy(&writer->queue_mutex);
        free(writer);
        return -1;
    }
    if (pthread_mutex_init(&writer->stream_mutex, NULL) != 0) {
        pthread_cond_destroy(&writer->queue_cond);
        pthread_mutex_destroy(&writer->queue_mutex);
        free(writer);
        return -1;
    }

    int open_failed = 0;
    writer->stream = kvs_log_writer_open_stream(log_path, &writer->owns_stream, &open_failed);
    writer->async_requested = async_enabled ? 1 : 0;
    writer->async_active = 0;

    if (writer->async_requested) {
        writer->capacity = KVS_LOG_QUEUE_CAPACITY;
        writer->records = (kvs_log_record_t *)calloc(writer->capacity, sizeof(*writer->records));
        if (writer->records) {
            if (pthread_create(&writer->thread, NULL, kvs_log_writer_thread_main, writer) == 0) {
                writer->thread_started = 1;
                writer->async_active = 1;
            } else {
                fprintf(stderr, "[log] failed to start async writer, fallback to sync\n");
                free(writer->records);
                writer->records = NULL;
                writer->capacity = 0;
            }
        } else {
            fprintf(stderr, "[log] failed to allocate async queue, fallback to sync\n");
        }
    }

    *writer_out = writer;
    return open_failed ? -1 : 0;
}

static int kvs_log_writer_enqueue(struct kvs_log_writer *writer,
                                  kvs_log_level_t level,
                                  const char *line,
                                  size_t line_len) {
    if (!writer || !line || line_len == 0 || !writer->async_active || !writer->records) {
        return -1;
    }

    if (line_len > KVS_LOG_LINE_CAP) {
        line_len = KVS_LOG_LINE_CAP;
    }

    pthread_mutex_lock(&writer->queue_mutex);
    if (writer->count == writer->capacity) {
        if (level >= KVS_LOG_LEVEL_INFO) {
            writer->dropped_low_priority++;
            pthread_mutex_unlock(&writer->queue_mutex);
            return 0;
        }
        writer->direct_fallback_count++;
        pthread_mutex_unlock(&writer->queue_mutex);
        pthread_mutex_lock(&writer->stream_mutex);
        int ret = kvs_log_writer_write_locked(writer, line, line_len);
        pthread_mutex_unlock(&writer->stream_mutex);
        return ret;
    }

    kvs_log_record_t *record = &writer->records[writer->tail];
    record->level = level;
    record->len = line_len;
    memcpy(record->line, line, line_len);

    writer->tail = (writer->tail + 1) % writer->capacity;
    writer->count++;
    pthread_cond_signal(&writer->queue_cond);
    pthread_mutex_unlock(&writer->queue_mutex);
    return 0;
}

int kvs_log_writer_emit(kvs_log_writer_t *writer,
                        kvs_log_level_t level,
                        const char *line,
                        size_t line_len) {
    if (!writer || !line || line_len == 0) return 0;

    if (!writer->async_active) {
        pthread_mutex_lock(&writer->stream_mutex);
        int ret = kvs_log_writer_write_locked(writer, line, line_len);
        pthread_mutex_unlock(&writer->stream_mutex);
        return ret;
    }

    return kvs_log_writer_enqueue(writer, level, line, line_len);
}

int kvs_log_writer_is_async(const kvs_log_writer_t *writer) {
    return writer && writer->async_active;
}

size_t kvs_log_writer_dropped_count(const kvs_log_writer_t *writer) {
    return writer ? writer->dropped_low_priority : 0;
}

void kvs_log_writer_shutdown(kvs_log_writer_t *writer) {
    if (!writer) return;

    pthread_mutex_lock(&writer->queue_mutex);
    writer->stopping = 1;
    pthread_cond_signal(&writer->queue_cond);
    pthread_mutex_unlock(&writer->queue_mutex);

    if (writer->thread_started) {
        pthread_join(writer->thread, NULL);
    }

    kvs_log_writer_emit_summary(writer, __FILE__, __LINE__);

    pthread_mutex_lock(&writer->stream_mutex);
    kvs_log_writer_close_stream(writer);
    pthread_mutex_unlock(&writer->stream_mutex);

    pthread_mutex_destroy(&writer->stream_mutex);
    pthread_cond_destroy(&writer->queue_cond);
    pthread_mutex_destroy(&writer->queue_mutex);
    free(writer->records);
    free(writer);
}