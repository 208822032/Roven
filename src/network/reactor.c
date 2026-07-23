


#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/time.h>

#include <sys/uio.h> // 用于writev
#include <fcntl.h>   // 用于fcntl
#include <netinet/tcp.h>
#include <stdint.h>

#include "server.h"
#include "kvs_api.h"
#include "resp_protocol.h"
#include "distributed.h"
#include "kvs_log.h"
#include "kvs_memory.h"
#include "kvs_command_executor.h"
#include "kvs_command_task.h"
#include "kvs_response_task.h"
#include "kvs_async_net_adapter.h"
#include "kvs_replication_fallback.h"
#include "kvs_shutdown.h"

#include "net_session.h" //通用会话层


#define CONNECTION_SIZE			1024 // 1024 * 1024

#define MAX_PORTS			1

#define MAX_IOVEC_COUNT     1024   //最大iovec数量
#define MAX_SINGLE_SEND     1024 * 1024  //单次发送最大数据量 1MB

int accept_cb(int fd);
int recv_cb(int fd);
int send_cb(int fd);

#if ENABLE_KVSTORE
typedef int (*msg_handler)(char **tokens, int count, size_t *token_lens, char *response);

static msg_handler kvs_handler;
#endif

//连接扩展结构 用于管理大缓冲区
typedef struct{
    net_session_t session; //通用会话层 封装recv/send缓冲区和RESP解析器
    size_t send_offset;//发送偏移量

    struct iovec *iovecs;//用于分散/聚集IO
    int iovec_count;  //iovec数量

    int is_replication;//是否是复制连接
} conn_extension_t;

int epfd = 0;
struct conn conn_list[CONNECTION_SIZE] = {0};

//全局连接扩展数组
static conn_extension_t *conn_extensions[CONNECTION_SIZE] = {0};
static uint64_t g_reactor_next_session_id = 1;
static uint32_t g_reactor_fd_generation[CONNECTION_SIZE] = {0};

extern kvs_config_t g_cfg;

// 设置非阻塞IO
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 设置TCP_NODELAY选项减少小包延迟
int set_tcp_nodelay(int fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

// 设置TCP缓冲区大小
int set_tcp_buffer_size(int fd, int recv_size, int send_size) {
    if (recv_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recv_size, sizeof(recv_size));
    }
    if (send_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size));
    }
    return 0;
}

int set_event(int fd, int event, int flag) {

	struct epoll_event ev;
	ev.events = event ? (event | EPOLLET) : 0;
	ev.data.fd = fd;

	if (flag) {  // non-zero add
		return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
	} else {  // zero mod
		return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
	}

}

static void reactor_conn_release(void *owner) {
    conn_extension_t *ext = (conn_extension_t *)owner;

    if (!ext) {
        return;
    }

    if (net_session_ref_dec(&ext->session) == 0 &&
        net_session_is_closing(&ext->session)) {
        net_session_free(&ext->session);
        if (ext->iovecs) {
            kvs_free(ext->iovecs);
            ext->iovecs = NULL;
        }
        kvs_free(ext);
    }
}

static void reactor_conn_ref_inc(conn_extension_t *ext) {
    if (!ext) {
        return;
    }
    net_session_ref_inc(&ext->session);
}

// 初始化连接扩展
int init_conn_extension(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    
    conn_extension_t *ext = (conn_extension_t *)kvs_malloc(sizeof(conn_extension_t));
    if (!ext) {
        return -1;
    }
    memset(ext, 0, sizeof(conn_extension_t));
    
    // 初始化 net_session：这里直接给 16MB 初始缓冲，和原来的 large_buffer 一致
    if (net_session_init(&ext->session, 8 * 1024 * 1024, 8 * 1024 * 1024) < 0) {
        kvs_free(ext);
        return -1;
    }

    g_reactor_fd_generation[fd]++;
    if (g_reactor_fd_generation[fd] == 0) {
        g_reactor_fd_generation[fd] = 1;
    }
    net_session_set_identity(
        &ext->session,
        g_reactor_next_session_id++,
        g_reactor_fd_generation[fd],
        0
    );
    
    // 分配iovec数组
    ext->iovecs = kvs_malloc(MAX_IOVEC_COUNT * sizeof(struct iovec));
    if (!ext->iovecs) {
        net_session_free(&ext->session);
        kvs_free(ext);
        return -1;
    }

    ext->iovec_count = 0;
    ext->send_offset = 0;
    ext->is_replication = 0;
    conn_extensions[fd] = ext;
    
    return 0;
}

// 释放连接扩展
void free_conn_extension(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;
    
    conn_extension_t *ext = conn_extensions[fd];
    conn_extensions[fd] = NULL;
    reactor_conn_release(ext);
}

//注册事件 + 初始化每一个连接状态
int event_register(int fd, int event) {

	if (fd < 0 || fd >= CONNECTION_SIZE) return -1;

	memset(&conn_list[fd], 0, sizeof(struct conn));
	conn_list[fd].fd = fd;
	conn_list[fd].r_action.recv_callback = recv_cb;
	conn_list[fd].send_callback = send_cb;

    // 初始化连接扩展
    if (init_conn_extension(fd) < 0) {
        return -1;
    }

    // 设置非阻塞和TCP优化
    set_nonblocking(fd);
    set_tcp_nodelay(fd);
    set_tcp_buffer_size(fd, 8 * 1024 * 1024, 8 * 1024 * 1024); // 8MB 缓冲区

	if (set_event(fd, event | EPOLLET, 1) < 0) {
        net_session_mark_closing(&conn_extensions[fd]->session);
        free_conn_extension(fd);
        conn_list[fd].fd = -1;
        return -1;
    }

	return 0;

}

