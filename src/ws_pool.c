/**
 * ws_pool.c - 方案二: 共享 lws_context 的 1:N 连接池
 *
 * 线程安全约定:
 *   - pool->pool_lock  保护 clients[], n_clients, pending[], n_pending
 *   - client->q_lock   保护每个客户端的发送队列
 *   - client->stop_mu  保护 wsi / stopping / stopped + stop_cond
 *                       (wsi 同时被 service 线程和 stop 线程读写)
 *   - state / sul 仅在 service 线程内访问, 无需额外锁
 */

#include "ws_pool.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- 编译期常量 ---- */
#define DEFAULT_PING_INTERVAL_MS   30000
#define DEFAULT_PONG_TIMEOUT_MS    10000
#define DEFAULT_RECONNECT_INIT_MS  1000
#define DEFAULT_RECONNECT_MAX_MS   60000
#define FRAG_INIT_CAP              4096
#define SERVICE_POLL_MS            50

/* ---- 内部类型 ---- */

typedef struct msg_node {
    unsigned char   *buf;       /* LWS_PRE + payload 连续分配 */
    size_t           len;
    int              is_binary;
    struct msg_node *next;
} msg_node_t;

typedef enum {
    STATE_IDLE,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_RECONNECT_WAIT,
    STATE_STOPPED,
} client_state_t;

struct wsp_client {
    /* 配置 (wsp_pool_add 前只读) */
    char        *host;
    int          port;
    char        *proto_name;
    char        *path;
    wsl_rx_cb_t  rx_cb;
    void        *user;

    int          use_ssl;
    int          ssl_skip_verify;

    int          ping_interval_ms;
    int          pong_timeout_ms;
    int          reconnect_init_ms;
    int          reconnect_max_ms;
    int          reconnect_delay_ms;

    size_t       max_queue_msgs;
    size_t       max_queue_bytes;

    /* LWS (service 线程内访问) */
    struct lws          *wsi;
    client_state_t       state;
    int                  send_ping;
    int                  ping_pending;

    /* lws sul 定时器, 用于重连退避 */
    lws_sorted_usec_list_t sul;

    /* 发送队列 */
    pthread_mutex_t  q_lock;
    msg_node_t      *q_head;
    msg_node_t      *q_tail;
    size_t           q_msgs;
    size_t           q_bytes;

    /* 分片缓冲 */
    unsigned char   *frag_buf;
    size_t           frag_len;
    size_t           frag_cap;

    /* 停止同步 */
    pthread_mutex_t  stop_mu;
    pthread_cond_t   stop_cond;
    int              stopping;
    int              stopped;   /* 1 = CLOSED 已回调, wsp_client_stop 可以返回 */

    /* 反向指针, service 线程用 */
    struct wsp_pool *pool;
};

struct wsp_pool {
    /* 已注册客户端 (pool_lock 保护) */
    pthread_mutex_t  pool_lock;
    wsp_client_t    *clients[WSP_MAX_CLIENTS];
    int              n_clients;

    /* 待加入客户端 (pool_lock 保护, 由 service 线程消费) */
    wsp_client_t    *pending[WSP_MAX_CLIENTS];
    int              n_pending;

    /* LWS */
    struct lws_context   *ctx;
    struct lws_protocols  protocols[2];

    /* Service 线程 */
    pthread_t  thread;
    int        thread_started;
    int        destroying;
};

/* ======================================================================
 * 内部工具
 * ====================================================================== */

static int frag_append(wsp_client_t *c, const unsigned char *data, size_t len)
{
    if (c->frag_len + len > c->frag_cap) {
        size_t cap = c->frag_cap ? c->frag_cap : FRAG_INIT_CAP;
        while (cap < c->frag_len + len)
            cap *= 2;
        unsigned char *p = realloc(c->frag_buf, cap);
        if (!p) return -1;
        c->frag_buf = p;
        c->frag_cap = cap;
    }
    memcpy(c->frag_buf + c->frag_len, data, len);
    c->frag_len += len;
    return 0;
}

