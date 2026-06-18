


#include <stdio.h>
#include <liburing.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h> 
#include <netinet/tcp.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>

#include "kvs_api.h"
#include "kvs_log.h"
#include "resp_protocol.h"
#include "distributed.h"
#include "kvs_command_executor.h"
#include "kvs_command_task.h"
#include "kvs_response_task.h"
#include "kvs_async_net_adapter.h"
#include "net_session.h" //通用会话层


#define EVENT_ACCEPT   	0
#define EVENT_READ		1
#define EVENT_WRITE		2
#define EVENT_EXECUTOR_EVENTFD 3

#define PROACTOR_CONNECTION_SIZE 1024

extern int g_shutdown_flag; // 声明全局关闭标志

typedef int (*msg_handler)(char **tokens, int count, size_t *token_lens, char *response);
static msg_handler kvs_handler;

typedef struct conn_info{
	int fd;
    struct io_uring *ring;

	net_session_t session; //通用会话层 封装recv/send缓冲区和RESP解析器
    size_t send_offset;        // 发送偏移量

    //当前是否有还在发送过程中的 READ/WRITE SQE
    int read_in_flight;
    int write_in_flight;
    int owner_released;

    int is_replication;//是否是复制连接
} conn_extension_t;

typedef struct{
    int op; // EVENT_ACCEPT / EVENT_READ / EVENT_WRITE
    conn_extension_t* ctx;// 对于 ACCEPT，ctx 只存 fd；对于 READ/WRITE，是连接上下文
} io_uring_op_t;

extern kvs_config_t g_cfg;

static conn_extension_t *proactor_connections[PROACTOR_CONNECTION_SIZE] = {0};
static uint32_t proactor_fd_generation[PROACTOR_CONNECTION_SIZE] = {0};
static uint64_t proactor_next_session_id = 1;
static int proactor_pending_conn_ops = 0;
static io_uring_op_t *proactor_executor_event_op = NULL;
static int proactor_executor_event_pending = 0;

typedef struct proactor_executor_stats_s {
    uint64_t executor_eventfd_rearm_failed;
    uint64_t executor_eventfd_cqe_count;
    uint64_t executor_eventfd_drain_count;
    uint64_t executor_responses_polled;
    uint64_t executor_dispatch_failed;
    uint64_t enable_write_failed;
    uint64_t async_submit_failed;
    uint64_t async_fallback_count;
    uint64_t pipeline_restore_submit_count;
} proactor_executor_stats_t;

static proactor_executor_stats_t proactor_executor_stats;

static int proactor_submit_async_command(struct io_uring *ring, conn_extension_t *ext);
int set_event_send(struct io_uring *ring, conn_extension_t *ctx);
static void post_recv_if_needed(struct io_uring *ring, conn_extension_t *ctx);
static void post_send_if_needed(struct io_uring *ring, conn_extension_t *ctx);

static uint64_t proactor_stats_add(uint64_t *field, uint64_t value) {
    return __atomic_add_fetch(field, value, __ATOMIC_RELAXED);
}

static uint64_t proactor_stats_load(const uint64_t *field) {
    return __atomic_load_n(field, __ATOMIC_RELAXED);
}

static int proactor_fd_valid(int fd) {
    return fd >= 0 && fd < PROACTOR_CONNECTION_SIZE;
}

static int proactor_target_is_current(conn_extension_t *ext) {
    int fd;

    if (!ext) {
        return 0;
    }
    fd = ext->fd;
    if (!proactor_fd_valid(fd)) {
        return 0;
    }
    if (proactor_connections[fd] != ext) {
        return 0;
    }
    if (net_session_is_closing(&ext->session)) {
        return 0;
    }
    if (ext->session.generation == 0 ||
        ext->session.generation != proactor_fd_generation[fd]) {
        return 0;
    }
    return 1;
}

static int proactor_can_submit_next(conn_extension_t *ext) {
    net_session_t *session;

    if (!proactor_target_is_current(ext)) {
        return 0;
    }
    session = &ext->session;
    return !session->single_inflight &&
           session->recv_buf.size > 0 &&
           session->send_buf.size == 0 &&
           ext->send_offset == 0 &&
           !ext->write_in_flight;
}

static void proactor_conn_ref_inc(conn_extension_t *ext) {
    if (!ext) {
        return;
    }
    net_session_ref_inc(&ext->session);
}

static void proactor_response_destroy(kvs_response_task_t *resp) {
    if (!resp) {
        return;
    }
    kvs_response_task_destroy(resp);
    kvs_free(resp);
}

static void proactor_conn_final_free(conn_extension_t *ext) {
    if (!ext) {
        return;
    }
    net_session_free(&ext->session);
    kvs_free(ext);
}