void event_unregister(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return;

    conn_extension_t *ext = conn_extensions[fd];
    if (ext) {
        net_session_mark_closing(&ext->session);
    }
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    free_conn_extension(fd);

    //关闭文件描述符
    if(conn_list[fd].fd > 0){
        close(conn_list[fd].fd);
        conn_list[fd].fd = -1;
    }
    
}


// listenfd(sockfd) --> EPOLLIN --> accept_cb
int accept_cb(int fd) {
    int accepted = 0;

    for (;;) {
        struct sockaddr_in clientaddr;
        socklen_t len = sizeof(clientaddr);
        int clientfd = accept(fd, (struct sockaddr*)&clientaddr, &len);

        if (clientfd >= 0) {
            accepted++;
            if (clientfd >= CONNECTION_SIZE) {
                KVS_LOG_WARN("net", "accepted fd %d exceeds connection table size", clientfd);
                close(clientfd);
                continue;
            }

            if (event_register(clientfd, EPOLLIN) < 0) {
                KVS_LOG_ERROR_ERRNO("net", "failed to register accepted fd %d", clientfd);
                close(clientfd);
                continue;
            }
            continue;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        KVS_LOG_ERROR_ERRNO("net", "accept failed: fd=%d", fd);
        return accepted > 0 ? 0 : -1;
    }
	return 0;
}

static long long g_total_cmd[CONNECTION_SIZE];

static int reactor_submit_async_command(int fd, conn_extension_t *ext);
static void reactor_process_executor_event(void);

//net_session 的handler 
//实现 REPLSYNC + is_replication + kvs_handler 调用
static int reactor_session_handler(resp_parser_t* parser,
                                    char* response_buf,
                                    size_t response_buf_cap,
                                    void *user_data){

    conn_extension_t* ext = (conn_extension_t* )user_data;
    
    if(!parser || parser->arg_index <= 0 || !parser->tokens[0]){
        const char* err = "-ERR invalid command\r\n";
        size_t len = strlen(err);
        if(len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    char *cmd_name = parser->tokens[0];
    int resp_len = 0;

        //单独处理REPLSYNC命令
        if(g_cfg.ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR &&
           strcasecmp(cmd_name, "REPLSYNC") == 0){
            extern distributed_config_t global_dist_config;

            const char* ok_resp = "+OK\r\n";
            const char* err_resp = "-ERR REPLSYNC not allowed in current role\r\n";

            if(global_dist_config.role == ROLE_SLAVE){
                //将该连接标记为复制连接
                ext->is_replication = 1;
                KVS_LOG_INFO("net", "reactor synchronous handler accepted REPLSYNC");

                //回复+OK
                size_t ok_len = strlen(ok_resp);
                if(ok_len > response_buf_cap) ok_len = response_buf_cap;
                memcpy(response_buf, ok_resp, ok_len);
                return (int)ok_len;
            }else{
                //只允许从节点接收 REPLSYNC
                size_t err_len = strlen(err_resp);
                if(err_len > response_buf_cap) err_len = response_buf_cap;
                memcpy(response_buf, err_resp, err_len);
                return (int)err_len;
            }
        }

        //其他命令 根据is_replication选择上下文
        int old_ctx = get_sync_context();
        if(ext->is_replication){
            set_sync_context(2);//复制连接上下文
        }else{
            set_sync_context(0);//普通连接上下文
        }

        resp_len = kvs_handler(parser->tokens,
                            parser->arg_index,
                            parser->token_lens,
                    response_buf);

        set_sync_context(old_ctx);


    if(resp_len < 0 || resp_len > (int)response_buf_cap){
        //不合法长度 返回错误
        const char* err = "-ERR internal handler error\r\n";
        size_t len = strlen(err);
        if(len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    return resp_len;
}

// 准备iovec结构用于分散写入
int prepare_iovecs(int fd) {
    conn_extension_t *ext = conn_extensions[fd];
    if (!ext) return -1;
    netbuf_t* send_buf = &ext->session.send_buf;
    
    if (send_buf->size <= 0) {
        ext->iovec_count = 0;
        return 0;
    }
    
    // 将发送缓冲区分割成多个iovec，每个最多MAX_SINGLE_SEND字节
    size_t remaining = send_buf->size - ext->send_offset;
    ext->iovec_count = 0;
    
    while (remaining > 0 && ext->iovec_count < MAX_IOVEC_COUNT) {
        size_t chunk_size = remaining > MAX_SINGLE_SEND ? MAX_SINGLE_SEND : remaining;

        ext->iovecs[ext->iovec_count].iov_base = send_buf->data + ext->send_offset;
        ext->iovecs[ext->iovec_count].iov_len  = chunk_size;

        ext->send_offset += chunk_size;
        remaining        -= chunk_size;
        ext->iovec_count++;
    }

    return ext->iovec_count;
}

static int reactor_command_is_replsync(net_session_t *session) {
    resp_parser_t *parser;

    if (!session) {
        return 0;
    }
    parser = &session->resp_parser;
    return parser->arg_index > 0 &&
           parser->tokens[0] &&
           strcasecmp(parser->tokens[0], "REPLSYNC") == 0;
}

static int reactor_append_inline_response(net_session_t *session,
                                          const char *response,
                                          size_t len) {
    if (!session || !response) {
        return -1;
    }
    if (session->send_buf.capacity - session->send_buf.size < len &&
        netbuf_reserve(&session->send_buf, session->send_buf.size + len) < 0) {
        return -1;
    }
    memcpy(session->send_buf.data + session->send_buf.size, response, len);
    session->send_buf.size += len;
    return 0;
}

static int reactor_finish_inline_fallback_command(int fd,
                                                  net_session_t *session,
                                                  resp_parser_t *parser,
                                                  size_t cmd_len,
                                                  const char *response) {
    if (reactor_append_inline_response(session, response, strlen(response)) < 0) {
        resp_parser_reset(parser);
        return -1;
    }
    netbuf_consume_front(&session->recv_buf, cmd_len);
    resp_parser_reset(parser);
    g_total_cmd[fd]++;
    set_event(fd, EPOLLIN | EPOLLOUT, 0);
    return 1;
}

static void reactor_destroy_response(kvs_response_task_t *resp) {
    if (!resp) {
        return;
    }
    kvs_response_task_destroy(resp);
    kvs_free(resp);
}

static int reactor_target_is_current(const kvs_async_net_response_target_t *target) {
    int fd;

    if (!target || target->owner_type != KVS_ASYNC_NET_OWNER_REACTOR) {
        return 0;
    }
    fd = target->fd;
    if (fd >= 0 && fd < CONNECTION_SIZE &&
        conn_list[fd].fd == fd &&
        conn_extensions[fd] == (conn_extension_t *)target->owner) {
        return 1;
    }
    return 0;
}

static int reactor_async_validate_owner(
    const kvs_async_net_response_target_t *target,
    kvs_async_net_drop_reason_t *reason
) {
    conn_extension_t *ext;
    net_session_t *session;

    if (reason) {
        *reason = KVS_ASYNC_NET_DROP_SESSION_MISMATCH;
    }

    if (!reactor_target_is_current(target)) {
        return 0;
    }

    ext = (conn_extension_t *)target->owner;
    session = &ext->session;
    if (session->session_id != target->session_id ||
        session->generation != target->generation) {
        return 0;
    }
    if (net_session_is_closing(session)) {
        if (reason) {
            *reason = KVS_ASYNC_NET_DROP_SESSION_CLOSING;
        }
        return 0;
    }

    return 1;
}

static int reactor_async_append_response(const kvs_async_net_response_target_t *target) {
    conn_extension_t *ext;
    net_session_t *session;
    size_t len;

    if (!reactor_target_is_current(target)) {
        return -1;
    }

    ext = (conn_extension_t *)target->owner;
    session = &ext->session;
    len = target->response_len;
    if (session->send_buf.capacity - session->send_buf.size < len &&
        netbuf_reserve(&session->send_buf, session->send_buf.size + len) < 0) {
        return -1;
    }

    memcpy(session->send_buf.data + session->send_buf.size,
           target->response_buf,
           len);
    session->send_buf.size += len;
    return 0;
}

static void reactor_async_drop_response(
    const kvs_async_net_response_target_t *target,
    kvs_async_net_drop_reason_t reason
) {
    int executor_reason = KVS_COMMAND_EXECUTOR_DROP_SESSION_MISMATCH;

    if (reason == KVS_ASYNC_NET_DROP_SESSION_CLOSING) {
        executor_reason = KVS_COMMAND_EXECUTOR_DROP_SESSION_CLOSING;
    } else if (reason == KVS_ASYNC_NET_DROP_APPEND_FAILED) {
        executor_reason = KVS_COMMAND_EXECUTOR_DROP_APPEND_FAILED;
        /*
         * APPEND_FAILED connection cleanup is centralized here. append_response
         * only reports reserve/copy failure; drop_response owns the decision to
         * unregister the still-current reactor fd.
         */
        if (reactor_target_is_current(target)) {
            event_unregister(target->fd);
        }
    }

    kvs_command_executor_record_response_drop(executor_reason);
}

static void reactor_async_release_session(kvs_async_net_response_target_t *target) {
    void (*release_fn)(void *) = NULL;
    void *owner = NULL;

    if (!target) {
        return;
    }

    release_fn = target->release_session;
    owner = target->owner;
    target->release_session = NULL;
    target->owner = NULL;
    target->session = NULL;
    if (release_fn && owner) {
        release_fn(owner);
    }
}

static int reactor_async_enable_write(const kvs_async_net_response_target_t *target) {
    if (!reactor_target_is_current(target)) {
        return -1;
    }
    return set_event(target->fd, EPOLLIN | EPOLLOUT, 0);
}

static int reactor_async_restore_read(const kvs_async_net_response_target_t *target) {
    conn_extension_t *ext;
    net_session_t *session;

    if (!reactor_target_is_current(target)) {
        return -1;
    }

    ext = (conn_extension_t *)target->owner;
    session = &ext->session;
    /*
     * After one async response is appended, a pipelined request may already be
     * buffered in recv_buf. Submitting it can fail and unregister the fd, which
     * releases the reactor-owned session reference. dispatch_response will then
     * release the executor response target's reference. These are distinct
     * refs, and net_session's atomic refcount is the ownership boundary.
     * single_inflight cleanup must remain idempotent across this path.
     */
    if (session->recv_buf.size > 0 && !net_session_is_closing(session)) {
        int processed = reactor_submit_async_command(target->fd, ext);
        if (processed < 0) {
            event_unregister(target->fd);
            return -1;
        }
    }
    return 0;
}

static int reactor_async_wake_owner(int owner_poller_id) {
    (void)owner_poller_id;
    return 0;
}

static const kvs_async_net_ops_t g_reactor_async_net_ops = {
    .validate_owner = reactor_async_validate_owner,
    .append_response = reactor_async_append_response,
    .drop_response = reactor_async_drop_response,
    .release_session = reactor_async_release_session,
    .restore_read = reactor_async_restore_read,
    .enable_write = reactor_async_enable_write,
    .wake_owner = reactor_async_wake_owner,
};

static void reactor_handle_executor_response(kvs_response_task_t *resp) {
    kvs_async_net_response_target_t target;

    if (!resp) {
        return;
    }

    /*
     * 第一步：把 response task 中的网络回包信息整理成 adapter target。
     * 这里会复制 fd、owner、session 身份、请求序号以及响应缓冲区指针，
     * 但不会复制响应内容；target.response_buf 仍然指向 resp 的缓冲区。
     */
    kvs_async_net_response_target_from_task(&target, resp);

    /*
     * 第二步：交给 Reactor 网络适配器处理。
     * dispatch 会重新校验 fd/owner/session_id/generation，确认连接仍是原连接；
     * 校验通过后把响应内容复制到 session.send_buf，开启 EPOLLOUT，并恢复读事件。
     * 如果连接已关闭、fd 已复用或发送缓冲扩容失败，dispatch 会走丢弃和引用释放路径。
     */
    kvs_async_net_dispatch_response(&g_reactor_async_net_ops, &target);

    /*
     * 第三步：销毁 response task。
     * 必须放在 dispatch 之后，因为 dispatch 期间还要读取 response_buf；
     * dispatch 成功时内容已经复制到 send_buf，失败时也已经完成连接引用清理。
     */
    reactor_destroy_response(resp);
}

static void reactor_process_executor_event(void) {
    int event_fd = kvs_command_executor_response_event_fd();
    uint64_t value;

    if (event_fd < 0) {
        return;
    }

    for (;;) {
        ssize_t n = read(event_fd, &value, sizeof(value));
        if (n == (ssize_t)sizeof(value)) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        break;
    }

    for (;;) {
        kvs_response_task_t *resp = NULL;
        int ret = kvs_command_executor_poll_response_for_owner(
            KVS_ASYNC_NET_OWNER_REACTOR,
            0,
            &resp
        );
        if (ret <= 0 || !resp) {
            break;
        }
        reactor_handle_executor_response(resp);
    }
}

/*
 * 把 parser 中已经解析好的“普通命令”封装成异步 command task 并提交 executor。
 *
 * 调用前提：
 * - net_session_parse_one_resp 已确认 recv_buf 开头有一条完整 RESP 命令；
 * - session->resp_parser 已保存该命令的 tokens 和 token_lens；
 * - cmd_len 是这条命令在 recv_buf 中占用的字节数。
 *
 * 主要工作：申请 response/task、校验并深拷贝参数、占用 single_inflight、增加
 * 连接引用、记录回包身份、消费 recv_buf 中的完整帧，最后提交 command executor。
 * executor 完成后会把连接信息转交给 response task，并通过 eventfd 唤醒 Reactor。
 *
 * 返回 1 表示 task 已提交；返回 0 表示无法开始新的在途命令；返回 -1 表示内存、
 * 参数复制或队列提交失败，调用方通常关闭连接。
 */
static int reactor_submit_command_task(int fd,
                                       conn_extension_t *ext,
                                       size_t cmd_len,
                                       int is_replication) {
    /* 当前连接的通用会话，包含 parser、收发缓冲和生命周期状态。 */
    net_session_t *session = &ext->session;
    /* parser 当前保存的是本次待提交命令的 RESP 参数。 */
    resp_parser_t *parser = &session->resp_parser;
    /*
     * 异步回包对象。它拥有独立响应缓冲区，并最终携带连接身份返回 Reactor。
     * task 只借用它，executor 完成后会把它放入响应队列。
     */
    kvs_response_task_t *resp = NULL;
    /* 命令对象：executor 使用它执行 handler、持锁和 post-commit。 */
    kvs_command_task_t *task = NULL;
    /* 当前连接的递增请求序号，用于追踪同一连接内的请求顺序。 */
    uint64_t request_seq;
    /* 暂存网络线程的线程本地复制上下文，build 后必须恢复。 */
    int old_sync_context;
    /* build 的返回值；错误也可能已被编码为可异步发送的 RESP 错误响应。 */
    int build_ret;

    /* task 和 response 都必须在堆上分配，因为函数返回后 executor 仍要使用它们。 */
    resp = (kvs_response_task_t *)kvs_malloc(sizeof(*resp));
    task = (kvs_command_task_t *)kvs_malloc(sizeof(*task));
    if (!resp || !task) {
        if (resp) kvs_free(resp);
        if (task) kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }

    /* 清除分配块中的旧数据，保证 destroy 和错误路径可安全判断各字段。 */
    memset(resp, 0, sizeof(*resp));
    memset(task, 0, sizeof(*task));

    /* response task 申请自己的响应缓冲区，不能借用 parser 或网络栈上的内存。 */
    if (kvs_response_task_init_owned(resp, KVS_MAX_RESPONSE_LENGTH) < 0) {
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }

    /*
     * 复制连接中的普通命令需要在 replica apply 上下文中校验和执行，
     * 以绕过 slave 对普通客户端写请求的只读限制。
     */
    if (is_replication) {
        task->sync_context = 2;
        task->is_replication = 1;
    }

    /*
     * build 查命令表、校验 arity、解析 command_context 并选择锁模式。
     * 它读取线程本地 sync_context，因此临时切换到本命令所属的上下文，
     * 随后无条件恢复网络线程原来的上下文。
     */
    old_sync_context = get_sync_context();
    set_sync_context(is_replication ? 2 : 0);
    build_ret = kvs_command_task_build(task,
                                       parser->tokens,
                                       parser->arg_index,
                                       parser->token_lens,
                                       resp);
    set_sync_context(old_sync_context);

    /*
     * build 失败但 task->status 大于 0 时，通常已在 resp 中生成一条 RESP 错误。
     * 这种情况仍要继续提交 task，让错误沿着正常的 executor → response → Reactor
     * 链路回到客户端；只有没有可发送响应的失败才在这里直接清理并报错。
     */
    if (build_ret < 0 && task->status <= 0) {
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }

    /*
     * parser 的 token 内存会在本函数返回后被 reset 或被下一条请求覆盖。
     * 因此交给 executor 前必须深拷贝 argv、argv_lens，并重新解析 ctx 中的指针。
     */
    if (kvs_command_task_copy_argv(task) < 0) {
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }

    /* 先取得请求序号，再把连接标记为“已有一条命令在途”。 */
    request_seq = net_session_next_request_seq(session);
    if (!net_session_try_begin_inflight(session)) {
        /* 连接关闭或已有在途命令时，不保留本次新建的 task/response。 */
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return 0;
    }

    /*
     * task 即将跨线程存活，因此增加连接扩展对象引用，并完整记录回包目标。
     * executor 自身不解释 session_owner；最终由 Reactor 通过 session_release
     * 减少引用，并用 fd + session_id + generation 防止 fd 复用误投递。
     */
    reactor_conn_ref_inc(ext);
    task->session = session;
    task->session_owner = ext;
    task->session_release = reactor_conn_release;
    task->fd = fd;
    task->session_id = session->session_id;
    task->generation = session->generation;
    task->request_seq = request_seq;
    task->owner_type = KVS_ASYNC_NET_OWNER_REACTOR;
    task->owner_poller_id = session->owner_poller_id;
    task->sync_context = is_replication ? 2 : 0;
    task->is_replication = is_replication ? 1 : 0;

    /*
     * task 已拥有参数副本后，当前完整帧可以从 recv_buf 删除，parser 也可复用。
     * 如果后续队列提交失败，函数会关闭连接，而不会尝试把命令字节放回缓冲区。
     */
    netbuf_consume_front(&session->recv_buf, cmd_len);
    resp_parser_reset(parser);

    /*
     * 将任务移交给唯一 command executor。失败时必须撤销 single_inflight 和连接
     * 引用，再销毁 task/response，避免连接永远卡在“有在途命令”的状态。
     */
    if (kvs_command_executor_submit(task) < 0) {
        net_session_end_inflight(session);
        reactor_conn_release(ext);
        task->session = NULL;
        task->session_owner = NULL;
        task->session_release = NULL;
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        return -1;
    }

    /* 记录该 fd 已接收一条命令；响应尚未生成时通常只继续监听 EPOLLIN。 */
    g_total_cmd[fd]++;
    if (session->send_buf.size > 0) {
        /* 少数情况下已有待发数据，需要同时安排写事件。 */
        set_event(fd, EPOLLOUT, 0);
    } else {
        /* 正常情况等待后续读事件或 executor 的 eventfd 响应事件。 */
        set_event(fd, EPOLLIN, 0);
    }
    return 1;
}

/*
 * 处理当前 Reactor 中的复制兼容命令。
 *
 * 这不是完整的 TCP 主从复制协议实现，而是 eBPF experimental mirror 场景使用的
 * fallback 路径。它根据已经解析好的 RESP tokens 将命令分为三类：
 * 1. REPLSYNC：复制连接的简单握手；
 * 2. 已复制的普通 KV/TTL mutation：转换为 ms_op_t，交给 command executor 应用；
 * 3. 其他普通命令：回到常规 command task 路径执行。
 *
 * 返回 1 表示已处理一条命令；返回 -1 表示分类、提交或写响应失败，调用方应
 * 关闭连接。这里的 parser 参数来自 net_session_parse_one_resp。
 */
static int reactor_handle_replication_fallback(int fd,
                                               conn_extension_t *ext,
                                               size_t cmd_len) {
    /* 当前连接的通用会话，负责 recv_buf、send_buf 和 RESP parser。 */
    net_session_t *session = &ext->session;
    /* 本条完整 RESP 命令的已解析 tokens/token_lens。 */
    resp_parser_t *parser = &session->resp_parser;
    /*
     * 分类器填写的确定性复制 mutation：包括操作类型、key/value 长度和 TTL。
     * 其中字符串指针暂时借用 parser token，提交函数需要在异步使用前复制数据。
     */
    kvs_replication_apply_t apply;
    /* 命令分类结果：握手、复制 apply、普通命令或未知命令。 */
    kvs_replication_fallback_kind_t kind;

    /*
     * 仅接受参数数量和格式均符合要求的普通 KV/TTL 写命令作为 replicated apply。
     * 不认识的命令会被标记为 NORMAL_COMMAND 或 UNKNOWN，而不是盲目当作复制写入。
     */
    kind = kvs_replication_fallback_classify(parser->tokens,
                                             parser->arg_index,
                                             parser->token_lens,
                                             &apply);
    if (kind == KVS_REPL_FALLBACK_REPLSYNC) {
        /* distributed 配置决定当前节点是否允许把这个连接升级为复制连接。 */
        extern distributed_config_t global_dist_config;

        /*
         * 仅 experimental mirror 的 slave 接受 REPLSYNC。
         * 成功后 ext->is_replication 会影响后续命令的分流和同步上下文。
         */
        if (g_cfg.ebpf_mode == KVS_EBPF_EXPERIMENTAL_MIRROR &&
            global_dist_config.role == ROLE_SLAVE) {
            ext->is_replication = 1;
            /* 追加 +OK，消费本条命令，重置 parser，并打开 EPOLLOUT 发送响应。 */
            return reactor_finish_inline_fallback_command(fd, session, parser, cmd_len, "+OK\r\n");
        }

        /* 当前节点或模式不允许握手，返回错误响应但仍正常消费这条命令。 */
        return reactor_finish_inline_fallback_command(
            fd,
            session,
            parser,
            cmd_len,
            "-ERR REPLSYNC not allowed in current role\r\n"
        );
    }

    /*
     * 只有已完成 REPLSYNC 的复制连接，才可提交 SET/DEL/MOD/PEXPIREAT/PERSIST
     * 等已分类 mutation。网络线程不直接修改存储，而是提交 replicated task，
     * 保持与普通命令相同的 command owner 提交边界。
     */
    if (kind == KVS_REPL_FALLBACK_REPLICATED_APPLY && ext->is_replication) {
        if (kvs_command_executor_submit_replicated(apply.op,
                                                  apply.key,
                                                  apply.key_len,
                                                  apply.value,
                                                  apply.value_len,
                                                  apply.expire_at_ms) != 0) {
            /* 提交失败时不生成成功确认；调用方随后会关闭连接。 */
            resp_parser_reset(parser);
            return -1;
        }

        /* replicated task 已接管 mutation；当前网络层只返回确认并消费输入帧。 */
        return reactor_finish_inline_fallback_command(fd, session, parser, cmd_len, "+OK\r\n");
    }

    /*
     * 分类器不认识为复制 mutation 的普通命令，仍通过正常 command task 执行。
     * 若连接已是复制连接，会传入 is_replication=1，使 task 使用 replica apply
     * 的同步上下文，而不是触发 slave 的普通客户端写保护。
     */
    if (kind == KVS_REPL_FALLBACK_NORMAL_COMMAND) {
        return reactor_submit_command_task(fd, ext, cmd_len, ext->is_replication ? 1 : 0);
    }

    /*
     * UNKNOWN 通常表示命令参数数量不对、TTL 时间无法解析，或复制连接发送了
     * 不允许的写格式。重置 parser 后返回错误，由上层关闭该不可信连接。
     */
    KVS_LOG_WARN("net",
                 "reactor eBPF replication fallback rejected command without synchronous handler: fd=%d",
                 fd);
    resp_parser_reset(parser);
    return -1;
}

/*
 * 从当前连接的 recv_buf 中取出一条完整命令，并交给正确的异步执行路径。
 *
 * recv_cb 已负责把内核数据读入 recv_buf。本函数不再调用 recv，也不会一次
 * 处理整个 Pipeline：它最多解析一条完整 RESP 命令。普通命令会被包装成
 * kvs_command_task_t 并提交给 command executor；复制相关命令则进入专用的
 * replication fallback 路径。
 *
 * 返回值：
 * - 1：已成功提交或处理一条命令；
 * - 0：连接正在关闭、已有在途命令，或 recv_buf 只有半包；
 * - 小于 0：RESP 协议错误、任务提交失败或复制处理失败，调用方应关闭连接。
 */
static int reactor_submit_async_command(int fd, conn_extension_t *ext) {
    /* 当前连接的通用会话，保存 recv_buf、parser 和 single_inflight。 */
    net_session_t *session;
    /* net_session_parse_one_resp 输出的一条完整 RESP 帧长度。 */
    size_t cmd_len = 0;
    /* 单命令解析结果：1 为完整帧，0 为半包，负数为协议/限制错误。 */
    int parse_ret;

    /* 没有连接扩展对象时无法取得 session，也无法安全提交任务。 */
    if (!ext) {
        return -1;
    }
    session = &ext->session;

    /*
     * 关闭中的连接不再接收新命令；已有在途命令时也暂不解析下一条 Pipeline
     * 命令。这样同一连接的响应顺序始终与请求顺序一致。
     */
    if (net_session_is_closing(session) || session->single_inflight) {
        return 0;
    }

    /*
     * 仅探测并解析 recv_buf 开头的一条完整 RESP 命令。
     * 成功时 parser 内保留 tokens/token_lens，普通提交路径会立即深拷贝它们。
     * 此函数尚未消费 recv_buf；消费动作由后续提交或复制处理路径完成。
     */
    parse_ret = net_session_parse_one_resp(fd, session, &cmd_len);
    if (parse_ret <= 0) {
        return parse_ret;
    }

    /*
     * 复制连接或 REPLSYNC 命令不走普通客户端命令路径。
     * fallback 会分类处理握手、已复制 mutation 和普通命令，并负责自己的
     * 缓冲区消费及响应。
     */
    if (ext->is_replication || reactor_command_is_replsync(session)) {
        return reactor_handle_replication_fallback(fd, ext, cmd_len);
    }

    /*
     * 普通客户端命令：创建 response task 和 command task，深拷贝 argv，
     * 持有连接引用，设置 single_inflight，然后提交给单 command executor。
     */
    return reactor_submit_command_task(fd, ext, cmd_len, 0);
}

/*
 * Reactor 的 EPOLLIN 回调：尽可能把内核接收缓冲区的数据搬到 session.recv_buf。
 *
 * 本服务注册的是 EPOLLET（边缘触发）。因此一次收到可读通知后必须循环 recv，
 * 直到返回 EAGAIN/EWOULDBLOCK，表示“当前已经读空”。如果提前返回，内核中剩余
 * 的旧数据未必会再次触发 EPOLLIN，连接可能永久卡住。
 *
 * 读空后，本函数最多调用一次 reactor_submit_async_command：它只解析并提交一条
 * 完整 RESP 命令。single_inflight 为 1 时不继续提交 Pipeline 后续命令，等 executor
 * 响应回到网络线程、结束在途状态后再处理下一条，借此保持同连接响应顺序。
 *
 * 返回 0 表示本次读事件处理完成；返回 -1 表示连接或缓冲区出现错误，调用方会
 * 注销连接。fd 只能由 Reactor 所属线程调用。
 */
int recv_cb(int fd) {
    /* fd 必须在 reactor 的固定连接表范围内。 */
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    /* 连接已被 event_unregister 注销时，本次 epoll 旧事件直接忽略。 */
    if (conn_list[fd].fd < 0) return -1;

    /* 当前 fd 的 Reactor 私有连接对象，里面嵌入 net_session_t。 */
    conn_extension_t *ext = conn_extensions[fd];
    if (!ext) return -1;
    /* 收到的 TCP 字节先追加到这里；半包不会直接交给 RESP parser。 */
    netbuf_t* recv_buf = &ext->session.recv_buf;

    /* 本轮读到的累计字节数；当前主要用于调试/统计预留。 */
    int total_recv = 0;
    /* 本轮实际调用 recv 的成功次数；当前主要用于调试/统计预留。 */
    int recv_count = 0;

    /*
     * ET 读循环。只有 EAGAIN 才表示已读空；连接关闭、系统错误或内存不足
     * 都需要立即注销连接。
     */
    while (1) {
        /* 每次希望至少留出 64 KiB 空闲空间，减少频繁小步扩容。 */
        size_t min_space = 64 * 1024;
        if(recv_buf->capacity - recv_buf->size < min_space &&
           recv_buf->capacity < recv_buf->max_capacity){
            /* 本次期望容量，但绝不超过该连接请求缓冲区的配置上限。 */
            size_t target = recv_buf->size + min_space;
            if (target > recv_buf->max_capacity) target = recv_buf->max_capacity;
            /* 扩容失败说明无法再安全接收该连接的数据。 */
            if(netbuf_reserve(recv_buf, target) < 0){
                KVS_LOG_ERROR("net", "failed to reserve space in recv buffer for fd %d", fd);
                event_unregister(fd);
                return -1;
            }
        }

        /* 当前 recv_buf 还可写入的字节数。 */
        size_t remaining = recv_buf->capacity - recv_buf->size;
        if (remaining == 0) {
            /* 已达到最大请求缓冲区容量，不能再接收，防止无限占用内存。 */
            event_unregister(fd);
            return -1;
        }

        /* 单次 recv 最多读取 256 KiB，避免单连接一次循环占用过长时间。 */
        size_t recv_size = remaining > (256 * 1024) ? (256 * 1024) : remaining;

        /* 把新字节直接写到 recv_buf 当前有效数据之后。 */
        int count = recv(fd, 
                         recv_buf->data + recv_buf->size, 
                         recv_size, 
                         0);
        if (count <= 0) {
            if (count == 0) {
                /*
                 * 对端已关闭写端或整个连接。当前实现会立即注销 fd，
                 * 即使此前循环已读到完整请求，也不会再走下面的 RESP 解析。
                 * 因此调试请求链时，客户端不能在收到响应前提前关闭连接。
                 */
                event_unregister(fd);
                return 0;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* ET 下的正常结束：内核接收缓冲区已经读空，开始解析用户态缓冲。 */
                break;
            } else {
                /* EINTR 在当前实现中也会按读失败关闭连接。 */
                KVS_LOG_ERROR_ERRNO("net", "recv error for fd %d", fd);
                event_unregister(fd);
                return -1;
            }
        }

        /* 只有成功读取正数时，才提交新增字节并更新本轮统计。 */
        recv_buf->size += count;
        total_recv += count;
        recv_count++;
    }

    /*
     * 用户态缓冲有数据且没有旧命令在 executor 中时，尝试解析一条完整命令。
     * 半包时 reactor_submit_async_command 返回 0，数据会继续留在 recv_buf。
     */
    if (recv_buf->size > 0 && !ext->session.single_inflight) {
        /* processed 表示解析/提交结果，而非已处理的 Pipeline 命令总数。 */
        int processed = reactor_submit_async_command(fd, ext);
        if (processed < 0) {
            event_unregister(fd);
            return -1;
        }
    }

    /*
     * 根据待发送响应和在途命令状态更新 epoll 关注事件。
     * EPOLLOUT 只在 send_buf 有数据时需要；否则无意义地监听可写事件会造成
     * 忙循环。single_inflight 为 1 时，下一条 Pipeline 命令仍须等待响应完成。
     */
    netbuf_t* send_buf = &ext->session.send_buf;
    if(send_buf->size > 0){
        if (ext->session.single_inflight) {
            /* 有待发响应且仍有在途请求：优先等待发送完成。 */
            set_event(fd, EPOLLOUT, 0);
        } else {
            /* 有待发响应但没有在途命令：同时保留继续收包的能力。 */
            set_event(fd, EPOLLIN | EPOLLOUT, 0);
        }
    }else if(ext->session.single_inflight){
        /* 响应尚未由 executor 送回；继续监听读事件，但不会提交下一条命令。 */
        set_event(fd, EPOLLIN, 0);
    }else{
        /* 空闲连接只需要等待下一次可读事件。 */
        set_event(fd, EPOLLIN, 0);
    }
    
    return 0;
}




static long long g_total_sent_bytes[CONNECTION_SIZE] = {0};

int send_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    if (conn_list[fd].fd < 0) return -1;
    
    conn_extension_t *ext = conn_extensions[fd];
    if (!ext) return -1;
    netbuf_t* send_buf = &ext->session.send_buf;
    
    if (send_buf->size <= 0 || ext->send_offset >= send_buf->size) {
        // 没有数据要发送或已发送完毕
        ext->send_offset = 0;
        send_buf->size = 0;
        if (ext->session.single_inflight) {
            set_event(fd, EPOLLIN, 0);
        } else {
            set_event(fd, EPOLLIN, 0);
        }
        return 0;
    }

    long long sent_this_round = 0;

    // 使用循环发送，尽可能发送所有数据
    while (ext->send_offset < send_buf->size) {
        size_t remaining = send_buf->size - ext->send_offset;
        
        ssize_t sent = send(fd, 
                           send_buf->data + ext->send_offset, 
                           remaining, 
                           MSG_NOSIGNAL);  // 避免SIGPIPE信号
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // TCP缓冲区满，等待下次EPOLLOUT
                set_event(fd, EPOLLOUT | EPOLLIN, 0);
                return 0;
            }
            KVS_LOG_ERROR_ERRNO("net", "send error for fd %d", fd);
            event_unregister(fd);
            return -1;
        } else if (sent == 0) {
            KVS_LOG_WARN("net", "connection closed during send for fd %d", fd);
            event_unregister(fd);
            return -1;
        }
        
        ext->send_offset += sent;
        sent_this_round   += sent;
        g_total_sent_bytes[fd] += sent;
    }
    
    //全部发送完毕
    if(ext->send_offset >= send_buf->size){
        ext->send_offset = 0;
        send_buf->size = 0;
        if (ext->session.single_inflight) {
            set_event(fd, EPOLLIN, 0);
        } else {
            set_event(fd, EPOLLIN, 0);
        }
    }else{
        set_event(fd, EPOLLOUT | EPOLLIN, 0);
    }
    
    return 0;
}


int r_init_server(unsigned short port) {

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		KVS_LOG_ERROR_ERRNO("net", "socket failed");
        return -1;
    }


    // 设置SO_REUSEADDR和SO_REUSEPORT选项
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        KVS_LOG_ERROR_ERRNO("net", "setsockopt SO_REUSEADDR failed");
        close(sockfd);
        return -1;
    }

    #ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
		KVS_LOG_ERROR_ERRNO("net", "setsockopt SO_REUSEPORT failed");
        close(sockfd);
        return -1;
    }
    #endif



	struct sockaddr_in servaddr;
	servaddr.sin_family = AF_INET;
	if (inet_pton(AF_INET, g_cfg.listen_address, &servaddr.sin_addr) != 1) {
		KVS_LOG_ERROR("net", "invalid listen address: %s", g_cfg.listen_address);
		close(sockfd);
		return -1;
	}
	servaddr.sin_port = htons(port); // 0-1023, 

	if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr)) < 0) {
        KVS_LOG_ERROR_ERRNO("net", "bind failed on port %u", port);
		close(sockfd);
        return -1;
	}

	if (listen(sockfd, g_cfg.network_listen_backlog) < 0) {
        KVS_LOG_ERROR_ERRNO("net", "listen failed on port %u", port);
        close(sockfd);
        return -1;
    }
	//printf("listen finshed: %d\n", sockfd); // 3 

    // 设置非阻塞
    set_nonblocking(sockfd);

	return sockfd;

}

