#pragma once

#include <stddef.h>

#include "kvs_log.h"

typedef struct kvs_log_writer kvs_log_writer_t;

int kvs_log_writer_init(kvs_log_writer_t **writer_out, const char *log_path, int async_enabled);
void kvs_log_writer_shutdown(kvs_log_writer_t *writer);
int kvs_log_writer_emit(kvs_log_writer_t *writer,
                        kvs_log_level_t level,
                        const char *line,
                        size_t line_len);
int kvs_log_writer_is_async(const kvs_log_writer_t *writer);
size_t kvs_log_writer_dropped_count(const kvs_log_writer_t *writer);