static void proactor_conn_release(conn_extension_t *ext) {
    if (!ext) {
        return;
    }
    if (net_session_ref_dec(&ext->session) == 0 &&
        net_session_is_closing(&ext->session)) {
        proactor_conn_final_free(ext);
    }
}

static void proactor_conn_release_owner(void *owner) {
    proactor_conn_release((conn_extension_t *)owner);
}

static void proactor_conn_remove_owner(conn_extension_t *ext) {
    int fd;

    if (!ext) {
        return;
    }
    fd = ext->fd;
    if (proactor_fd_valid(fd) && proactor_connections[fd] == ext) {
        proactor_connections[fd] = NULL;
    }
}

static void proactor_conn_mark_closing(conn_extension_t *ext) {
    int fd;

    if (!ext || net_session_is_closing(&ext->session)) {
        return;
    }

    fd = ext->fd;
    net_session_mark_closing(&ext->session);
    proactor_conn_remove_owner(ext);
    if (fd >= 0) {
        close(fd);
        ext->fd = -1;
    }
}

static void proactor_conn_request_close(conn_extension_t *ext) {
    if (!ext) {
        return;
    }
    if (ext->owner_released) {
        return;
    }
    proactor_conn_mark_closing(ext);
    ext->owner_released = 1;
    proactor_conn_release(ext);
}

static void proactor_close_all_connections(void) {
    int fd;

    for (fd = 0; fd < PROACTOR_CONNECTION_SIZE; ++fd) {
        conn_extension_t *ext = proactor_connections[fd];
        if (ext) {
            proactor_conn_request_close(ext);
        }
    }
}

static void proactor_op_release(io_uring_op_t *op) {
    if (!op) {
        return;
    }
    if ((op->op == EVENT_READ || op->op == EVENT_WRITE) && op->ctx) {
        conn_extension_t *ctx = op->ctx;
        op->ctx = NULL;
        if (proactor_pending_conn_ops > 0) {
            proactor_pending_conn_ops--;
        }
        proactor_conn_release(ctx);
    }
    if (op->op == EVENT_EXECUTOR_EVENTFD &&
        proactor_executor_event_op == op) {
        proactor_executor_event_op = NULL;
        proactor_executor_event_pending = 0;
    }
    kvs_free(op);
}

static void proactor_drain_executor_eventfd(void) {
    int event_fd = kvs_command_executor_response_event_fd();
    uint64_t value;

    if (event_fd < 0) {
        return;
    }

    for (;;) {
        ssize_t n = read(event_fd, &value, sizeof(value));
        if (n == (ssize_t)sizeof(value)) {
            proactor_stats_add(&proactor_executor_stats.executor_eventfd_drain_count, 1);
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
}

static int proactor_arm_executor_eventfd(struct io_uring *ring) {
    int event_fd = kvs_command_executor_response_event_fd();
    io_uring_op_t *op;
    struct io_uring_sqe *sqe;

    if (event_fd < 0 || g_shutdown_flag) {
        return 0;
    }
    if (proactor_executor_event_pending) {
        return 0;
    }

    op = kvs_malloc(sizeof(io_uring_op_t));
    if (!op) {
        return -1;
    }

    sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        kvs_free(op);
        return -1;
    }

    op->op = EVENT_EXECUTOR_EVENTFD;
    op->ctx = NULL;
    io_uring_prep_poll_add(sqe, event_fd, POLLIN);
    io_uring_sqe_set_data(sqe, op);

    proactor_executor_event_op = op;
    proactor_executor_event_pending = 1;
    return 0;
}

static void proactor_log_executor_stats(void) {
    kvs_command_executor_stats_t executor_stats;

    memset(&executor_stats, 0, sizeof(executor_stats));
    kvs_command_executor_get_stats(&executor_stats);

    KVS_LOG_INFO("network",
                 "proactor executor stats: submitted=%llu completed=%llu submit_failed=%llu cmd_depth=%llu cmd_max=%llu resp_depth=%llu resp_max=%llu pushed=%llu polled=%llu eventfd_wakeup=%llu eventfd_write_fail=%llu dropped=%llu mismatch_drop=%llu closing_drop=%llu append_failed=%llu",
                 (unsigned long long)executor_stats.submitted_tasks,
                 (unsigned long long)executor_stats.completed_tasks,
                 (unsigned long long)executor_stats.submit_failed,
                 (unsigned long long)executor_stats.command_queue_depth,
                 (unsigned long long)executor_stats.command_queue_max_depth,
                 (unsigned long long)executor_stats.response_queue_depth,
                 (unsigned long long)executor_stats.response_queue_max_depth,
                 (unsigned long long)executor_stats.pushed_responses,
                 (unsigned long long)executor_stats.polled_responses,
                 (unsigned long long)executor_stats.eventfd_wakeup_count,
                 (unsigned long long)executor_stats.eventfd_write_fail_count,
                 (unsigned long long)executor_stats.dropped_responses,
                 (unsigned long long)executor_stats.session_mismatch_drop,
                 (unsigned long long)executor_stats.session_closing_drop,
                 (unsigned long long)executor_stats.response_append_failed);

    KVS_LOG_INFO("network",
                 "proactor executor local stats: eventfd_cqe=%llu eventfd_drain=%llu eventfd_rearm_failed=%llu responses_polled=%llu dispatch_failed=%llu enable_write_failed=%llu async_submit_failed=%llu async_fallback=%llu pipeline_restore_submit=%llu",
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.executor_eventfd_cqe_count),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.executor_eventfd_drain_count),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.executor_eventfd_rearm_failed),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.executor_responses_polled),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.executor_dispatch_failed),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.enable_write_failed),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.async_submit_failed),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.async_fallback_count),
                 (unsigned long long)proactor_stats_load(&proactor_executor_stats.pipeline_restore_submit_count));
}

