/**
 * ws_client_v2.c - 方案一 v2: lws_cancel_service + mutex 链表队列
 *
 * 实现要点:
 *   - 每个实例拥有独立的 lws_context + service 线程(1:1 模式)
 *   - ws_internal.h 共享类型: ws_msg_node_t (柔性数组), ws_frag_buf_t, 内联工具函数
 *   - 重连基于 lws_sul 定时器, 消除 service 循环轮询
 *   - 心跳基于 lws_set_timer_usecs + LWS_CALLBACK_TIMER
 *   - 分片接收: 单帧直通回调, 多帧 ws_frag_buf_t 缓冲
 *   - WAIT_CANCELLED 在 !wsi 守卫之前处理, 修复 v1 的线程安全唤醒 bug
 *   - 停止采用 v1 风格: stopping 标志 -> cancel_service -> pthread_join -> context_destroy
 *   - conn_cb/err_cb 事件回调
 */
#include "ws_client_v2.h"
#include "ws_internal.h"
#include <libwebsockets.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ====================================================================
 * 常量
 * ==================================================================== */
#define DEFAULT_PING_INTERVAL_MS  30000
#define DEFAULT_PONG_TIMEOUT_MS   10000
#define DEFAULT_RECONNECT_INIT_MS 1000
#define DEFAULT_RECONNECT_MAX_MS  60000

/* ====================================================================
 * 内部状态枚举
 * ==================================================================== */
enum wsl_state {
    STATE_IDLE          = 0,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_RECONNECT_WAIT,
};

/* ====================================================================
 * wsl_client 结构体
 * ==================================================================== */
struct wsl_client {
    /* ---- 连接配置 (wsl_create / wsl_set_* 设置) ---- */
    char          *host;
    int            port;
    char          *path;
    char          *proto_name;
    wsl_rx_cb_t    rx_cb;
    void          *rx_user;
    wsl_conn_cb_t  conn_cb;
    void          *conn_user;
    wsl_err_cb_t   err_cb;
    void          *err_user;

    /* ---- 运行时参数 ---- */
    int            use_ssl;
    int            ssl_skip_verify;
    int            ping_interval_ms;
    int            pong_timeout_ms;
    int            reconnect_init_ms;
    int            reconnect_max_ms;
    int            max_queue_msgs;
    int            max_queue_bytes;

    /* ---- libwebsockets ---- */
    struct lws_context          *ctx;
    struct lws                  *wsi;
    struct lws_protocols         protocols[2];

    /* ---- service 线程 ---- */
    pthread_t        thread;
    volatile int     thread_started;
    volatile int     stopping;

    /* ---- 连接状态 ---- */
    int              state;
    int              reconnect_delay_ms;

    /* ---- 重连 sul 定时器 ---- */
    lws_sorted_usec_list_t      sul;

    /* ---- 心跳 ---- */
    volatile int     send_ping;
    volatile int     ping_pending;

    /* ---- 发送队列 (受 q_lock 保护) ---- */
    pthread_mutex_t  q_lock;
    ws_msg_node_t   *q_head;
    ws_msg_node_t   *q_tail;
    size_t           q_msgs;
    size_t           q_bytes;

    /* ---- 分片接收缓冲 ---- */
    ws_frag_buf_t    frag;
};

/* ====================================================================
 * 内部函数声明
 * ==================================================================== */
static void schedule_reconnect(wsl_client_t *c);
static int  do_connect(wsl_client_t *c);
static void sul_reconnect_cb(lws_sorted_usec_list_t *sul);
static int  ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len);
static void *service_thread(void *arg);
static LwsClientRet_e enqueue_msg(wsl_client_t *c, const void *data,
                                   size_t len, int is_binary);

/* ====================================================================
 * 重连 (lws_sul 定时器, 指数退避)
 * ==================================================================== */
static void schedule_reconnect(wsl_client_t *c)
{
    /* 计算延迟: 翻倍, 上限 max_ms */
    int delay = c->reconnect_delay_ms;
    if (delay == 0)
        delay = c->reconnect_init_ms;
    else {
        delay *= 2;
        if (delay > c->reconnect_max_ms)
            delay = c->reconnect_max_ms;
    }
    c->reconnect_delay_ms = delay;
    c->state = STATE_RECONNECT_WAIT;

    lws_sul_schedule(c->ctx, 0, &c->sul, sul_reconnect_cb,
                     (lws_usec_t)delay * 1000);
}

/* sul 回调: 到期触发重连 */
static void sul_reconnect_cb(lws_sorted_usec_list_t *sul)
{
    wsl_client_t *c = lws_container_of(sul, wsl_client_t, sul);
    if (c->stopping)
        return;
    do_connect(c);
}

/* ====================================================================
 * 发起连接
 * ==================================================================== */
