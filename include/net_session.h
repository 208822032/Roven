#ifndef NET_SESSION_H
#define NET_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "resp_protocol.h"
#include "netbuf.h"

/*
 * 一个客户端连接在网络层的会话状态。
 *
 * 它不直接保存 socket fd；fd 和不同网络模型自己的连接对象由 Reactor、
 * Proactor 或 NtyCo 管理。net_session_t 只保存三类通用状态：
 * 1. TCP 字节流的接收/发送缓冲区；
 * 2. RESP 解析器的中间状态；
 * 3. 异步命令完成后识别原连接所需的身份和引用计数。
 *
 * 除 refcount 外，大部分字段由连接所属的网络 owner 线程读写。不要让
 * executor 线程直接修改 recv_buf、send_buf、single_inflight 或 closing。
 */
typedef struct net_session_s {
    /* 收到但尚未完全解析的 TCP 字节。半包会保留在这里等待下次 recv。 */
    netbuf_t recv_buf;
    /* 已生成但尚未全部 send 的 RESP 响应。部分写后保留剩余字节。 */
    netbuf_t send_buf;
    /* 当前 RESP 命令的解析状态和 argv/token_lens 结果。 */
    resp_parser_t resp_parser;

    /*
     * 当前是否正在解析或提交本会话的命令。
     * 防止同一网络 owner 因重复事件重入解析流程。
     */
    int processing;
    /* 是否已完成首个非空白字节必须是 RESP 数组前缀 '*' 的协议检查。 */
    int first_packet_checked;

    /*
     * 下面是异步 command task/response task 使用的连接生命周期字段。
     * fd 可能被操作系统复用，因此响应回写时需要 session_id 和 generation
     * 一起验证，不能只凭 fd 判断“还是同一个客户端”。
     */
    /* 连接创建时分配的逻辑 ID。 */
    uint64_t session_id;
    /* 同一个连接槽位被重新使用时递增的代次。 */
    uint32_t generation;
    /* 网络 owner 已开始关闭连接时置 1，后续响应不应再写回该连接。 */
    int closing;
    /*
     * 连接对象的引用计数。网络 owner 持有初始引用；异步 task/response
     * 持有额外引用，保证连接关闭后不会过早释放内存。
     */
    int refcount;
    /* 所属 poller 的编号，用于把异步响应唤醒到正确网络线程。 */
    int owner_poller_id;
    /* 同一连接下一个请求的递增序号，用于追踪请求和响应顺序。 */
    uint64_t next_request_seq;
    /*
     * 是否已有一个命令在 executor 中等待响应。
     * 当前每连接只允许一个在途命令，以保留 Pipeline 的响应顺序。
     */
    int single_inflight;
    /* 本会话是否已计入全局连接数，free 时据此只减一次计数。 */
    int connection_counted;
} net_session_t;

/*
 * 同步协议处理回调。
 * parser 已包含一条完整 RESP 命令；回调应向 response_buf 写入 RESP 响应。
 * 返回值大于 0 表示写入的响应字节数；小于等于 0 表示失败，调用方会关闭连接。
 */
typedef int (*net_session_handler_fn)(
        resp_parser_t* parser,
        char* response_buf,
        size_t response_buf_cap,
        void* user_data);

/*
 * 初始化会话、收发缓冲区和 RESP parser，并尝试占用一个全局连接名额。
 * recv_initial/send_initial 是缓冲区初始容量；实际最大容量受统一配置限制。
 * 成功返回 0，连接数超过上限或内存申请失败返回 -1。
 */
int net_session_init(net_session_t* session,
                     size_t recv_initial,
                     size_t send_initial);

/*
 * 释放收发缓冲区和 RESP parser 资源，并归还全局连接名额。
 * 调用前必须确保引用计数已不再被异步 task/response 持有。
 */
void net_session_free(net_session_t* session);

/*
 * 设置本会话的异步身份信息，并恢复初始引用计数和 Pipeline 状态。
 * 通常在网络模型接受新连接或复用连接对象时调用。
 */
void net_session_set_identity(net_session_t *session,
                              uint64_t session_id,
                              uint32_t generation,
                              int owner_poller_id);

/* 标记连接开始关闭；网络层后续应拒绝新请求和迟到响应。 */
void net_session_mark_closing(net_session_t *session);

/* 返回连接是否正在关闭；NULL 会被当作已关闭。 */
int net_session_is_closing(const net_session_t *session);

/* 增加异步持有者的引用。该函数使用原子加法。 */
void net_session_ref_inc(net_session_t *session);

/*
 * 减少一个引用并返回剩余引用数。不会把计数减成负数。
 * 返回 0 只表示没有引用，不代表本函数会自动调用 net_session_free。
 */
int net_session_ref_dec(net_session_t *session);

/* 原子读取当前引用数；NULL 时返回 0。 */
int net_session_ref_count(const net_session_t *session);

/*
 * 生成本连接下一个递增请求序号。
 * 该字段由连接所属网络 owner 串行访问，不是多线程计数器。
 */
uint64_t net_session_next_request_seq(net_session_t *session);

/*
 * 尝试开始一个在途命令。连接关闭或已有在途命令时返回 0；成功返回 1。
 * 这不是全局锁，只能由同一连接的网络 owner 串行调用。
 */
int net_session_try_begin_inflight(net_session_t *session);

/* 一条响应已发送或丢弃后结束在途状态，允许继续提交下一条 Pipeline 命令。 */
void net_session_end_inflight(net_session_t *session);

/*
 * 同步处理 recv_buf 中尽可能多的完整 RESP 命令。
 *
 * 函数先检查请求大小和 Pipeline 命令数，再用 resp_peek_command 找出完整
 * 帧边界，随后调用有状态 resp_parser 得到 argv，最后调用 handler 并将响应
 * 追加到 send_buf。未完成的半包不会丢弃，会留在 recv_buf 中等待下次数据。
 *
 * 返回值：
 * - 大于等于 0：本轮已处理的完整命令数；0 也可能表示正在等待半包。
 * - 小于 0：请求超限、协议错误、内存不足或 handler 失败；上层应关闭连接。
 */
int net_session_process_resp(int fd,
                             net_session_t* session,
                             net_session_handler_fn handler,
                             void* user_data);

/*
 * 异步网络路径一次只解析一条完整 RESP 命令。
 *
 * 成功时 parser 会保留该命令的 tokens/token_lens，调用方必须先复制或处理
 * 参数，再从 recv_buf 消费 *cmd_len 字节，并在适当时机重置 parser。
 * 返回 1 表示成功得到一条完整命令；返回 0 表示需要更多数据或正在处理；
 * 返回小于 0 表示协议错误或请求超限，上层应关闭连接。
 */
int net_session_parse_one_resp(int fd,
                               net_session_t *session,
                               size_t *cmd_len);

#endif