int reactor_start(unsigned short port, msg_handler handler) {
	int listener_count = 0;

	//unsigned short port = 2000;
	kvs_handler = handler;

	epfd = epoll_create(1);
	if (epfd < 0) {
		KVS_LOG_ERROR_ERRNO("net", "epoll_create1 failed");
        return -1;
    }

    // 初始化连接扩展数组
    memset(conn_extensions, 0, sizeof(conn_extensions));
    memset(conn_list, 0, sizeof(conn_list));

	for (int i = 0;i < MAX_PORTS;i ++) {
		int sockfd = r_init_server(port + i);
		if (sockfd < 0) continue;
		listener_count++;

		conn_list[sockfd].fd = sockfd;
		conn_list[sockfd].r_action.recv_callback = accept_cb;
		
		set_event(sockfd, EPOLLIN, 1);
	}
	if (listener_count == 0) {
		close(epfd);
		return -1;
	}

    int executor_event_fd = kvs_command_executor_response_event_fd();
    if (executor_event_fd < 0 || set_event(executor_event_fd, EPOLLIN, 1) != 0) {
        KVS_LOG_ERROR_ERRNO("net", "failed to register executor response eventfd");
        close(epfd);
        return -1;
    }

	struct epoll_event events[1024] = {0};
	while (!kvs_shutdown_requested()) { // mainloop
        if(kvs_shutdown_requested()) break;

		int nready = epoll_wait(epfd, events, 1024, -1); //-1
		if (nready < 0 && errno != EINTR) {
			KVS_LOG_ERROR_ERRNO("net", "epoll_wait failed");
            break;
        }

		for (int i = 0;i < nready;i ++) {
			int connfd = events[i].data.fd;
            if (connfd == executor_event_fd) {
                reactor_process_executor_event();
                continue;
            }
            if (conn_list[connfd].fd < 0) continue;  // 已关闭，忽略本次事件

			if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                event_unregister(connfd);
                continue;
            }

			if (events[i].events & EPOLLIN) {
				conn_list[connfd].r_action.recv_callback(connfd);
			} 

            if (connfd >= 0 && connfd < CONNECTION_SIZE && conn_list[connfd].fd < 0) {
                continue;
            }

			if (events[i].events & EPOLLOUT) {
				conn_list[connfd].send_callback(connfd);
			}

		}

	}

	for (int i = 0; i < CONNECTION_SIZE; i++) {
        if (conn_list[i].fd > 0) {
            event_unregister(i);
        }
    }
	
    reactor_process_executor_event();
    epoll_ctl(epfd, EPOLL_CTL_DEL, executor_event_fd, NULL);
	close(epfd);
	return 0;
}
