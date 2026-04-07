//统一同步后端
#ifndef __SYNC_BACKEND_H__
#define __SYNC_BACKEND_H__

#include <stddef.h>

// 初始化同步后端（在 master 上调用）
int sync_backend_init(int sync_port);

// 向所有 slave 广播一条已经编码好的 master_slave_proto 消息
int sync_backend_broadcast(const void *data, size_t len);

// 向所有 slave 广播一条由调用方持有的已分配消息，队列消费后由后端释放
int sync_backend_broadcast_owned(void *msg);


// 关闭 / 清理
void sync_backend_shutdown(void);
#endif