static void flush_queue_locked(wsp_client_t *c)
{
    msg_node_t *n = c->q_head;
    while (n) {
        msg_node_t *next = n->next;
        free(n->buf);
        free(n);
        n = next;
    }
    c->q_head = c->q_tail = NULL;
    c->q_msgs = c->q_bytes = 0;
}

/* ======================================================================
 * lws sul 定时器回调 (前向声明, 供 schedule_reconnect 使用)
 * ====================================================================== */
static void sul_reconnect_cb(lws_sorted_usec_list_t *sul);

/* ======================================================================
 * 重连退避调度 (service 线程内调用)
 * ====================================================================== */

static void schedule_reconnect(wsp_client_t *c)
{
    if (c->stopping) return;
    lws_sul_schedule(c->pool->ctx, 0, &c->sul, sul_reconnect_cb,
                     (lws_usec_t)c->reconnect_delay_ms * 1000);
    c->reconnect_delay_ms *= 2;
    if (c->reconnect_delay_ms > c->reconnect_max_ms)
        c->reconnect_delay_ms = c->reconnect_max_ms;
    c->state = STATE_RECONNECT_WAIT;
}

/* ======================================================================
 * 发起连接 (service 线程内调用)
 * ====================================================================== */

static void do_connect(wsp_client_t *c)
{
    struct lws_client_connect_info info;
    memset(&info, 0, sizeof(info));
    info.context  = c->pool->ctx;
    info.address  = c->host;
    info.port     = c->port;
    info.path     = (c->path && c->path[0]) ? c->path : "/";
    info.host     = c->host;
    info.origin   = c->host;
    info.protocol = (c->proto_name && c->proto_name[0]) ? c->proto_name : NULL;
    info.userdata = c;   /* lws_wsi_user(wsi) == c */

    if (c->use_ssl) {
        info.ssl_connection = LCCSCF_USE_SSL;
        if (c->ssl_skip_verify)
            info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED
                                 | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK
                                 | LCCSCF_ALLOW_EXPIRED;
    }

    c->state = STATE_CONNECTING;
    c->wsi   = lws_client_connect_via_info(&info);
    if (!c->wsi)
        schedule_reconnect(c);
}

/* ======================================================================
 * lws sul 定时器回调 (重连触发, service 线程内)
 * ====================================================================== */

static void sul_reconnect_cb(lws_sorted_usec_list_t *sul)
{
    wsp_client_t *c = lws_container_of(sul, wsp_client_t, sul);
    if (!c->stopping)
        do_connect(c);
}

/* ======================================================================
 * lws 回调
 * ====================================================================== */

