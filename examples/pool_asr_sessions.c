/**
 * pool_asr_sessions.c - 使用 ws_pool 的 ASR 多会话示例
 *
 * 场景: 与 asr_multi_session.c 完全相同的智能会议 ASR 场景，
 *       但将底层从 ws_client（1:1 模型）改为 ws_pool（1:N 模型）。
 *
 *   asr_multi_session.c (ws_client)  │  pool_asr_sessions.c (ws_pool)
 *   ─────────────────────────────────┼──────────────────────────────────
 *   N 个 lws_context                 │  1 个 lws_context
 *   N+1 个线程 (每会话 1 个)          │  2 个线程 (1 service + 1 main)
 *   fd 数: ~N × 数十                 │  fd 数: ~N (仅 socket)
 *   内存: ~N × lws_context 开销      │  内存: 1 个 lws_context
 *
 * 用法:
 *   ./pool_asr_sessions [host] [port] [--wss] [--sessions N]
 *   默认 N=5；最大 64 个会话
 *
 * 编译:
 *   cmake --build build && ./build/app/pool_asr_sessions
 */

#include "ws_pool.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_SESSIONS    64
#define AUDIO_CHUNK_MS  100
#define AUDIO_CHUNK_SZ  (16000 * 2 * AUDIO_CHUNK_MS / 1000)  /* 16kHz 16-bit mono */

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ==================== 数据结构 ==================== */

typedef struct {
    char          user_id[32];
    char          room_id[32];
    wsp_client_t *client;
    volatile int  is_speaking;
    int           chunks_sent;
    int           results_recv;
} asr_session_t;

typedef struct {
    asr_session_t   sessions[MAX_SESSIONS];
    int             count;
    pthread_mutex_t lock;
    volatile int    running;
    wsp_pool_t     *pool;
} asr_system_t;

static asr_system_t g_sys;

/* ==================== ASR 结果回调（在 service 线程中调用）==================== */

static void on_asr_result(const char *data, size_t len, void *user)
{
    asr_session_t *s = (asr_session_t *)user;

    pthread_mutex_lock(&g_sys.lock);
    s->results_recv++;
    int n = s->results_recv;
    pthread_mutex_unlock(&g_sys.lock);

    printf("[ASR] %-10s %-8s | #%-3d %.*s\n",
           s->user_id, s->room_id, n, (int)len, data);
}

/* ==================== 会话管理 ==================== */

static int session_start(asr_session_t *s, const char *host, int port, int use_ssl)
{
    char path[128];
    snprintf(path, sizeof(path), "/ws/asr?user=%s&room=%s", s->user_id, s->room_id);

    s->client = wsp_client_create(host, port, "asr-protocol", on_asr_result, s);
    if (!s->client) {
        fprintf(stderr, "[ERR] wsp_client_create failed for %s\n", s->user_id);
        return -1;
    }

    wsp_set_ssl(s->client, use_ssl, 0);
    wsp_set_path(s->client, path);
    wsp_set_ping(s->client, 15000, 5000);
    wsp_set_reconnect(s->client, 500, 10000);
    wsp_set_queue_limit(s->client, 50, 512 * 1024);

    /* wsp_pool_add 在 pool_start 前后均可调用 */
    if (wsp_pool_add(g_sys.pool, s->client) != LWS_OK) {
        fprintf(stderr, "[ERR] wsp_pool_add failed for %s\n", s->user_id);
        wsp_client_destroy(s->client);
        s->client = NULL;
        return -1;
    }

    printf("[+] %-10s (room:%-8s) registered\n", s->user_id, s->room_id);
    return 0;
}

/* ==================== 模拟音频流线程 ==================== */

typedef struct {
    asr_session_t *session;
    int            duration_ms;
} speak_args_t;

static void *speaking_thread(void *arg)
{
    speak_args_t  *a     = (speak_args_t *)arg;
    asr_session_t *s     = a->session;
    int total_chunks     = a->duration_ms / AUDIO_CHUNK_MS;
    free(a);

    unsigned char audio[AUDIO_CHUNK_SZ];
    memset(audio, 0, sizeof(audio)); /* 真实场景替换为 PCM 采集数据 */

    s->is_speaking = 1;
    for (int i = 0; i < total_chunks && g_sys.running; i++) {
        LwsClientRet_e ret = wsp_send_binary(s->client, audio, sizeof(audio));
        if (ret == LWS_OK)
            s->chunks_sent++;
        else if (ret == LWS_ERR_QUEUE_FULL)
            fprintf(stderr, "[WARN] %-10s queue full, chunk dropped\n", s->user_id);
        msleep(AUDIO_CHUNK_MS);
    }
    s->is_speaking = 0;
    return NULL;
}

