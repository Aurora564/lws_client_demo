#include "ws_server.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAG_INIT_CAP   4096
#define SERVICE_POLL_MS 50

/* ---- 每连接数据 ---- */

typedef struct {
    int            client_id;

    /* echo 缓冲 (LWS_PRE + payload) */
    unsigned char *echo_buf;
    size_t         echo_len;
    int            echo_binary;

    /* 分片重组缓冲 */
    unsigned char *frag_buf;
    size_t         frag_len;
    size_t         frag_cap;
} per_session_t;

/* ---- 服务器结构体 ---- */

struct wss_server {
    int              port;
    char            *protocol;

    wss_rx_cb_t      rx_cb;
    void            *rx_user;
    wss_conn_cb_t    conn_cb;
    void            *conn_user;

    struct lws_context     *ctx;
    struct lws_protocols    protocols[2]; /* [0]=业务, [1]=哨兵 */

    pthread_t        thread;
    volatile int     stopping;
    int              next_id;  /* 单调递增的连接编号，仅 service 线程访问 */
};

/* ---- 内部工具 ---- */

static int frag_append(per_session_t *ps, const unsigned char *data, size_t len)
{
    if (ps->frag_len + len > ps->frag_cap) {
        size_t cap = ps->frag_cap ? ps->frag_cap : FRAG_INIT_CAP;
        while (cap < ps->frag_len + len) cap *= 2;
        unsigned char *p = realloc(ps->frag_buf, cap);
        if (!p) return -1;
        ps->frag_buf = p;
        ps->frag_cap = cap;
    }
    memcpy(ps->frag_buf + ps->frag_len, data, len);
    ps->frag_len += len;
    return 0;
}

static int prepare_echo(per_session_t *ps, const unsigned char *data,
                         size_t len, int binary)
{
    free(ps->echo_buf);
    ps->echo_buf = malloc(LWS_PRE + len);
    if (!ps->echo_buf) return -1;
    memcpy(ps->echo_buf + LWS_PRE, data, len);
    ps->echo_len    = len;
    ps->echo_binary = binary;
    return 0;
}

/* ---- LWS 回调 ---- */

static int server_callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len)
{
    per_session_t *ps  = (per_session_t *)user;
    wss_server_t  *srv = (wss_server_t *)lws_context_user(lws_get_context(wsi));

    if (!srv) return 0;

    switch (reason) {

    case LWS_CALLBACK_ESTABLISHED:
        memset(ps, 0, sizeof(*ps));
        ps->client_id = srv->next_id++;
        printf("[server] client #%d connected\n", ps->client_id);
        if (srv->conn_cb)
            srv->conn_cb(ps->client_id, 1, srv->conn_user);
        break;

    case LWS_CALLBACK_RECEIVE: {
        int first  = lws_is_first_fragment(wsi);
        int final  = lws_is_final_fragment(wsi);
        int binary = lws_frame_is_binary(wsi);

        if (first) ps->frag_len = 0;

        if (frag_append(ps, (const unsigned char *)in, len) < 0) {
            fprintf(stderr, "[server] OOM in frag_append\n");
            return -1;
        }

        if (final) {
            const unsigned char *payload = ps->frag_buf;
            size_t               plen   = ps->frag_len;

            printf("[server] client #%d rx %zu bytes (%s)\n",
                   ps->client_id, plen, binary ? "binary" : "text");

            if (srv->rx_cb)
                srv->rx_cb((const char *)payload, plen, binary,
                            ps->client_id, srv->rx_user);

            if (prepare_echo(ps, payload, plen, binary) == 0)
                lws_callback_on_writable(wsi);

            ps->frag_len = 0;
        }
        break;
    }

    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (ps->echo_buf) {
            int write_mode = ps->echo_binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT;
            lws_write(wsi, ps->echo_buf + LWS_PRE, ps->echo_len, write_mode);
            free(ps->echo_buf);
            ps->echo_buf = NULL;
        }
        break;

    case LWS_CALLBACK_CLOSED:
        printf("[server] client #%d disconnected\n", ps->client_id);
        if (srv->conn_cb)
            srv->conn_cb(ps->client_id, 0, srv->conn_user);
        free(ps->echo_buf);
        free(ps->frag_buf);
        ps->echo_buf = NULL;
        ps->frag_buf = NULL;
        break;

    default:
        break;
    }
    return 0;
}

/* ---- Service 线程 ---- */

static void *service_thread(void *arg)
{
    wss_server_t *srv = (wss_server_t *)arg;
    while (!srv->stopping)
        lws_service(srv->ctx, SERVICE_POLL_MS);
    return NULL;
}

/* ---- 公共 API ---- */

wss_server_t *wss_create(int port, const char *protocol)
{
    wss_server_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->port     = port;
    s->protocol = protocol ? strdup(protocol) : NULL;
    s->next_id  = 1;
    return s;
}

void wss_set_rx_cb(wss_server_t *s, wss_rx_cb_t cb, void *user)
{
    s->rx_cb   = cb;
    s->rx_user = user;
}

void wss_set_conn_cb(wss_server_t *s, wss_conn_cb_t cb, void *user)
{
    s->conn_cb   = cb;
    s->conn_user = user;
}

int wss_start(wss_server_t *s)
{
    s->protocols[0].name                  = s->protocol ? s->protocol : "default";
    s->protocols[0].callback              = server_callback;
    s->protocols[0].per_session_data_size = sizeof(per_session_t);
    s->protocols[0].rx_buffer_size        = 65536;
    /* protocols[1] 是全零哨兵，calloc 已清零 */

    struct lws_context_creation_info info = {0};
    info.port      = s->port;
    info.protocols = s->protocols;
    info.gid       = -1;
    info.uid       = -1;
    info.user      = s;

    s->ctx = lws_create_context(&info);
    if (!s->ctx) {
        fprintf(stderr, "[server] lws_create_context failed\n");
        return -1;
    }

    s->stopping = 0;
    if (pthread_create(&s->thread, NULL, service_thread, s) != 0) {
        fprintf(stderr, "[server] pthread_create failed\n");
        lws_context_destroy(s->ctx);
        s->ctx = NULL;
        return -1;
    }

    printf("[server] listening on ws://0.0.0.0:%d  protocol=%s\n",
           s->port, s->protocol ? s->protocol : "(any)");
    return 0;
}

void wss_stop(wss_server_t *s)
{
    if (!s->ctx) return;
    s->stopping = 1;
    lws_cancel_service(s->ctx);
    pthread_join(s->thread, NULL);
    lws_context_destroy(s->ctx);
    s->ctx = NULL;
}

void wss_destroy(wss_server_t *s)
{
    if (!s) return;
    free(s->protocol);
    free(s);
}
