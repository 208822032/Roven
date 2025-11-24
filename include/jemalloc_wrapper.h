#pragma once

#include <stddef.h>

void* jemalloc_alloc_impl(size_t size);
void jemalloc_free_impl(void* ptr);
void* jemalloc_realloc_impl(void* ptr, size_t new_size);