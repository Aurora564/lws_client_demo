#include "ws_client.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ---------------------编译期常量---------------------------*/
#define DEFAULT_PING_INTERVAL_MS    30000   /* 默认心跳间隔时间 */
#define DEFAULT_PONG_TIMEOUT_MS     10000   /* 默认 PONG 超时时间 */
#define DEFAULT_RECONNECT_INIT_MS   1000    /* 默认初始重连间隔 */
#define DEFAULT_RECONNECT_MAX_MS    60000   /* 默认最大重连间隔 */
#define SERVICE_POLL_MS             50      /* lws_service 超时， 控制超时重连检查粒度 */
#define FRAG_INIT_CAP               4096    /* 分片缓冲初始容量 */

/*-----------------------内部数据结构------------------------*/

/* 发送队列节点 */
typedef struct msg_node {
    unsigned char   *buf;       /* LWS_PRE + payload 连续分配 */
    size_t           len;       /* payload 字节数 */
    int              is_binary;
    struct msg_node *next;
} msg_node_t;

typedef enum {
    STATE_IDLE,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_RECONNECT_WAIT,
} client_state_t;

struct wsl_client {
    /* -- 配置 （wsl_start 前只读）-- */
    char        *host;
    int          port;
    char        *proto_name;
    char        *path;          /* WebSocket 路径 */
    wsl_rx_cb_t  rx_cb;
    void        *user;

    int          use_ssl;       /* 1 = WSS, 0 = WS */
    int          ssl_skip_verify; /* 1 = 跳过证书验证 */

    int          ping_interval_ms;
    int          pong_timeout_ms;
    int          reconnect_init_ms;
    int          reconnect_max_ms;
    size_t       max_queue_msgs;
    size_t       max_queue_bytes;

    /* -- LWS -- */
    struct lws_context      *ctx;
    struct lws              *wsi;
    struct lws_protocols     protocols[2]; /* [0]=业务， [1]=哨兵 */

    /* -- Service -- */
    pthread_t       thread;
    int             thread_started;
    volatile int    stopping;

    /* -- 状态 （仅 service 线程可访问） -- */
    client_state_t  state;
    int             reconnect_delay_ms; /* 当前退避间隔  */
    struct timespec reconnect_at;   /* CLOCK_MONOTONIC 绝对时刻 */

    /* -- 心跳 -- */
    int send_ping;
    int ping_pending;

    /* -- 发送队列（q_lock 保护) -- */
    pthread_mutex_t     q_lock;
    msg_node_t         *q_head;
    msg_node_t         *q_tail;
    size_t              q_msgs;
    size_t              q_bytes;

    /* -- 分片缓冲区 -- */
    unsigned char *frag_buf;
    size_t         frag_len;
    size_t         frag_cap;
};

/* -----------------------内部工具函数 --------------------------- */

/* 返回 CLOCK_MONOTONIC 的毫秒时间戳 */
static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}


/* 按当前的 reconnect_delay_ms 设置 reconnect_at, 并对 delay 做指数退避 */
static void schedule_reconnect(wsl_client_t *c)
{
    long at = ms_now() + c->reconnect_delay_ms;
    c->reconnect_at.tv_sec = at / 1000L;
    c->reconnect_at.tv_nsec = (at % 1000L) * 1000000L;

    c->reconnect_delay_ms *= 2;
    if (c->reconnect_delay_ms > c->reconnect_max_ms)
    {
        c->reconnect_delay_ms = c->reconnect_max_ms;
    }

    c->state = STATE_RECONNECT_WAIT;
}


/* 向分片缓冲区添加数据， OOM 返回 -1 */
static int frag_append(wsl_client_t *c, const unsigned char *data, size_t len)
{
    if (c->frag_len + len > c->frag_cap)
    {
        size_t cap = c->frag_cap ? c->frag_cap : FRAG_INIT_CAP;
        while (cap < c->frag_len + len)
        {
            cap *= 2;
        }

        unsigned char *p = realloc(c->frag_buf, cap);
        if (!p) return -1;
        c->frag_cap = cap;
        c->frag_buf = p;
    }
    memcpy(c->frag_buf + c->frag_len, data, len);
    c->frag_len += len;
    return 0;
}


/* 清空队列节点，调用者必须持有 q_lock */
static void flush_queue_locked(wsl_client_t *c)
{
    msg_node_t *n = c->q_head;
    while (n)
    {
        msg_node_t *next = n->next;
        free(n->buf);
        free(n);
        n = next;
    }
    c->q_head = c->q_tail = NULL;
    c->q_msgs = 0;
    c->q_bytes = 0;
}