static int ws_pool_callback(struct lws *wsi, enum lws_callback_reasons reason,
                             void *user_data, void *in, size_t len)
{
    if (!wsi) return 0;

    /* per-connection 指针通过 lws_wsi_user(wsi) 获取 */
    wsp_client_t *c = (wsp_client_t *)lws_wsi_user(wsi);

    switch (reason) {

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        c->state = STATE_CONNECTED;
        c->reconnect_delay_ms = c->reconnect_init_ms;
        c->send_ping  = 0;
        c->ping_pending = 0;
        lws_set_timer_usecs(wsi, (lws_usec_t)c->ping_interval_ms * 1000);
        break;

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        int first     = lws_is_first_fragment(wsi);
        int final     = lws_is_final_fragment(wsi);
        size_t remain = lws_remaining_packet_payload(wsi);

        if (first && final && remain == 0) {
            if (c->rx_cb)
                c->rx_cb((const char *)in, len, c->user);
        } else {
            if (first) c->frag_len = 0;
            if (frag_append(c, (const unsigned char *)in, len) < 0)
                return -1;
            if (final && remain == 0) {
                if (c->rx_cb)
                    c->rx_cb((const char *)c->frag_buf, c->frag_len, c->user);
                c->frag_len = 0;
            }
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (c->send_ping) {
            unsigned char ping_buf[LWS_PRE + 4] = {0};
            lws_write(wsi, ping_buf + LWS_PRE, 0, LWS_WRITE_PING);
            c->send_ping    = 0;
            c->ping_pending = 1;

            pthread_mutex_lock(&c->q_lock);
            int has_data = (c->q_head != NULL);
            pthread_mutex_unlock(&c->q_lock);
            if (has_data)
                lws_callback_on_writable(wsi);
            break;
        }

        pthread_mutex_lock(&c->q_lock);
        msg_node_t *node = c->q_head;
        if (node) {
            c->q_head = node->next;
            if (!c->q_head) c->q_tail = NULL;
            c->q_msgs--;
            c->q_bytes -= node->len;
        }
        int more = (c->q_head != NULL);
        pthread_mutex_unlock(&c->q_lock);

        if (node) {
            int n = lws_write(wsi, node->buf + LWS_PRE, node->len,
                              node->is_binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
            free(node->buf);
            free(node);
            if (n < 0)
                return -1;
            if (more)
                lws_callback_on_writable(wsi);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        /*
         * 任意线程调用 lws_cancel_service() 后触发.
         * 1. 从 pool 的 pending 队列消费新客户端并发起连接
         * 2. 对所有已连接、有待发数据的客户端请求 WRITABLE
         */
        wsp_pool_t *pool = (wsp_pool_t *)lws_context_user(lws_get_context(wsi));
        if (!pool) break;

        /* 消费 pending 队列 */
        pthread_mutex_lock(&pool->pool_lock);
        int np = pool->n_pending;
        wsp_client_t *pend[WSP_MAX_CLIENTS];
        memcpy(pend, pool->pending, np * sizeof(wsp_client_t *));
        pool->n_pending = 0;
        pthread_mutex_unlock(&pool->pool_lock);

        for (int i = 0; i < np; i++) {
            wsp_client_t *pc = pend[i];
            pc->state = STATE_IDLE;
            do_connect(pc);
        }

        /* 请求有数据的已连接客户端写 */
        pthread_mutex_lock(&pool->pool_lock);
        int nc = pool->n_clients;
        wsp_client_t *snap[WSP_MAX_CLIENTS];
        memcpy(snap, pool->clients, nc * sizeof(wsp_client_t *));
        pthread_mutex_unlock(&pool->pool_lock);

        for (int i = 0; i < nc; i++) {
            wsp_client_t *cc = snap[i];
            if (cc->stopping && cc->wsi) {
                /* 发起关闭, CLOSED 中 signal stop_cond */
                lws_close_reason(cc->wsi, LWS_CLOSE_STATUS_NORMAL,
                                 NULL, 0);
                lws_callback_on_writable(cc->wsi);
                continue;
            }
            if (cc->state != STATE_CONNECTED || !cc->wsi) continue;
            pthread_mutex_lock(&cc->q_lock);
            int has = (cc->q_head != NULL);
            pthread_mutex_unlock(&cc->q_lock);
            if (has)
                lws_callback_on_writable(cc->wsi);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_TIMER:
        if (!c) break;
        if (c->ping_pending) {
            fprintf(stderr, "[wsp] pong timeout on %s:%d, closing\n",
                    c->host, c->port);
            return -1;
        }
        c->send_ping = 1;
        lws_callback_on_writable(wsi);
        lws_set_timer_usecs(wsi, (lws_usec_t)c->pong_timeout_ms * 1000);
        break;

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        if (!c) break;
        c->ping_pending = 0;
        lws_set_timer_usecs(wsi, (lws_usec_t)c->ping_interval_ms * 1000);
        break;

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if (!c) break;
        fprintf(stderr, "[wsp] connection error %s:%d: %s\n",
                c->host, c->port, in ? (const char *)in : "(none)");
        pthread_mutex_lock(&c->stop_mu);
        c->wsi = NULL;
        if (c->stopping) {
            c->stopped = 1;
            pthread_cond_signal(&c->stop_cond);
            pthread_mutex_unlock(&c->stop_mu);
        } else {
            pthread_mutex_unlock(&c->stop_mu);
            schedule_reconnect(c);
        }
        break;

    /* ------------------------------------------------------------------ */
    case LWS_CALLBACK_CLIENT_CLOSED:
        if (!c) break;
        pthread_mutex_lock(&c->stop_mu);
        c->wsi   = NULL;
        c->state = STATE_IDLE;
        if (c->stopping) {
            c->stopped = 1;
            pthread_cond_signal(&c->stop_cond);
            pthread_mutex_unlock(&c->stop_mu);
        } else {
            pthread_mutex_unlock(&c->stop_mu);
            schedule_reconnect(c);
        }
        break;

    default:
        break;
    }

    (void)user_data;
    return 0;
}

/* ======================================================================
 * Service 线程
 * ====================================================================== */

static void *service_thread(void *arg)
{
    wsp_pool_t *pool = arg;

    while (!pool->destroying) {
        lws_service(pool->ctx, SERVICE_POLL_MS);
    }

    return NULL;
}

/* ======================================================================
 * 连接池生命周期
 * ====================================================================== */

wsp_pool_t *wsp_pool_create(void)
{
    wsp_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pthread_mutex_init(&pool->pool_lock, NULL);
    return pool;
}

int wsp_pool_start(wsp_pool_t *pool)
{
    if (!pool) return 0;

    /* 初始化协议表 */
    pool->protocols[0].name                  = "wsp-pool";
    pool->protocols[0].callback              = ws_pool_callback;
    pool->protocols[0].per_session_data_size = 0;
    pool->protocols[0].rx_buffer_size        = 0;
    /* protocols[1] 全零作为哨兵 */

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = pool->protocols;
    info.user      = pool;   /* lws_context_user(ctx) == pool */
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    pool->ctx = lws_create_context(&info);
    if (!pool->ctx) return 0;

    pool->destroying = 0;
    if (pthread_create(&pool->thread, NULL, service_thread, pool) != 0) {
        lws_context_destroy(pool->ctx);
        pool->ctx = NULL;
        return 0;
    }
    pool->thread_started = 1;

    /* 若 start 前已有 pending 客户端, 触发一次 CANCELLED 让 service 消费 */
    pthread_mutex_lock(&pool->pool_lock);
    int has_pending = (pool->n_pending > 0);
    pthread_mutex_unlock(&pool->pool_lock);
    if (has_pending)
        lws_cancel_service(pool->ctx);

    return 1;
}

void wsp_pool_destroy(wsp_pool_t *pool)
{
    if (!pool) return;

    /* 停止所有客户端 */
    pthread_mutex_lock(&pool->pool_lock);
    int nc = pool->n_clients;
    wsp_client_t *snap[WSP_MAX_CLIENTS];
    memcpy(snap, pool->clients, nc * sizeof(wsp_client_t *));
    pthread_mutex_unlock(&pool->pool_lock);

    for (int i = 0; i < nc; i++)
        wsp_client_stop(snap[i]);

    /* 停止 service 线程 */
    if (pool->thread_started) {
        pool->destroying = 1;
        if (pool->ctx)
            lws_cancel_service(pool->ctx);
        pthread_join(pool->thread, NULL);
        pool->thread_started = 0;
    }

    if (pool->ctx) {
        lws_context_destroy(pool->ctx);
        pool->ctx = NULL;
    }

    /* 释放所有客户端内存 */
    for (int i = 0; i < nc; i++)
        wsp_client_destroy(snap[i]);

    /* 释放 pending 中尚未 add 成功的客户端 (极端情况) */
    pthread_mutex_lock(&pool->pool_lock);
    for (int i = 0; i < pool->n_pending; i++)
        wsp_client_destroy(pool->pending[i]);
    pool->n_pending = 0;
    pthread_mutex_unlock(&pool->pool_lock);

    pthread_mutex_destroy(&pool->pool_lock);
    free(pool);
}

/* ======================================================================
 * 客户端创建与配置
 * ====================================================================== */

wsp_client_t *wsp_client_create(const char *host, int port,
                                  const char *protocol,
                                  wsl_rx_cb_t rx_cb, void *user)
{
    if (!host || port <= 0) return NULL;

    wsp_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->host       = strdup(host);
    c->proto_name = strdup(protocol ? protocol : "");
    c->path       = strdup("/");
    if (!c->host || !c->proto_name || !c->path) {
        wsp_client_destroy(c);
        return NULL;
    }

    c->port = port;
    c->rx_cb = rx_cb;
    c->user  = user;

    c->ping_interval_ms   = DEFAULT_PING_INTERVAL_MS;
    c->pong_timeout_ms    = DEFAULT_PONG_TIMEOUT_MS;
    c->reconnect_init_ms  = DEFAULT_RECONNECT_INIT_MS;
    c->reconnect_max_ms   = DEFAULT_RECONNECT_MAX_MS;
    c->reconnect_delay_ms = DEFAULT_RECONNECT_INIT_MS;

    pthread_mutex_init(&c->q_lock,   NULL);
    pthread_mutex_init(&c->stop_mu,  NULL);
    pthread_cond_init (&c->stop_cond, NULL);

    c->state    = STATE_IDLE;
    c->stopping = 0;
    c->stopped  = 0;

    return c;
}

void wsp_set_ping(wsp_client_t *c, int interval_ms, int pong_timeout_ms)
{
    if (!c) return;
    c->ping_interval_ms = interval_ms;
    c->pong_timeout_ms  = pong_timeout_ms;
}

void wsp_set_reconnect(wsp_client_t *c, int init_ms, int max_ms)
{
    if (!c) return;
    c->reconnect_init_ms  = init_ms;
    c->reconnect_max_ms   = max_ms;
    c->reconnect_delay_ms = init_ms;
}

void wsp_set_queue_limit(wsp_client_t *c, int max_msgs, int max_bytes)
{
    if (!c) return;
    c->max_queue_msgs  = (size_t)max_msgs;
    c->max_queue_bytes = (size_t)max_bytes;
}

void wsp_set_ssl(wsp_client_t *c, int enabled, int skip_verify)
{
    if (!c) return;
    c->use_ssl        = enabled ? 1 : 0;
    c->ssl_skip_verify = skip_verify ? 1 : 0;
}

void wsp_set_path(wsp_client_t *c, const char *path)
{
    if (!c || !path) return;
    free(c->path);
    c->path = strdup(path);
}

/* ======================================================================
 * 连接池管理
 * ====================================================================== */

LwsClientRet_e wsp_pool_add(wsp_pool_t *pool, wsp_client_t *c)
{
    if (!pool || !c) return LWS_ERR_PARAM;
    if (c->pool)      return LWS_ERR_PARAM; /* 已属于某个池 */

    pthread_mutex_lock(&pool->pool_lock);

    if (pool->n_clients + pool->n_pending >= WSP_MAX_CLIENTS) {
        pthread_mutex_unlock(&pool->pool_lock);
        return LWS_ERR_PARAM;
    }

    c->pool = pool;

    /* 注册到活跃列表 */
    pool->clients[pool->n_clients++] = c;

    /* 同时放入 pending, 由 service 线程在 CANCELLED 中发起连接 */
    pool->pending[pool->n_pending++] = c;

    int ctx_ready = (pool->ctx != NULL);
    pthread_mutex_unlock(&pool->pool_lock);

    if (ctx_ready)
        lws_cancel_service(pool->ctx);

    return LWS_OK;
}

/* ======================================================================
 * 客户端停止与释放
 * ====================================================================== */

void wsp_client_stop(wsp_client_t *c)
{
    if (!c) return;

    pthread_mutex_lock(&c->stop_mu);
    if (c->stopping || c->stopped) {
        pthread_mutex_unlock(&c->stop_mu);
        return;
    }
    c->stopping = 1;
    pthread_mutex_unlock(&c->stop_mu);

    /* 取消 sul 定时器, 阻止下一次自动重连 */
    if (c->pool && c->pool->ctx)
        lws_sul_cancel(&c->sul);

    /* 加锁读取 wsi: service 线程在 CLOSED/CONNECTION_ERROR 中
     * 持有 stop_mu 写入 wsi, 确保读取不构成 data race.
     * lws_cancel_service 在锁内调用, 随后 condvar wait 原子释放锁,
     * 消除 lost wakeup 窗口:
     *   stop 线程                  service 线程 (CLOSED)
     *   ───────────                ───────────────────
     *   lock(stop_mu)
     *   read wsi (non-NULL)
     *   lws_cancel_service()
     *   pthread_cond_wait()        lock(stop_mu)  ← wait 已释放锁
     *   (atomically unlocks)       wsi = NULL
     *                              stopped = 1
     *                              signal(stop_cond)
     *                              unlock(stop_mu)
     *   wait returns (locked)
     *   stopped == true → exit
     *   unlock(stop_mu) */
    pthread_mutex_lock(&c->stop_mu);
    if (c->wsi && c->pool && c->pool->ctx) {
        lws_cancel_service(c->pool->ctx);
        while (!c->stopped)
            pthread_cond_wait(&c->stop_cond, &c->stop_mu);
        pthread_mutex_unlock(&c->stop_mu);
    } else {
        if (!c->stopped)
            c->stopped = 1;
        pthread_mutex_unlock(&c->stop_mu);
    }

    /* 清空发送队列 */
    pthread_mutex_lock(&c->q_lock);
    flush_queue_locked(c);
    pthread_mutex_unlock(&c->q_lock);
}

void wsp_client_destroy(wsp_client_t *c)
{
    if (!c) return;

    pthread_mutex_lock(&c->q_lock);
    flush_queue_locked(c);
    pthread_mutex_unlock(&c->q_lock);

    free(c->host);
    free(c->proto_name);
    free(c->path);
    free(c->frag_buf);

    pthread_mutex_destroy(&c->q_lock);
    pthread_mutex_destroy(&c->stop_mu);
    pthread_cond_destroy(&c->stop_cond);

    free(c);
}

/* ======================================================================
 * 发送 (线程安全)
 * ====================================================================== */

static LwsClientRet_e enqueue(wsp_client_t *c, const void *data, size_t len, int is_binary)
{
    if (!c || !data) return LWS_ERR_PARAM;

    msg_node_t *node = malloc(sizeof(*node));
    if (!node) return LWS_ERR_QUEUE_FULL;

    node->buf = malloc(LWS_PRE + len);
    if (!node->buf) { free(node); return LWS_ERR_QUEUE_FULL; }

    memcpy(node->buf + LWS_PRE, data, len);
    node->len       = len;
    node->is_binary = is_binary;
    node->next      = NULL;

    pthread_mutex_lock(&c->q_lock);

    if (c->stopping) {
        pthread_mutex_unlock(&c->q_lock);
        free(node->buf);
        free(node);
        return LWS_ERR_PARAM;
    }

    if ((c->max_queue_msgs  && c->q_msgs >= c->max_queue_msgs) ||
        (c->max_queue_bytes && c->q_bytes + len > c->max_queue_bytes)) {
        pthread_mutex_unlock(&c->q_lock);
        free(node->buf);
        free(node);
        return LWS_ERR_QUEUE_FULL;
    }

    if (c->q_tail) c->q_tail->next = node;
    else           c->q_head = node;
    c->q_tail = node;
    c->q_msgs++;
    c->q_bytes += len;

    pthread_mutex_unlock(&c->q_lock);

    if (c->pool && c->pool->ctx)
        lws_cancel_service(c->pool->ctx);

    return LWS_OK;
}

LwsClientRet_e wsp_send(wsp_client_t *c, const char *data, size_t len)
{
    return enqueue(c, data, len, 0);
}

LwsClientRet_e wsp_send_binary(wsp_client_t *c, const void *data, size_t len)
{
    return enqueue(c, data, len, 1);
}