static int do_connect(wsl_client_t *c)
{
    struct lws_client_connect_info info;
    memset(&info, 0, sizeof(info));

    c->state = STATE_CONNECTING;

    info.context   = c->ctx;
    info.address   = c->host;
    info.port      = c->port;
    info.protocol  = c->proto_name;
    info.path      = c->path ? c->path : "/";
    info.host      = c->host;
    info.origin    = c->host;
    info.method    = "GET";
    info.pwsi      = &c->wsi;

    /* SSL 配置 */
    if (c->use_ssl) {
        info.ssl_connection = LCCSCF_USE_SSL;
        if (c->ssl_skip_verify)
            info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED |
                                    LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                                    LCCSCF_ALLOW_EXPIRED;
    }

    if (!lws_client_connect_via_info(&info)) {
        c->wsi = NULL;
        c->state = STATE_RECONNECT_WAIT;
        schedule_reconnect(c);
        return 0;
    }
    return 1;
}

/* ====================================================================
 * 主 lws 回调
 * ==================================================================== */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    wsl_client_t *c;

    /*
     * LWS_CALLBACK_EVENT_WAIT_CANCELLED:
     * 必须在 !wsi 守卫之前处理, 因为 WAIT_CANCELLED 可能携带 NULL wsi.
     * (这是 v1 中线程安全唤醒被破坏的 bug 修复)
     */
    if (reason == LWS_CALLBACK_EVENT_WAIT_CANCELLED) {
        /* lws 4.x: wsi 在默认 poll 后端下非 NULL，但在 libuv/libev/libevent
         * 后端下可能为 NULL，此时 in 参数携带 lws_context 指针。 */
        struct lws_context *ctx = wsi ? lws_get_context(wsi)
                                      : (struct lws_context *)in;
        if (!ctx)
            return 0;
        c = (wsl_client_t *)lws_context_user(ctx);
        if (!c || c->stopping)
            return 0;
        /* 请求 WRITABLE, 后续在 WRITABLE 中出队列 */
        if (c->wsi)
            lws_callback_on_writable(c->wsi);
        return 0;
    }

    /* 获取客户端指针 */
    if (!wsi)
        return 0;
    c = (wsl_client_t *)lws_context_user(lws_get_context(wsi));
    if (!c)
        return 0;
    (void)user;

    switch (reason) {
    /* ---- 连接建立 ---- */
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        c->state = STATE_CONNECTED;
        c->reconnect_delay_ms = c->reconnect_init_ms;
        c->ping_pending = 0;

        /* 启动心跳定时器 (间隔 ms 转为 us) */
        if (c->ping_interval_ms > 0)
            lws_set_timer_usecs(c->wsi,
                                (lws_usec_t)c->ping_interval_ms * 1000);

        /* 连接事件回调 */
        if (c->conn_cb)
            c->conn_cb(1, c->conn_user);

        /* 队列中有积压数据, 触发可写 */
        if (c->q_head)
            lws_callback_on_writable(c->wsi);
        break;

    /* ---- 收到数据 ---- */
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        int is_final = lws_is_final_fragment(wsi);
        int is_first = lws_is_first_fragment(wsi);
        size_t remaining = lws_remaining_packet_payload(wsi);

        if (is_first && is_final && remaining == 0) {
            /* 单帧完整消息, 直接回调 */
            if (c->rx_cb)
                c->rx_cb((const char *)in, len, c->rx_user);
        } else {
            /* 多帧分片: 追加至缓冲 */
            if (ws_frag_append(&c->frag, (const unsigned char *)in, len) < 0)
                return -1;
            if (is_final) {
                /* 最后一块, 整体回调 */
                if (c->rx_cb && c->frag.len > 0)
                    c->rx_cb((const char *)c->frag.buf, c->frag.len,
                             c->rx_user);
                c->frag.len = 0;
            }
        }
        break;
    }

    /* ---- 可写: 出队发送 ---- */
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        /* 优先处理 ping */
        if (c->send_ping) {
            c->send_ping = 0;
            c->ping_pending = 1;
            lws_write(wsi, (unsigned char *)"", 0, LWS_WRITE_PING);
            if (c->q_head)
                lws_callback_on_writable(c->wsi);
            break;
        }

        /* 正常数据出队 */
        pthread_mutex_lock(&c->q_lock);

        ws_msg_node_t *n = c->q_head;
        if (!n) {
            pthread_mutex_unlock(&c->q_lock);
            break;
        }
        c->q_head = n->next;
        if (!c->q_head)
            c->q_tail = NULL;
        c->q_msgs--;
        c->q_bytes -= n->len;

        pthread_mutex_unlock(&c->q_lock);

        int write_type = n->is_binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
        int nwritten = lws_write(wsi, n->buf + LWS_PRE, n->len, write_type);
        free(n);
        if (nwritten < 0)
            return -1;

        /* 队列还有剩余数据, 继续请求可写 */
        if (c->q_head)
            lws_callback_on_writable(c->wsi);
        break;
    }

    /* ---- 心跳: TIMER 触发 ---- */
    case LWS_CALLBACK_TIMER:
        if (c->ping_pending) {
            /* 上次 ping 未收到 pong, 超时关闭连接 */
            return -1;
        }
        c->send_ping = 1;
        lws_callback_on_writable(c->wsi);
        break;

    /* ---- pong 收到 ---- */
    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        c->ping_pending = 0;
        /* 重置心跳定时器 */
        if (c->ping_interval_ms > 0 && c->wsi)
            lws_set_timer_usecs(c->wsi,
                                (lws_usec_t)c->ping_interval_ms * 1000);
        break;

    /* ---- 连接错误 ---- */
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        c->wsi = NULL;
        c->state = STATE_RECONNECT_WAIT;

        if (!c->stopping) {
            /* 错误回调 */
            if (c->err_cb)
                c->err_cb((const char *)(in ? in : "unknown error"),
                          c->err_user);
            /* 断开回调 */
            if (c->conn_cb)
                c->conn_cb(0, c->conn_user);
            schedule_reconnect(c);
        }
        break;

    /* ---- 连接关闭 ---- */
    case LWS_CALLBACK_CLIENT_CLOSED:
        c->wsi = NULL;
        c->state = STATE_RECONNECT_WAIT;

        if (!c->stopping) {
            if (c->conn_cb)
                c->conn_cb(0, c->conn_user);
            schedule_reconnect(c);
        }
        break;

    default:
        break;
    }

    return 0;
}

