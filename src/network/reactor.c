


#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
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
        if(g_cfg.sync_mode == KVS_SYNC_EBPF && strcasecmp(cmd_name, "REPLSYNC") == 0){
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

    kvs_async_net_response_target_from_task(&target, resp);
    kvs_async_net_dispatch_response(&g_reactor_async_net_ops, &target);
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

static int reactor_submit_command_task(int fd,
                                       conn_extension_t *ext,
                                       size_t cmd_len,
                                       int is_replication) {
    net_session_t *session = &ext->session;
    resp_parser_t *parser = &session->resp_parser;
    kvs_response_task_t *resp = NULL;
    kvs_command_task_t *task = NULL;
    uint64_t request_seq;
    int old_sync_context;
    int build_ret;

    resp = (kvs_response_task_t *)kvs_malloc(sizeof(*resp));
    task = (kvs_command_task_t *)kvs_malloc(sizeof(*task));
    if (!resp || !task) {
        if (resp) kvs_free(resp);
        if (task) kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }
    memset(resp, 0, sizeof(*resp));
    memset(task, 0, sizeof(*task));

    if (kvs_response_task_init_owned(resp, KVS_MAX_RESPONSE_LENGTH) < 0) {
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }

    if (is_replication) {
        task->sync_context = 2;
        task->is_replication = 1;
    }
    old_sync_context = get_sync_context();
    set_sync_context(is_replication ? 2 : 0);
    build_ret = kvs_command_task_build(task,
                                       parser->tokens,
                                       parser->arg_index,
                                       parser->token_lens,
                                       resp);
    set_sync_context(old_sync_context);
    if (build_ret < 0 && task->status <= 0) {
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }
    if (kvs_command_task_copy_argv(task) < 0) {
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return -1;
    }

    request_seq = net_session_next_request_seq(session);
    if (!net_session_try_begin_inflight(session)) {
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        resp_parser_reset(parser);
        return 0;
    }

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

    netbuf_consume_front(&session->recv_buf, cmd_len);
    resp_parser_reset(parser);

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

    g_total_cmd[fd]++;
    if (session->send_buf.size > 0) {
        set_event(fd, EPOLLOUT, 0);
    } else {
        set_event(fd, EPOLLIN, 0);
    }
    return 1;
}

static int reactor_handle_replication_fallback(int fd,
                                               conn_extension_t *ext,
                                               size_t cmd_len) {
    net_session_t *session = &ext->session;
    resp_parser_t *parser = &session->resp_parser;
    kvs_replication_apply_t apply;
    kvs_replication_fallback_kind_t kind;

    kind = kvs_replication_fallback_classify(parser->tokens,
                                             parser->arg_index,
                                             parser->token_lens,
                                             &apply);
    if (kind == KVS_REPL_FALLBACK_REPLSYNC) {
        extern distributed_config_t global_dist_config;

        if (g_cfg.sync_mode == KVS_SYNC_EBPF &&
            global_dist_config.role == ROLE_SLAVE) {
            ext->is_replication = 1;
            return reactor_finish_inline_fallback_command(fd, session, parser, cmd_len, "+OK\r\n");
        }
        return reactor_finish_inline_fallback_command(
            fd,
            session,
            parser,
            cmd_len,
            "-ERR REPLSYNC not allowed in current role\r\n"
        );
    }

    if (kind == KVS_REPL_FALLBACK_REPLICATED_APPLY && ext->is_replication) {
        if (kvs_command_executor_submit_replicated(apply.op,
                                                  apply.key,
                                                  apply.key_len,
                                                  apply.value,
                                                  apply.value_len,
                                                  apply.expire_at_ms) != 0) {
            resp_parser_reset(parser);
            return -1;
        }
        return reactor_finish_inline_fallback_command(fd, session, parser, cmd_len, "+OK\r\n");
    }

    if (kind == KVS_REPL_FALLBACK_NORMAL_COMMAND) {
        return reactor_submit_command_task(fd, ext, cmd_len, ext->is_replication ? 1 : 0);
    }

    KVS_LOG_WARN("net",
                 "reactor eBPF replication fallback rejected command without synchronous handler: fd=%d",
                 fd);
    resp_parser_reset(parser);
    return -1;
}

static int reactor_submit_async_command(int fd, conn_extension_t *ext) {
    net_session_t *session;
    size_t cmd_len = 0;
    int parse_ret;

    if (!ext) {
        return -1;
    }
    session = &ext->session;
    if (net_session_is_closing(session) || session->single_inflight) {
        return 0;
    }

    parse_ret = net_session_parse_one_resp(fd, session, &cmd_len);
    if (parse_ret <= 0) {
        return parse_ret;
    }

    if (ext->is_replication || reactor_command_is_replsync(session)) {
        return reactor_handle_replication_fallback(fd, ext, cmd_len);
    }

    return reactor_submit_command_task(fd, ext, cmd_len, 0);
}


int recv_cb(int fd) {
    if (fd < 0 || fd >= CONNECTION_SIZE) return -1;
    if (conn_list[fd].fd < 0) return -1;   // 已经被 event_unregister
    
    conn_extension_t *ext = conn_extensions[fd];
    if (!ext) return -1;
    netbuf_t* recv_buf = &ext->session.recv_buf;

    int total_recv = 0;  // 添加统计
    int recv_count = 0;   // 统计recv调用次数
    
    while (1) {
        size_t min_space = 64 * 1024;
        if(recv_buf->capacity - recv_buf->size < min_space){
            // 扩展缓冲区
            if(netbuf_reserve(recv_buf, recv_buf->size + min_space) < 0){
                KVS_LOG_ERROR("net", "failed to reserve space in recv buffer for fd %d", fd);
                event_unregister(fd);
                return -1;
            }
        }
        
        // 计算剩余空间
        size_t remaining = recv_buf->capacity - recv_buf->size;
        if (remaining == 0) {
            event_unregister(fd);
            return -1;
        }

        size_t recv_size = remaining > (256 * 1024) ? (256 * 1024) : remaining;
        
        int count = recv(fd, 
                         recv_buf->data + recv_buf->size, 
                         recv_size, 
                         0);
        if (count <= 0) {
            if (count == 0) {
                event_unregister(fd);
                return 0;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有更多数据可读
                break;
            } else {
                // 其他错误
                KVS_LOG_ERROR_ERRNO("net", "recv error for fd %d", fd);
                event_unregister(fd);
                return -1;
            }
        }
        
        recv_buf->size += count;
        total_recv += count;
        recv_count++;
        
    }

    if (recv_buf->size > 0 && !ext->session.single_inflight) {
        int processed = reactor_submit_async_command(fd, ext);
        if (processed < 0) {
            event_unregister(fd);
            return -1;
        }
    }

    //根据 send_buf 是否有数据 决定是否监听 EPOLLOUT
    netbuf_t* send_buf = &ext->session.send_buf;
    if(send_buf->size > 0){
        if (ext->session.single_inflight) {
            set_event(fd, EPOLLOUT, 0);
        } else {
            set_event(fd, EPOLLIN | EPOLLOUT, 0);
        }
    }else if(ext->session.single_inflight){
        set_event(fd, EPOLLIN, 0);
    }else{
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
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0
	servaddr.sin_port = htons(port); // 0-1023, 

	if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(struct sockaddr)) < 0) {
        KVS_LOG_ERROR_ERRNO("net", "bind failed on port %u", port);
		close(sockfd);
        return -1;
	}

	if (listen(sockfd, 1024) < 0) {
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

		conn_list[sockfd].fd = sockfd;
		conn_list[sockfd].r_action.recv_callback = accept_cb;
		
		set_event(sockfd, EPOLLIN, 1);
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