static int proactor_fail_async_command(conn_extension_t *ext,
                                       resp_parser_t *parser,
                                       kvs_response_task_t *resp,
                                       kvs_command_task_t *task,
                                       const char *reason) {
    if (task) {
        kvs_command_task_destroy(task);
        kvs_free(task);
    }
    if (resp) {
        kvs_response_task_destroy(resp);
        kvs_free(resp);
    }
    if (parser) {
        resp_parser_reset(parser);
    }
    proactor_stats_add(&proactor_executor_stats.async_fallback_count, 1);
    KVS_LOG_WARN("network",
                 "proactor async command failed fast without synchronous fallback: fd=%d reason=%s",
                 ext ? ext->fd : -1,
                 reason ? reason : "unknown");
    return -1;
}

static void proactor_cancel_executor_eventfd(struct io_uring *ring) {
    struct io_uring_sqe *sqe;
    io_uring_op_t *op = proactor_executor_event_op;

    if (!op || !proactor_executor_event_pending) {
        return;
    }

    sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        return;
    }
    io_uring_prep_cancel(sqe, op, 0);
    io_uring_sqe_set_data(sqe, NULL);
}

static void proactor_drain_pending_ops(struct io_uring *ring) {
    int attempts = 0;

    while ((proactor_pending_conn_ops > 0 || proactor_executor_event_pending) &&
           attempts < 4096) {
        struct io_uring_cqe *cqe = NULL;
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        int ret;

        io_uring_submit(ring);
        ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
        if (ret < 0 || !cqe) {
            attempts++;
            continue;
        }

        io_uring_op_t *op = (io_uring_op_t *)io_uring_cqe_get_data(cqe);
        if (op && op->ctx) {
            if (op->op == EVENT_READ) {
                op->ctx->read_in_flight = 0;
            } else if (op->op == EVENT_WRITE) {
                op->ctx->write_in_flight = 0;
            }
        }
        io_uring_cqe_seen(ring, cqe);
        proactor_op_release(op);
    }
    if (proactor_executor_event_pending && proactor_executor_event_op) {
        io_uring_op_t *op = proactor_executor_event_op;
        proactor_executor_event_op = NULL;
        proactor_executor_event_pending = 0;
        kvs_free(op);
    }
}

//工具函数
static int p_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 设置TCP_NODELAY选项减少小包延迟
static int p_set_tcp_nodelay(int fd) {
    int yes = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

// 设置TCP缓冲区大小
static int p_set_tcp_buffer_size(int fd, int recv_size, int send_size) {
    if (recv_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &recv_size, sizeof(recv_size));
    }
    if (send_size > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size));
    }
    return 0;
}


//初始化连接上下文
static int p_init_conn_extension(conn_extension_t *ext, int fd, struct io_uring *ring) {
    uint32_t generation;

    if (!proactor_fd_valid(fd)) {
        return -1;
    }
    memset(ext, 0, sizeof(conn_extension_t));
    ext->fd = fd;
    ext->ring = ring;
    ext->send_offset = 0;
    ext->is_replication = 0;
    ext->read_in_flight = 0;
    ext->write_in_flight = 0;
    ext->owner_released = 0;
    
    //初始化net_session
    if(net_session_init(&ext->session,
                        1024 * 1024,
                        1024 * 1024) < 0){
        return -1;
    }

    proactor_fd_generation[fd]++;
    if (proactor_fd_generation[fd] == 0) {
        proactor_fd_generation[fd] = 1;
    }
    generation = proactor_fd_generation[fd];
    net_session_set_identity(&ext->session,
                             proactor_next_session_id++,
                             generation,
                             0);
    proactor_connections[fd] = ext;

    //TCP优化
    p_set_nonblocking(fd);
    p_set_tcp_nodelay(fd);
    p_set_tcp_buffer_size(fd, 1024 * 1024, 1024 * 1024); // 1MB缓冲区
    
    return 0;
}