/* ====================================================================
 * service 线程
 * ==================================================================== */
static void *service_thread(void *arg)
{
    wsl_client_t *c = (wsl_client_t *)arg;

    /* 发起首次连接 */
    do_connect(c);

    /* service 循环 */
    while (!c->stopping) {
        lws_service(c->ctx, 50);
    }

    /* 退出前清理: 清空发送队列 */
    pthread_mutex_lock(&c->q_lock);
    ws_flush_queue(&c->q_head, &c->q_tail, &c->q_msgs, &c->q_bytes);
    pthread_mutex_unlock(&c->q_lock);

    /* 释放分片缓冲 */
    free(c->frag.buf);
    c->frag.buf = NULL;
    c->frag.len = 0;
    c->frag.cap = 0;

    return NULL;
}

/* ====================================================================
 * 入队 (线程安全)
 * ==================================================================== */
static LwsClientRet_e enqueue_msg(wsl_client_t *c, const void *data,
                                   size_t len, int is_binary)
{
    if (!c || !data)
        return LWS_ERR_PARAM;

    pthread_mutex_lock(&c->q_lock);

    /* 流控检查 */
    if (c->max_queue_msgs > 0 && (int)c->q_msgs >= c->max_queue_msgs) {
        pthread_mutex_unlock(&c->q_lock);
        return LWS_ERR_QUEUE_FULL;
    }
    if (c->max_queue_bytes > 0 &&
        (int)(c->q_bytes + len) > c->max_queue_bytes) {
        pthread_mutex_unlock(&c->q_lock);
        return LWS_ERR_QUEUE_FULL;
    }

    ws_msg_node_t *n = ws_msg_new(data, len, is_binary, LWS_PRE);
    if (!n) {
        pthread_mutex_unlock(&c->q_lock);
        return LWS_ERR_QUEUE_FULL;
    }

    /* 入队 */
    if (c->q_tail)
        c->q_tail->next = n;
    else
        c->q_head = n;
    c->q_tail = n;
    c->q_msgs++;
    c->q_bytes += len;

    pthread_mutex_unlock(&c->q_lock);

    /* 唤醒 service 线程 */
    if (c->ctx)
        lws_cancel_service(c->ctx);

    return LWS_OK;
}

/* ====================================================================
 * 公开 API
 * ==================================================================== */