/* ==================== 信号处理 ==================== */

static void on_signal(int sig) { (void)sig; g_sys.running = 0; }

/* ==================== 统计输出 ==================== */

static void print_stats(void)
{
    printf("\n--- stats ---\n");
    pthread_mutex_lock(&g_sys.lock);
    for (int i = 0; i < g_sys.count; i++) {
        asr_session_t *s = &g_sys.sessions[i];
        printf("  %-10s %-8s | chunks=%-5d results=%-4d %s\n",
               s->user_id, s->room_id, s->chunks_sent, s->results_recv,
               s->is_speaking ? "[speaking]" : "[silent]");
    }
    pthread_mutex_unlock(&g_sys.lock);
    printf("  total sessions: %d  service threads: 1\n", g_sys.count);
    printf("-------------\n\n");
}

/* ==================== 主函数 ==================== */

int main(int argc, char *argv[])
{
    const char *host    = "localhost";
    int         port    = 8080;
    int         use_ssl = 0;
    int         n_sess  = 5;

    for (int i = 1, pos = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--wss")) { use_ssl = 1; continue; }
        if (!strcmp(argv[i], "--sessions") && i + 1 < argc) {
            n_sess = atoi(argv[++i]);
            if (n_sess < 1)           n_sess = 1;
            if (n_sess > MAX_SESSIONS) n_sess = MAX_SESSIONS;
            continue;
        }
        switch (++pos) {
        case 1: host = argv[i]; break;
        case 2: port = atoi(argv[i]); break;
        }
    }

    printf("pool_asr_sessions  %s://%s:%d  sessions=%d  threads=2\n\n",
           use_ssl ? "wss" : "ws", host, port, n_sess);

    memset(&g_sys, 0, sizeof(g_sys));
    pthread_mutex_init(&g_sys.lock, NULL);
    g_sys.running = 1;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* 1. 创建池（单个 lws_context，尚未启动） */
    g_sys.pool = wsp_pool_create();
    if (!g_sys.pool) { fprintf(stderr, "wsp_pool_create failed\n"); return 1; }

    /* 2. 批量注册会话（在 pool_start 之前注册亦可）*/
    static const char *rooms[] = { "room_A", "room_B", "room_C", "room_D" };
    for (int i = 0; i < n_sess; i++) {
        asr_session_t *s = &g_sys.sessions[i];
        snprintf(s->user_id, sizeof(s->user_id), "user_%03d", i + 1);
        snprintf(s->room_id,  sizeof(s->room_id),  "%s", rooms[i % 4]);
        if (session_start(s, host, port, use_ssl) == 0)
            g_sys.count++;
    }

    /* 3. 启动池（一次调用驱动全部 N 个连接，仅创建 1 个 service 线程）*/
    if (!wsp_pool_start(g_sys.pool)) {
        fprintf(stderr, "wsp_pool_start failed\n");
        wsp_pool_destroy(g_sys.pool);
        return 1;
    }
    printf("\n%d sessions started with 1 service thread. "
           "Waiting 2s for connections...\n\n", g_sys.count);
    sleep(2);

    /* 4. 启动模拟音频流线程（每会话一个，代表麦克风输入线程）*/
    pthread_t threads[MAX_SESSIONS];
    int       thread_count = 0;

    for (int i = 0; i < g_sys.count; i++) {
        speak_args_t *a = malloc(sizeof(*a));
        a->session     = &g_sys.sessions[i];
        a->duration_ms = 8000 + (i % 4) * 2000; /* 8~14 秒，各会话错落 */
        pthread_create(&threads[thread_count++], NULL, speaking_thread, a);
        msleep(200); /* 轻微错开启动，避免同时握手 */
    }

    /* 5. 主循环：定期打印统计，等待所有会话说完 */
    int tick = 0;
    while (g_sys.running) {
        sleep(3);
        if (++tick % 2 == 0) print_stats();

        int all_done = 1;
        for (int i = 0; i < g_sys.count; i++)
            if (g_sys.sessions[i].is_speaking) { all_done = 0; break; }
        if (all_done) {
            printf("All sessions finished speaking.\n");
            break;
        }
    }

    for (int i = 0; i < thread_count; i++)
        pthread_join(threads[i], NULL);

    print_stats();

    /* 6. 销毁池（统一停止所有连接，释放全部内存）*/
    printf("Shutting down pool...\n");
    wsp_pool_destroy(g_sys.pool);
    pthread_mutex_destroy(&g_sys.lock);
    printf("Done.\n");
    return 0;
}
