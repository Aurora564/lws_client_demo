/**
 * asr_multi_session.c - ASR 多会话并发示例
 *
 * 场景: 智能会议系统，多个用户同时进行实时语音识别。
 *       每个用户独立持有一个 wsl_client_t 实例（独立线程 + 独立 lws_context），
 *       互不干扰，天然支持不同协议/路径/语言参数。
 *
 * 生命周期:
 *   wsl_create → wsl_set_* → wsl_start → wsl_send_binary(loop) → wsl_stop → wsl_destroy
 *
 * 编译:
 *   cmake --build build && ./build/app/asr_multi_session
 *
 * 用法:
 *   ./asr_multi_session [host] [port] [--wss]
 */

#include "ws_client.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ==================== 常量 ==================== */

#define MAX_SESSIONS    10
#define AUDIO_CHUNK_MS  100                     /* 每块音频时长 ms */
#define AUDIO_CHUNK_SZ  (16000 * 2 * AUDIO_CHUNK_MS / 1000) /* 16kHz 16bit mono */

/* ==================== 数据结构 ==================== */

typedef struct {
    char            user_id[32];
    char            room_id[32];
    wsl_client_t   *client;
    volatile int    is_speaking;
    int             chunks_sent;
    int             results_recv;
} asr_session_t;

typedef struct {
    asr_session_t   sessions[MAX_SESSIONS];
    int             count;
    pthread_mutex_t lock;
    volatile int    running;
} asr_system_t;

static asr_system_t g_sys;

/* ==================== ASR 结果回调 ==================== */

static void on_asr_result(const char *data, size_t len, int is_binary, void *user)
{
    asr_session_t *s = (asr_session_t *)user;
    (void)is_binary;

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

    s->client = wsl_create(host, port, "asr-protocol", on_asr_result, s);
    if (!s->client) {
        fprintf(stderr, "[ERR] wsl_create failed for %s\n", s->user_id);
        return -1;
    }

    wsl_set_ssl(s->client, use_ssl, 0);
    wsl_set_path(s->client, path);
    wsl_set_ping(s->client, 15000, 5000);
    wsl_set_reconnect(s->client, 500, 10000);
    wsl_set_queue_limit(s->client, 50, 512 * 1024);

    if (!wsl_start(s->client)) {
        fprintf(stderr, "[ERR] wsl_start failed for %s\n", s->user_id);
        wsl_destroy(s->client);
        s->client = NULL;
        return -1;
    }

    printf("[+] %-10s (room:%-8s) session started\n", s->user_id, s->room_id);
    return 0;
}

static void session_stop(asr_session_t *s)
{
    if (!s->client) return;
    wsl_stop(s->client);
    wsl_destroy(s->client);
    s->client = NULL;
    printf("[-] %-10s (room:%-8s) session stopped  chunks=%d results=%d\n",
           s->user_id, s->room_id, s->chunks_sent, s->results_recv);
}

/* ==================== 模拟音频流线程 ==================== */

typedef struct {
    asr_session_t *session;
    int            duration_ms;
} speak_args_t;

static void *speaking_thread(void *arg)
{
    speak_args_t  *a = (speak_args_t *)arg;
    asr_session_t *s = a->session;
    int            total_chunks = a->duration_ms / AUDIO_CHUNK_MS;
    free(a);

    unsigned char audio[AUDIO_CHUNK_SZ];
    memset(audio, 0, sizeof(audio)); /* 实际使用时替换为真实 PCM 数据 */

    s->is_speaking = 1;

    for (int i = 0; i < total_chunks && g_sys.running; i++) {
        LwsClientRet_e ret = wsl_send_binary(s->client, audio, sizeof(audio));
        if (ret == LWS_OK) {
            s->chunks_sent++;
        } else if (ret == LWS_ERR_QUEUE_FULL) {
            fprintf(stderr, "[WARN] %-10s queue full, chunk dropped\n", s->user_id);
        }
        msleep(AUDIO_CHUNK_MS);
    }

    s->is_speaking = 0;
    return NULL;
}