static int proactor_session_handler(resp_parser_t* parser,
                                     char* response_buf,
                                     size_t response_buf_cap,
                                     void* user_data)
{
    conn_extension_t* ext = (conn_extension_t* )user_data;

    if(!parser || parser->arg_index <= 0 || !parser->tokens[0]){
        const char* err = "-ERR invalid command\r\n";
        size_t len = strlen(err);
        if(len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    char* cmd_name = parser->tokens[0];

    // 单独处理 REPLSYNC
    if (g_cfg.sync_mode == KVS_SYNC_EBPF && strcasecmp(cmd_name, "REPLSYNC") == 0) {
        extern distributed_config_t global_dist_config;

        const char *ok_resp  = "+OK\r\n";
        const char *err_resp = "-ERR REPLSYNC not allowed in current role\r\n";

        if (global_dist_config.role == ROLE_SLAVE) {
            ext->is_replication = 1;
            KVS_LOG_WARN("network",
                         "proactor eBPF REPLSYNC uses legacy synchronous replication path; rwlock removal is not supported for this path");

            size_t ok_len = strlen(ok_resp);
            if (ok_len > response_buf_cap) ok_len = response_buf_cap;
            memcpy(response_buf, ok_resp, ok_len);
            return (int)ok_len;
        } else {
            size_t err_len = strlen(err_resp);
            if (err_len > response_buf_cap) err_len = response_buf_cap;
            memcpy(response_buf, err_resp, err_len);
            return (int)err_len;
        }
    }

    int old_ctx = get_sync_context();
    if (ext->is_replication) {
        set_sync_context(2);
    } else {
        set_sync_context(0);
    }

    int resp_len = kvs_handler(parser->tokens,
                               parser->arg_index,
                               parser->token_lens,
                               response_buf);

    set_sync_context(old_ctx);

    if (resp_len < 0 || (size_t)resp_len > response_buf_cap) {
        const char *err = "-ERR internal handler error\r\n";
        size_t len = strlen(err);
        if (len > response_buf_cap) len = response_buf_cap;
        memcpy(response_buf, err, len);
        return (int)len;
    }

    return resp_len;

}

// === net_session 封装 ===

static int p_process_received_data(conn_extension_t *ext) {
    if (!proactor_target_is_current(ext)) {
        return -1;
    }
    return net_session_process_resp(ext->fd,
                                    &ext->session,
                                    proactor_session_handler,
                                    ext);
}

static int proactor_command_is_replsync(net_session_t *session) {
    resp_parser_t *parser;

    if (!session) {
        return 0;
    }
    parser = &session->resp_parser;
    return parser->arg_index > 0 &&
           parser->tokens[0] &&
           strcasecmp(parser->tokens[0], "REPLSYNC") == 0;
}

static int proactor_async_validate_owner(
    const kvs_async_net_response_target_t *target,
    kvs_async_net_drop_reason_t *reason
) {
    conn_extension_t *ext;
    net_session_t *session;

    if (reason) {
        *reason = KVS_ASYNC_NET_DROP_SESSION_MISMATCH;
    }

    if (!target || target->owner_type != KVS_ASYNC_NET_OWNER_PROACTOR) {
        return 0;
    }
    ext = (conn_extension_t *)target->owner;
    if (!proactor_target_is_current(ext)) {
        return 0;
    }
    if (ext->fd != target->fd) {
        return 0;
    }

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

static int proactor_async_append_response(const kvs_async_net_response_target_t *target) {
    conn_extension_t *ext;
    net_session_t *session;
    size_t len;

    if (!target || target->owner_type != KVS_ASYNC_NET_OWNER_PROACTOR) {
        return -1;
    }
    ext = (conn_extension_t *)target->owner;
    if (!proactor_target_is_current(ext)) {
        return -1;
    }

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

static void proactor_async_drop_response(
    const kvs_async_net_response_target_t *target,
    kvs_async_net_drop_reason_t reason
) {
    int executor_reason = KVS_COMMAND_EXECUTOR_DROP_SESSION_MISMATCH;

    if (reason == KVS_ASYNC_NET_DROP_SESSION_CLOSING) {
        executor_reason = KVS_COMMAND_EXECUTOR_DROP_SESSION_CLOSING;
    } else if (reason == KVS_ASYNC_NET_DROP_APPEND_FAILED) {
        executor_reason = KVS_COMMAND_EXECUTOR_DROP_APPEND_FAILED;
        if (target && target->owner_type == KVS_ASYNC_NET_OWNER_PROACTOR) {
            conn_extension_t *ext = (conn_extension_t *)target->owner;
            if (proactor_target_is_current(ext)) {
                proactor_conn_request_close(ext);
            }
        }
    }

    kvs_command_executor_record_response_drop(executor_reason);
}

static void proactor_async_release_session(kvs_async_net_response_target_t *target) {
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

static int proactor_async_enable_write(const kvs_async_net_response_target_t *target) {
    conn_extension_t *ext;
    int ret;

    if (!target || target->owner_type != KVS_ASYNC_NET_OWNER_PROACTOR) {
        return -1;
    }
    ext = (conn_extension_t *)target->owner;
    if (!proactor_target_is_current(ext)) {
        return -1;
    }
    ret = set_event_send(ext->ring, ext);
    if (ret < 0) {
        proactor_stats_add(&proactor_executor_stats.enable_write_failed, 1);
        KVS_LOG_ERROR("network",
                      "proactor executor enable_write failed: fd=%d session_id=%llu generation=%u request_seq=%llu send_size=%zu send_offset=%zu write_in_flight=%d",
                      ext->fd,
                      (unsigned long long)ext->session.session_id,
                      ext->session.generation,
                      (unsigned long long)target->request_seq,
                      ext->session.send_buf.size,
                      ext->send_offset,
                      ext->write_in_flight);
    }
    return ret;
}

static int proactor_async_restore_read(const kvs_async_net_response_target_t *target) {
    conn_extension_t *ext;

    if (!target || target->owner_type != KVS_ASYNC_NET_OWNER_PROACTOR) {
        return -1;
    }
    ext = (conn_extension_t *)target->owner;
    if (!proactor_target_is_current(ext)) {
        return -1;
    }
    if (proactor_can_submit_next(ext)) {
        int processed = proactor_submit_async_command(ext->ring, ext);
        if (processed < 0) {
            proactor_conn_request_close(ext);
            return -1;
        }
        if (processed > 0) {
            proactor_stats_add(&proactor_executor_stats.pipeline_restore_submit_count, 1);
        }
    }
    return 0;
}

static int proactor_async_wake_owner(int owner_poller_id) {
    (void)owner_poller_id;
    return 0;
}

static const kvs_async_net_ops_t g_proactor_async_net_ops = {
    .validate_owner = proactor_async_validate_owner,
    .append_response = proactor_async_append_response,
    .drop_response = proactor_async_drop_response,
    .release_session = proactor_async_release_session,
    .restore_read = proactor_async_restore_read,
    .enable_write = proactor_async_enable_write,
    .wake_owner = proactor_async_wake_owner,
};

static void proactor_handle_executor_response(kvs_response_task_t *resp) {
    kvs_async_net_response_target_t target;
    int dispatch_ret;

    if (!resp) {
        return;
    }
    kvs_async_net_response_target_from_task(&target, resp);
    dispatch_ret = kvs_async_net_dispatch_response(&g_proactor_async_net_ops, &target);
    if (dispatch_ret == KVS_ASYNC_NET_DISPATCH_APPEND_FAILED) {
        proactor_stats_add(&proactor_executor_stats.executor_dispatch_failed, 1);
    }
    proactor_response_destroy(resp);
}

static void proactor_process_executor_responses(struct io_uring *ring) {
    for (;;) {
        kvs_response_task_t *resp = NULL;
        int ret = kvs_command_executor_poll_response_for_owner(
            KVS_ASYNC_NET_OWNER_PROACTOR,
            0,
            &resp
        );
        if (ret <= 0 || !resp) {
            break;
        }
        proactor_stats_add(&proactor_executor_stats.executor_responses_polled, 1);
        proactor_handle_executor_response(resp);
    }

    for (int fd = 0; fd < PROACTOR_CONNECTION_SIZE; ++fd) {
        conn_extension_t *ext = proactor_connections[fd];
        if (!proactor_target_is_current(ext)) {
            continue;
        }
        if (proactor_can_submit_next(ext)) {
            if (proactor_submit_async_command(ring, ext) < 0) {
                proactor_conn_request_close(ext);
            }
        }
        post_send_if_needed(ring, ext);
        post_recv_if_needed(ring, ext);
    }
}

static int proactor_submit_async_command(struct io_uring *ring, conn_extension_t *ext) {
    net_session_t *session;
    resp_parser_t *parser;
    kvs_response_task_t *resp = NULL;
    kvs_command_task_t *task = NULL;
    size_t cmd_len = 0;
    uint64_t request_seq;
    int parse_ret;
    int build_ret;

    if (!proactor_target_is_current(ext)) {
        return -1;
    }
    session = &ext->session;
    if (net_session_is_closing(session) || session->single_inflight) {
        return 0;
    }

    parse_ret = net_session_parse_one_resp(ext->fd, session, &cmd_len);
    if (parse_ret <= 0) {
        return parse_ret;
    }

    if (ext->is_replication || proactor_command_is_replsync(session)) {
        int processed = p_process_received_data(ext);
        return processed < 0 ? -1 : processed;
    }

    parser = &session->resp_parser;
    resp = (kvs_response_task_t *)kvs_malloc(sizeof(*resp));
    task = (kvs_command_task_t *)kvs_malloc(sizeof(*task));
    if (resp) {
        memset(resp, 0, sizeof(*resp));
    }
    if (task) {
        memset(task, 0, sizeof(*task));
    }
    if (!resp || !task) {
        return proactor_fail_async_command(ext, parser, resp, task, "alloc");
    }

    if (kvs_response_task_init_owned(resp, KVS_MAX_RESPONSE_LENGTH) < 0) {
        return proactor_fail_async_command(ext, parser, resp, task, "response_init");
    }

    build_ret = kvs_command_task_build(task,
                                       parser->tokens,
                                       parser->arg_index,
                                       parser->token_lens,
                                       resp);
    if (build_ret < 0 && task->status <= 0) {
        return proactor_fail_async_command(ext, parser, resp, task, "task_build");
    }
    if (kvs_command_task_copy_argv(task) < 0) {
        return proactor_fail_async_command(ext, parser, resp, task, "copy_argv");
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

    proactor_conn_ref_inc(ext);
    task->session = session;
    task->session_owner = ext;
    task->session_release = proactor_conn_release_owner;
    task->fd = ext->fd;
    task->session_id = session->session_id;
    task->generation = session->generation;
    task->request_seq = request_seq;
    task->owner_type = KVS_ASYNC_NET_OWNER_PROACTOR;
    task->owner_poller_id = session->owner_poller_id;
    task->sync_context = 0;
    task->is_replication = 0;

    if (kvs_command_executor_submit(task) < 0) {
        net_session_end_inflight(session);
        proactor_conn_release(ext);
        task->session = NULL;
        task->session_owner = NULL;
        task->session_release = NULL;
        kvs_command_task_destroy(task);
        kvs_response_task_destroy(resp);
        kvs_free(resp);
        kvs_free(task);
        proactor_stats_add(&proactor_executor_stats.async_submit_failed, 1);
        proactor_stats_add(&proactor_executor_stats.async_fallback_count, 1);
        KVS_LOG_WARN("network",
                     "proactor executor submit failed fast without synchronous fallback: fd=%d",
                     ext->fd);
        return -1;
    }

    netbuf_consume_front(&session->recv_buf, cmd_len);
    resp_parser_reset(parser);

    (void)ring;
    return 1;
}


void proactor_wait(void) {
    // 简单实现：等待1秒或直到关闭标志被设置
    struct timespec req = {0, 10000000}; // 10ms
    while (!g_shutdown_flag) {
        nanosleep(&req, NULL);
    }
}

#define ENTRIES_LENGTH		1024

int set_event_recv(struct io_uring *ring, conn_extension_t *ctx) {
    io_uring_op_t* op;
    struct io_uring_sqe *sqe;
    netbuf_t* recv_buf;

    if(!proactor_target_is_current(ctx)) return -1;
    if(ctx->read_in_flight) return 0; // 已经有一个 READ 在飞

    recv_buf = &ctx->session.recv_buf;

	// 确保有足够空间接收数据
    size_t min_space = 4096;
    if (recv_buf->size + 4096 > recv_buf->capacity) {
        if(netbuf_reserve(recv_buf, recv_buf->size + min_space) < 0){
            return -1;
        }
    }

    op = kvs_malloc(sizeof(io_uring_op_t));
    if(!op) return -1;

	sqe = io_uring_get_sqe(ring);
	if (!sqe) {
        kvs_free(op);
        return -1;
    }

    op->op  = EVENT_READ;
    op->ctx = ctx;
    proactor_conn_ref_inc(ctx);
    proactor_pending_conn_ops++;
	
	io_uring_prep_recv(sqe, ctx->fd, 
                      recv_buf->data + recv_buf->size, 
                      recv_buf->capacity - recv_buf->size - 1, 0);

    io_uring_sqe_set_data(sqe, op);

    ctx->read_in_flight = 1;

    return 0;
}


int set_event_send(struct io_uring *ring, conn_extension_t *ctx) {
    io_uring_op_t *op;
    struct io_uring_sqe *sqe;
    netbuf_t* send_buf;
    size_t remaining;

    if (!proactor_target_is_current(ctx)) return -1;
    if (ctx->write_in_flight) return 0; // 已经有一个 WRITE 在飞

    send_buf = &ctx->session.send_buf;

    if (ctx->send_offset >= send_buf->size) return 0; // 无数据可发

    op = kvs_malloc(sizeof(io_uring_op_t));
    if (!op) return -1;

	sqe = io_uring_get_sqe(ring);
	if(!sqe) {
        kvs_free(op);
        return -1;
    }

	remaining = send_buf->size - ctx->send_offset;

    op->op  = EVENT_WRITE;
    op->ctx = ctx;
    proactor_conn_ref_inc(ctx);
    proactor_pending_conn_ops++;

    io_uring_prep_send(sqe,
                       ctx->fd,
                       send_buf->data + ctx->send_offset,
                       remaining,
                       0);// io_uring_sqe_set_data(sqe, ctx);
    io_uring_sqe_set_data(sqe, op);
	
    ctx->write_in_flight = 1;
	return 0;
}

int set_event_accept(struct io_uring *ring, int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    io_uring_op_t* op;
    struct io_uring_sqe *sqe;

    op = kvs_malloc(sizeof(io_uring_op_t));
    if(!op) return -1;

	sqe = io_uring_get_sqe(ring);
	if(!sqe) {
        kvs_free(op);
        return -1;
    }

    op->op  = EVENT_ACCEPT;
    op->ctx = NULL; // 对于 ACCEPT，ctx 暂时不用

    io_uring_prep_accept(sqe, sockfd, (struct sockaddr*)addr, addrlen, flags);
    io_uring_sqe_set_data(sqe, op);
	return 0;

}

static void post_recv_if_needed(struct io_uring *ring, conn_extension_t *ctx) {
    // 连接还活着且当前没有 READ，就再投一个
    if (proactor_target_is_current(ctx) &&
        !ctx->read_in_flight &&
        !ctx->session.single_inflight &&
        ctx->session.send_buf.size == 0 &&
        ctx->send_offset == 0 &&
        !ctx->write_in_flight) {
        set_event_recv(ring, ctx);
    }
}

static void post_send_if_needed(struct io_uring *ring, conn_extension_t *ctx) {
    if (!proactor_target_is_current(ctx)) {
        return;
    }
    netbuf_t* send_buf = &ctx->session.send_buf;
    if (ctx->send_offset < send_buf->size && !ctx->write_in_flight) {
        set_event_send(ring, ctx);
    }
}

//服务器监听

int p_init_server(unsigned short port) {	

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);	
	if (sockfd < 0) {
    KVS_LOG_ERROR_ERRNO("network", "socket failed");
        return -1;
    }

	// 设置SO_REUSEADDR和SO_REUSEPORT选项
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        KVS_LOG_ERROR_ERRNO("network", "setsockopt SO_REUSEADDR failed");
        close(sockfd);
        return -1;
    }
    
    #ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        KVS_LOG_ERROR_ERRNO("network", "setsockopt SO_REUSEPORT failed");
        close(sockfd);
        return -1;
    }
    #endif

	struct sockaddr_in serveraddr;	
	memset(&serveraddr, 0, sizeof(struct sockaddr_in));	
	serveraddr.sin_family = AF_INET;	
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);	
	serveraddr.sin_port = htons(port);	

	if (-1 == bind(sockfd, (struct sockaddr*)&serveraddr, sizeof(struct sockaddr))) {		
        KVS_LOG_ERROR_ERRNO("network", "bind failed on port %u", port);
        close(sockfd);
		return -1;	
	}	

	// 设置监听socket为非阻塞
    p_set_nonblocking(sockfd);

	if(listen(sockfd, 10)){
        KVS_LOG_ERROR_ERRNO("network", "listen failed on port %u", port);
        close(sockfd);
        return -1;
    }
	
	return sockfd;
}

int proactor_start(unsigned short port, msg_handler handler) {

	int sockfd = p_init_server(port);
	if (sockfd < 0) {
        return -1;
    }
	kvs_handler = handler;

	struct io_uring_params params;
	memset(&params, 0, sizeof(params));
    struct io_uring ring;
	if (io_uring_queue_init_params(ENTRIES_LENGTH, &ring, &params) < 0) {
    KVS_LOG_ERROR_ERRNO("network", "io_uring_queue_init_params failed");
        close(sockfd);
        return -1;
    }

	struct sockaddr_in clientaddr;	
	socklen_t len = sizeof(clientaddr);
	set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);
    if (proactor_arm_executor_eventfd(&ring) < 0) {
        KVS_LOG_ERROR("network", "failed to arm executor eventfd poll in proactor");
        close(sockfd);
        io_uring_queue_exit(&ring);
        return -1;
    }
	
	// int nready;
	while (!g_shutdown_flag) {

		if (io_uring_submit(&ring) < 0) {
            KVS_LOG_ERROR_ERRNO("network", "io_uring_submit failed");
            break;
        }

		struct io_uring_cqe *cqe;
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms超时
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
		if (ret < 0 || !cqe) continue;  // 超时或错误

		// conn_extension_t *ctx = (conn_extension_t *)io_uring_cqe_get_data(cqe);
        io_uring_op_t *op = (io_uring_op_t *)io_uring_cqe_get_data(cqe);

		if (g_shutdown_flag) {
			io_uring_cqe_seen(&ring, cqe);
            proactor_op_release(op);
			break;
		}

        if (!op) {
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

    if (op->op == EVENT_ACCEPT) {
            int connfd = cqe->res;
            if (connfd > 0) {
                conn_extension_t *new_ctx = kvs_malloc(sizeof(conn_extension_t));
                if (new_ctx && p_init_conn_extension(new_ctx, connfd, &ring) == 0) {
                    // 新连接：立即投一个 READ
                    if (set_event_recv(&ring, new_ctx) < 0) {
                        proactor_conn_request_close(new_ctx);
                    }
                } else {
                    if (new_ctx) kvs_free(new_ctx);
                    close(connfd);
                }
            }
            // 继续监听下一个连接
            set_event_accept(&ring, sockfd, (struct sockaddr*)&clientaddr, &len, 0);

        } else if (op->op == EVENT_READ) {
            conn_extension_t *ctx = op->ctx;
            int n = cqe->res;

            if (!ctx) {
                /* nothing to do */
            } else if (!proactor_target_is_current(ctx)) {
                ctx->read_in_flight = 0;
            } else if (n <= 0) {
                ctx->read_in_flight = 0;
                // client 关闭或错误
                proactor_conn_request_close(ctx);
            } else {
                ctx->read_in_flight = 0;
                netbuf_t* recv_buf = &ctx->session.recv_buf;
                recv_buf->size += n;

                int cmd_cnt = 0;
                if (proactor_can_submit_next(ctx)) {
                    cmd_cnt = proactor_submit_async_command(&ring, ctx);
                }
                if (cmd_cnt < 0) {
                    proactor_conn_request_close(ctx);
                } else {
                    // 始终保持至少一个 READ 在队列中
                    post_recv_if_needed(&ring, ctx);
                    // 若有数据要发，附带一个 WRITE
                    post_send_if_needed(&ring, ctx);
                }
            }

        } else if (op->op == EVENT_WRITE) {

            conn_extension_t *ctx = op->ctx;
            int n = cqe->res;

            if (!ctx) {
                /* nothing to do */
            } else if (!proactor_target_is_current(ctx)) {
                ctx->write_in_flight = 0;
            } else if (n <= 0) {
                ctx->write_in_flight = 0;
                proactor_conn_request_close(ctx);
            } else {
                ctx->write_in_flight = 0;
                ctx->send_offset += n;

                netbuf_t* send_buf = &ctx->session.send_buf;
                if (ctx->send_offset >= send_buf->size) {
                    // 全部发送完毕，重置发送缓冲
                    ctx->send_offset   = 0;
                    send_buf->size = 0;
                    if (proactor_can_submit_next(ctx)) {
                        int processed = proactor_submit_async_command(&ring, ctx);
                        if (processed < 0) {
                            proactor_conn_request_close(ctx);
                        }
                    }
                }
                // 如果还有未发送的内容，再投一个 WRITE
                post_send_if_needed(&ring, ctx);
                // 同时确保 READ 也一直在排队
                post_recv_if_needed(&ring, ctx);
            }
        } else if (op->op == EVENT_EXECUTOR_EVENTFD) {
            proactor_stats_add(&proactor_executor_stats.executor_eventfd_cqe_count, 1);
            if (proactor_executor_event_op == op) {
                proactor_executor_event_op = NULL;
                proactor_executor_event_pending = 0;
            }
            if (cqe->res >= 0) {
                proactor_drain_executor_eventfd();
                proactor_process_executor_responses(&ring);
            } else {
                KVS_LOG_ERROR("network",
                              "proactor executor eventfd poll CQE failed: res=%d",
                              cqe->res);
            }
            if (!g_shutdown_flag) {
                if (proactor_arm_executor_eventfd(&ring) < 0) {
                    proactor_stats_add(&proactor_executor_stats.executor_eventfd_rearm_failed, 1);
                    KVS_LOG_ERROR("network",
                                  "failed to rearm executor eventfd poll in proactor; executor responses may stop waking owner loop");
                }
            }
        }

        io_uring_cqe_seen(&ring, cqe);
        proactor_op_release(op);
    }


    proactor_close_all_connections();
    proactor_drain_executor_eventfd();
    proactor_process_executor_responses(&ring);
    proactor_cancel_executor_eventfd(&ring);
    proactor_drain_pending_ops(&ring);
	close(sockfd);
    io_uring_queue_exit(&ring);
    proactor_log_executor_stats();
    KVS_LOG_INFO("network", "Proactor network exited");

	return 0;

}