/* ---------------------------------lws callback ----------------------------*/

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user_data, void *in, size_t len)
{
    (void)user_data;

    /* 部分 reason 会以 NULL wsi 进入， 提前预防 */
    if (!wsi) return 0;

    wsl_client_t *c = lws_context_user(lws_get_context(wsi));
    if (!c) return 0;

    switch (reason){

    /* -- 连接建立 -- */
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        c->wsi = wsi;
        c->state = STATE_CONNECTED;
        c->reconnect_delay_ms = c->reconnect_init_ms;
        c->send_ping = 0;
        c->ping_pending = 0;
        lws_set_timer_usecs(wsi, (lws_usec_t)c->ping_interval_ms * 1000);
        break;

    /* -- 接收 -- */
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        int first = lws_is_first_fragment(wsi);
        int final = lws_is_final_fragment(wsi);
        size_t remaining = lws_remaining_packet_payload(wsi);

        if (first && final && remaining == 0) {
            if (c->rx_cb)
                c->rx_cb((const char *)in, len, c->user);
        } else {
            if (first)
                c->frag_len = 0;
            if (frag_append(c, (const unsigned char *)in, len) < 0)
                return -1;
            if (final && remaining == 0) {
                if (c->rx_cb)
                    c->rx_cb((const char *)c->frag_buf, c->frag_len, c->user);
                c->frag_len = 0;
            }
        }
        break;
    }

    /* -- 可写 -- */
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (c->send_ping) {
            unsigned char ping_buf[LWS_PRE + 4] = {0};
            lws_write(wsi, ping_buf + LWS_PRE, 0, LWS_WRITE_PING);
            c->send_ping = 0;
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
            lws_write(wsi, node->buf + LWS_PRE, node->len,
                      node->is_binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
            free(node->buf);
            free(node);
            if (more)
                lws_callback_on_writable(wsi);
        }
        break;
    }

    /* -- lws_cancel_service 唤醒 -- */
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        if (c->wsi && c->state == STATE_CONNECTED) {
            pthread_mutex_lock(&c->q_lock);
            int has = (c->q_head != NULL);
            pthread_mutex_unlock(&c->q_lock);
            if (has)
                lws_callback_on_writable(c->wsi);
        }
        break;

    /* -- 心跳定时器 -- */
    case LWS_CALLBACK_TIMER:
        if (c->ping_pending) {
            fprintf(stderr, "[wsl] pong timeout, closing\n");
            return -1;
        }
        c->send_ping = 1;
        lws_callback_on_writable(wsi);
        lws_set_timer_usecs(wsi, (lws_usec_t)c->pong_timeout_ms * 1000);
        break;

    /* -- PONG -- */
    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        c->ping_pending = 0;
        lws_set_timer_usecs(wsi, (lws_usec_t)c->ping_interval_ms * 1000);
        break;

    /* -- 连接失败 -- */
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "[wsl] connection error: %s\n",
                in ? (const char *)in : "(none)");
        c->wsi = NULL;
        if (!c->stopping)
            schedule_reconnect(c);
        break;

    /* -- 连接关闭 -- */
    case LWS_CALLBACK_CLIENT_CLOSED:
        c->wsi = NULL;
        if (!c->stopping)
            schedule_reconnect(c);
        break;

    default:
        break;
    }

    return 0;
}

/* -------------------------发起连接------------------------------- */

static void do_connect(wsl_client_t *c)
{
    struct lws_client_connect_info info;
    memset(&info, 0, sizeof(info));
    info.context = c->ctx;
    info.address = c->host;
    info.port = c->port;
    info.path = c->path && c->path[0] ? c->path : "/";
    info.host = c->host;
    info.origin = c->host;
    info.protocol = c->proto_name[0] ? c->proto_name : NULL;

    /* SSL/TLS 配置 */
    if (c->use_ssl) {
        /* LCCSCF_USE_SSL: 启用 SSL
         * LCCSCF_ALLOW_SELFSIGNED: 允许自签名证书
         * LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK: 跳过主机名验证
         * LCCSCF_ALLOW_EXPIRED: 允许过期证书
         */
        info.ssl_connection = LCCSCF_USE_SSL;
        if (c->ssl_skip_verify) {
            info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED |
                                   LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                                   LCCSCF_ALLOW_EXPIRED;
        }
    } else {
        info.ssl_connection = 0;
    }

    c->state = STATE_CONNECTING;
    c->wsi = lws_client_connect_via_info(&info);
    if (!c->wsi)
        schedule_reconnect(c);
}

/* -------------------------Service 线程---------------------------- */

static void *service_thread(void *arg)
{
    wsl_client_t *c = arg;
    do_connect(c);

    while (!c->stopping) {
        if (c->state == STATE_RECONNECT_WAIT) {
            long at = (long)c->reconnect_at.tv_sec * 1000L
                    + (long)c->reconnect_at.tv_nsec / 1000000L;
            if (ms_now() >= at)
                do_connect(c);
        }
        lws_service(c->ctx, SERVICE_POLL_MS);
    }

    pthread_mutex_lock(&c->q_lock);
    flush_queue_locked(c);
    pthread_mutex_unlock(&c->q_lock);

    return NULL;
}

/* -------------------------公开 API------------------------------- */