/* ---- 创建 ---- */
wsl_client_t *wsl_create(const char *host, int port,
                          const char *protocol,
                          wsl_rx_cb_t rx_cb, void *user)
{
    if (!host || port <= 0)
        return NULL;

    wsl_client_t *c = (wsl_client_t *)calloc(1, sizeof(wsl_client_t));
    if (!c)
        return NULL;

    c->host       = strdup(host);
    c->port       = port;
    c->rx_cb      = rx_cb;
    c->rx_user    = user;

    if (protocol)
        c->proto_name = strdup(protocol);
    if (!c->host || (protocol && !c->proto_name)) {
        free(c->host);
        free(c->proto_name);
        free(c);
        return NULL;
    }

    /* 默认值 */
    c->path               = NULL;
    c->ping_interval_ms   = DEFAULT_PING_INTERVAL_MS;
    c->pong_timeout_ms    = DEFAULT_PONG_TIMEOUT_MS;
    c->reconnect_init_ms  = DEFAULT_RECONNECT_INIT_MS;
    c->reconnect_max_ms   = DEFAULT_RECONNECT_MAX_MS;
    c->max_queue_msgs     = 0;
    c->max_queue_bytes    = 0;
    c->use_ssl            = 0;
    c->ssl_skip_verify    = 0;

    /* 初始化锁 */
    pthread_mutex_init(&c->q_lock, NULL);

    return c;
}

/* ---- 配置 (start 前调用) ---- */
void wsl_set_ping(wsl_client_t *c, int interval_ms, int pong_timeout_ms)
{
    if (c) {
        c->ping_interval_ms = interval_ms;
        c->pong_timeout_ms  = pong_timeout_ms;
    }
}

void wsl_set_reconnect(wsl_client_t *c, int init_ms, int max_ms)
{
    if (c) {
        c->reconnect_init_ms = init_ms;
        c->reconnect_max_ms  = max_ms;
    }
}

void wsl_set_queue_limit(wsl_client_t *c, int max_msgs, int max_bytes)
{
    if (c) {
        c->max_queue_msgs  = max_msgs;
        c->max_queue_bytes = max_bytes;
    }
}

void wsl_set_ssl(wsl_client_t *c, int enabled, int skip_verify)
{
    if (c) {
        c->use_ssl        = enabled;
        c->ssl_skip_verify = skip_verify;
    }
}

void wsl_set_path(wsl_client_t *c, const char *path)
{
    if (c) {
        free(c->path);
        c->path = path ? strdup(path) : NULL;
    }
}

void wsl_set_conn_cb(wsl_client_t *c, wsl_conn_cb_t cb, void *user)
{
    if (c) {
        c->conn_cb   = cb;
        c->conn_user = user;
    }
}

void wsl_set_err_cb(wsl_client_t *c, wsl_err_cb_t cb, void *user)
{
    if (c) {
        c->err_cb   = cb;
        c->err_user = user;
    }
}

/* ---- 启动 ---- */
int wsl_start(wsl_client_t *c)
{
    if (!c)
        return 0;
    if (c->thread_started)
        return 1;

    /* 初始化 lws 协议表 (必须在 lws_create_context 之前) */
    memset(c->protocols, 0, sizeof(c->protocols));
    c->protocols[0].name  = c->proto_name ? c->proto_name : "ws";
    c->protocols[0].callback = ws_callback;
    c->protocols[0].user  = (void *)c;  /* per-session user data */
    c->protocols[0].rx_buffer_size = 0;

    /* 创建 lws 上下文 */
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port     = CONTEXT_PORT_NO_LISTEN;
    info.protocols = c->protocols;
    info.user     = (void *)c;
    info.options  = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    c->ctx = lws_create_context(&info);
    if (!c->ctx) {
        return 0;
    }

    /* 启动 service 线程 */
    c->state    = STATE_IDLE;
    c->stopping = 0;

    if (pthread_create(&c->thread, NULL, service_thread, c) != 0) {
        lws_context_destroy(c->ctx);
        c->ctx = NULL;
        return 0;
    }
    c->thread_started = 1;

    return 1;
}

/* ---- 停止 ---- */
void wsl_stop(wsl_client_t *c)
{
    if (!c || !c->thread_started)
        return;

    c->stopping = 1;

    /* 唤醒 service 线程 (可能阻塞在 lws_service) */
    if (c->ctx)
        lws_cancel_service(c->ctx);

    /* 等待线程退出 */
    pthread_join(c->thread, NULL);
    c->thread_started = 0;

    /* 销毁 lws 上下文 (会关闭所有残余 wsi) */
    if (c->ctx) {
        lws_context_destroy(c->ctx);
        c->ctx = NULL;
    }

    c->wsi   = NULL;
    c->state = STATE_IDLE;
}

/* ---- 销毁 ---- */
void wsl_destroy(wsl_client_t *c)
{
    if (!c)
        return;

    free(c->host);
    free(c->proto_name);
    free(c->path);
    free(c->frag.buf);
    pthread_mutex_destroy(&c->q_lock);
    free(c);
}

/* ---- 发送 ---- */
LwsClientRet_e wsl_send(wsl_client_t *c, const char *data, size_t len)
{
    return enqueue_msg(c, data, len, 0);
}

LwsClientRet_e wsl_send_binary(wsl_client_t *c, const void *data, size_t len)
{
    return enqueue_msg(c, data, len, 1);
}