/* ==================== 信号处理 ==================== */

static void on_signal(int sig)
{
    (void)sig;
    g_sys.running = 0;
}

/* ==================== 辅助：打印统计 ==================== */

static void print_stats(void)
{
    printf("\n--- stats ---\n");
    pthread_mutex_lock(&g_sys.lock);
    for (int i = 0; i < g_sys.count; i++) {
        asr_session_t *s = &g_sys.sessions[i];
        printf("  %-10s %-8s | chunks=%-5d results=%-4d %s\n",
               s->user_id, s->room_id,
               s->chunks_sent, s->results_recv,
               s->is_speaking ? "[speaking]" : "[silent]");
    }
    pthread_mutex_unlock(&g_sys.lock);
    printf("-------------\n\n");
}

/* ==================== 主函数 ==================== */

int main(int argc, char *argv[])
{
    const char *host = "localhost";
    int         port = 8080;
    int         use_ssl = 0;

    for (int i = 1, pos = 0; i < argc; i++) {
        if (strcmp(argv[i], "--wss") == 0) { use_ssl = 1; continue; }
        switch (++pos) {
            case 1: host = argv[i]; break;
            case 2: port = atoi(argv[i]); break;
        }
    }

    printf("ASR multi-session demo  %s://%s:%d\n\n",
           use_ssl ? "wss" : "ws", host, port);

    memset(&g_sys, 0, sizeof(g_sys));
    pthread_mutex_init(&g_sys.lock, NULL);
    g_sys.running = 1;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* ------ 定义会话 ------ */
    static const struct { const char *user; const char *room; } cfg[] = {
        { "user_001", "room_A" },
        { "user_002", "room_A" },
        { "user_003", "room_B" },
        { "user_004", "room_B" },
        { "user_005", "room_C" },
    };
    int n = (int)(sizeof(cfg) / sizeof(cfg[0]));

    /* ------ 启动会话 ------ */
    for (int i = 0; i < n; i++) {
        asr_session_t *s = &g_sys.sessions[i];
        strncpy(s->user_id, cfg[i].user, sizeof(s->user_id) - 1);
        strncpy(s->room_id, cfg[i].room, sizeof(s->room_id) - 1);
        if (session_start(s, host, port, use_ssl) == 0)
            g_sys.count++;
    }

    printf("\n%d sessions active, waiting 2s for connections...\n\n", g_sys.count);
    sleep(2);

    /* ------ 启动说话线程（各用户错开开始，持续时长不同）------ */
    static const int durations_ms[] = { 10000, 8000, 12000, 6000, 15000 };
    pthread_t threads[MAX_SESSIONS];

    for (int i = 0; i < g_sys.count; i++) {
        speak_args_t *a = malloc(sizeof(*a));
        a->session     = &g_sys.sessions[i];
        a->duration_ms = durations_ms[i];
        pthread_create(&threads[i], NULL, speaking_thread, a);
        msleep(500); /* 各用户错开 0.5s 启动 */
    }

    /* ------ 主循环：定期打印统计，等待用户说完 ------ */
    int tick = 0;
    while (g_sys.running) {
        sleep(3);
        if (++tick % 3 == 0) print_stats();

        int all_done = 1;
        for (int i = 0; i < g_sys.count; i++)
            if (g_sys.sessions[i].is_speaking) { all_done = 0; break; }
        if (all_done) {
            printf("All users finished speaking.\n");
            break;
        }
    }

    /* ------ 等待说话线程退出 ------ */
    for (int i = 0; i < g_sys.count; i++)
        pthread_join(threads[i], NULL);

    print_stats();

    /* ------ 关闭所有会话 ------ */
    printf("Shutting down sessions...\n");
    for (int i = 0; i < g_sys.count; i++)
        session_stop(&g_sys.sessions[i]);

    pthread_mutex_destroy(&g_sys.lock);
    printf("\nDone.\n");
    return 0;
}
