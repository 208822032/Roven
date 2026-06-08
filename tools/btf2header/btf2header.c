/* btf2header — 从 raw BTF 生成 vmlinux.h
 *
 * 用法: btf2header [/sys/kernel/btf/vmlinux] > vmlinux.h
 *
 * 与 bpftool btf dump file /sys/kernel/btf/vmlinux format c 不同:
 *   1. 使用 btf__parse_raw() 而非 btf__parse_elf()，直接处理 raw BTF
 *   2. 兼容 Linux 5.4 内核的 /sys/kernel/btf/vmlinux
 *   3. 耐受无法 dump 的类型 (跳过而非退出)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "bpf/btf.h"
#include "bpf/libbpf.h"

static void do_printf(void *ctx, const char *fmt, va_list args) {
    (void)ctx;
    vfprintf(stdout, fmt, args);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/sys/kernel/btf/vmlinux";

    struct btf *btf = btf__parse_raw(path);
    if (libbpf_get_error(btf)) {
        long err = libbpf_get_error(btf);
        fprintf(stderr, "btf2header: 无法解析 raw BTF 从 %s: %ld\n", path, err);
        return 1;
    }

    struct btf_dump *d = btf_dump__new(btf, do_printf, NULL, NULL);
    if (libbpf_get_error(d)) {
        long err = libbpf_get_error(d);
        fprintf(stderr, "btf2header: 无法创建 BTF dump: %ld\n", err);
        btf__free(btf);
        return 1;
    }

    /* CO-RE 所需的 pragma */
    printf("#ifndef __VMLINUX_H__\n");
    printf("#define __VMLINUX_H__\n\n");
    printf("#ifndef BPF_NO_PRESERVE_ACCESS_INDEX\n");
    printf("#pragma clang attribute push (__attribute__((preserve_access_index)), apply_to = record)\n");
    printf("#endif\n\n");

    __u32 nr_types = btf__type_cnt(btf);
    int skipped = 0;
    for (__u32 id = 1; id <= nr_types; id++) {
        int err = btf_dump__dump_type(d, id);
        if (err < 0) {
            skipped++;
            continue;
        }
    }
    if (skipped > 0)
        fprintf(stderr, "btf2header: 跳过 %d 个无法 dump 的类型\n", skipped);

    printf("\n#ifndef BPF_NO_PRESERVE_ACCESS_INDEX\n");
    printf("#pragma clang attribute pop\n");
    printf("#endif\n");
    printf("\n#endif /* __VMLINUX_H__ */\n");

    btf_dump__free(d);
    btf__free(btf);
    return 0;
}