wsl_client_t *wsl_create(const char *host, int port,
                            const char *protocol,
                            wsl_rx_cb_t rx_cb, void *user)
{
    if (!host || port <= 0) return NULL;

    wsl_client_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    pthread_mutex_init(&c->q_lock, NULL);

    c->host = strdup(host);
    c->proto_name = strdup(protocol ? protocol : "");
    c->path = strdup("/");  /* 默认路径 */
    if (!c->host || !c->proto_name || !c->path) {
        wsl_destroy(c);
        return NULL;
    }

    c->port = port;
    c->rx_cb = rx_cb;
    c->user = user;

    c->use_ssl = 0;            /* 默认不使用 SSL */
    c->ssl_skip_verify = 0;    /* 默认严格验证证书 */

    c->ping_interval_ms = DEFAULT_PING_INTERVAL_MS;
    c->pong_timeout_ms = DEFAULT_PONG_TIMEOUT_MS;
    c->reconnect_init_ms = DEFAULT_RECONNECT_INIT_MS;
    c->reconnect_max_ms = DEFAULT_RECONNECT_MAX_MS;
    c->reconnect_delay_ms = DEFAULT_RECONNECT_INIT_MS;

    c->protocols[0].name = c->proto_name;
    c->protocols[0].callback = ws_callback;
    c->protocols[0].per_session_data_size = 0;
    c->protocols[0].rx_buffer_size = 0;

    return c;
}

void wsl_set_ping(wsl_client_t *c, int interval_ms, int pong_timeout_ms)
{
    if (!c) return;
    c->ping_interval_ms = interval_ms;
    c->pong_timeout_ms = pong_timeout_ms;
}

void wsl_set_reconnect(wsl_client_t *c, int init_ms, int max_ms)
{
    if (!c) return;
    c->reconnect_init_ms = init_ms;
    c->reconnect_max_ms = max_ms;
    c->reconnect_delay_ms = init_ms;
}

void wsl_set_queue_limit(wsl_client_t *c, int max_msgs, int max_bytes)
{
    if (!c) return;
    c->max_queue_msgs = max_msgs;
    c->max_queue_bytes = max_bytes;
}

void wsl_set_ssl(wsl_client_t *c, int enabled, int skip_verify)
{
    if (!c) return;
    c->use_ssl = enabled ? 1 : 0;
    c->ssl_skip_verify = skip_verify ? 1 : 0;
}

void wsl_set_path(wsl_client_t *c, const char *path)
{
    if (!c || !path) return;
    free(c->path);
    c->path = strdup(path);
}

int wsl_start(wsl_client_t *c)
{
    if (!c) return 0;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = c->protocols;
    info.user = c;

    c->ctx = lws_create_context(&info);
    if (!c->ctx) return 0;

    c->stopping = 0;
    c->state = STATE_IDLE;

    if (pthread_create(&c->thread, NULL, service_thread, c) != 0) {
        lws_context_destroy(c->ctx);
        c->ctx = NULL;
        return 0;
    }

    c->thread_started = 1;
    return 1;
}

void wsl_stop(wsl_client_t *c)
{
    if (!c || !c->thread_started || c->stopping) return;

    c->stopping = 1;
    if (c->ctx)
        lws_cancel_service(c->ctx);

    pthread_join(c->thread, NULL);
    c->thread_started = 0;
    c->wsi = NULL;

    if (c->ctx) {
        lws_context_destroy(c->ctx);
        c->ctx = NULL;
    }
}

void wsl_destroy(wsl_client_t *c)
{
    if (!c) return;

    msg_node_t *n = c->q_head;
    while (n) {
        msg_node_t *next = n->next;
        free(n->buf);
        free(n);
        n = next;
    }

    free(c->host);
    free(c->proto_name);
    free(c->path);
    free(c->frag_buf);
    pthread_mutex_destroy(&c->q_lock);
    free(c);
}

/* -- 内部入队 -- */
static LwsClientRet_e enqueue(wsl_client_t *c, const void *data, size_t len, int is_binary)
{
    if (!c || !data) return LWS_ERR_PARAM;

    msg_node_t *node = malloc(sizeof(*node));
    if (!node) return LWS_ERR_QUEUE_FULL;

    node->buf = malloc(LWS_PRE + len);
    if (!node->buf) { free(node); return LWS_ERR_QUEUE_FULL; }

    memcpy(node->buf + LWS_PRE, data, len);
    node->len = len;
    node->is_binary = is_binary;
    node->next = NULL;

    pthread_mutex_lock(&c->q_lock);
    if ((c->max_queue_msgs && c->q_msgs >= c->max_queue_msgs) ||
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

    if (c->ctx)
        lws_cancel_service(c->ctx);

    return LWS_OK;
}

LwsClientRet_e wsl_send(wsl_client_t *c, const char *data, size_t len)
{
    return enqueue(c, data, len, 0);
}

LwsClientRet_e wsl_send_binary(wsl_client_t *c, const void *data, size_t len)
{
    return enqueue(c, data, len, 1);
}
