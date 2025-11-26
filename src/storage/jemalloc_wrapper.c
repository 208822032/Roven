#include "jemalloc_wrapper.h"
#include "../../jemalloc/include/jemalloc/jemalloc.h"
#include <execinfo.h>
#include <stdio.h>

/*
 * 当前构建的 jemalloc 是“无前缀版本”，库里导出的是 malloc/free/realloc，
 * 而不是 je_malloc/je_free/je_realloc。
 *
 * 因此这里直接调用标准的 malloc/free/realloc。因为最终链接时我们链接了
 * jemalloc/lib/libjemalloc.a，实际执行的就是 jemalloc 的实现。
 */

void* jemalloc_alloc_impl(size_t size) {
    return je_malloc(size);
}

void jemalloc_free_impl(void* ptr) {
    // // 临时调试：打印调用栈
    // void *bt[16];
    // int n = backtrace(bt, 16);
    // fprintf(stderr, "[JEMALLOC_FREE] ptr=%p, backtrace depth=%d\n", ptr, n);
    // backtrace_symbols_fd(bt, n, fileno(stderr));


    je_free(ptr);
}

void* jemalloc_realloc_impl(void* ptr, size_t new_size) {
    return je_realloc(ptr, new_size);
}