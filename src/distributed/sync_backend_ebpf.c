#include "sync_backend.h"
#include "distributed.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static int g_center_fd = -1;
static int g_center_port = 0;

// 建立到本地中心通道的长连接：127.0.0.1:g_center_port
static int connect_center()
{
    if (g_center_fd >= 0) return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[SYNC_EBPF] socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(g_center_port);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[SYNC_EBPF] connect");
        close(fd);
        return -1;
    }

    g_center_fd = fd;
    // printf("[SYNC_EBPF] connected to center 127.0.0.1:%d\n", g_center_port);
    return 0;
}

int sync_backend_init(int sync_port)
{
    // 约定：ebpf center 端口 = sync_port + 1
    g_center_port = sync_port + 1;
    g_center_fd   = -1;
    // 连接会在第一次 broadcast 时创建
    // printf("[SYNC_EBPF] backend init, center port=%d\n", g_center_port);
    return 0;
}

int sync_backend_broadcast(const void *data, size_t len)
{
    if (!data || len == 0) return -1;
    if (g_center_port == 0) return -1;

    if (connect_center() != 0) {
        return -1;
    }

    ssize_t n = send(g_center_fd, data, len, 0);
    if (n < 0) {
        int err = errno;
        fprintf(stderr, "[SYNC_EBPF] send to center failed: %s\n", strerror(err));
        // 简单处理：关闭，下次重连
        close(g_center_fd);
        g_center_fd = -1;
        return -1;
    }
    return 0;
}

void sync_backend_shutdown(void)
{
    if (g_center_fd >= 0) {
        close(g_center_fd);
        g_center_fd = -1;
    }
    g_center_port = 0;
    // printf("[SYNC_EBPF] backend shutdown\n");
}
