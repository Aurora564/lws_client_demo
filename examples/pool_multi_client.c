/**
 * pool_multi_client.c - ws_pool 多客户端示例
 *
 * 场景: 一个连接池管理 5 个 WebSocket 连接（各连接不同路径）。
 *       演示:
 *         1. 多个客户端共享同一 service 线程，无额外线程开销
 *         2. 精准发送：只向指定客户端发消息
 *         3. 广播：向所有客户端发送同一条消息
 *         4. 单独停止某个客户端，其余继续运行
 *         5. 运行期动态添加新客户端（不重启池）
 *
 * 编译:
 *   cmake --build build && ./build/app/pool_multi_client
 *
 * 用法:
 *   ./pool_multi_client [host] [port] [--wss]
 */

#include "ws_pool.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUM_CLIENTS 5

static volatile int g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

typedef struct {
    int  id;
    char name[32];
    int  recv_count;
} client_ctx_t;

static void on_message(const char *data, size_t len, int is_binary, void *user)
{
    client_ctx_t *ctx = (client_ctx_t *)user;
    (void)is_binary;
    ctx->recv_count++;
    printf("[recv] client%-2d (%-8s) #%-3d  %.*s\n",
           ctx->id, ctx->name, ctx->recv_count, (int)len, data);
}

static void broadcast(wsp_client_t **clients, int n, const char *msg)
{
    size_t len = strlen(msg);
    for (int i = 0; i < n; i++) {
        if (clients[i])
            wsp_send(clients[i], msg, len);
    }
}

int main(int argc, char *argv[])
{
    const char *host    = "localhost";
    int         port    = 8080;
    int         use_ssl = 0;

    for (int i = 1, pos = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--wss")) { use_ssl = 1; continue; }
        switch (++pos) {
        case 1: host = argv[i]; break;
        case 2: port = atoi(argv[i]); break;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("pool_multi_client  %s://%s:%d  (%d clients)\n\n",
           use_ssl ? "wss" : "ws", host, port, NUM_CLIENTS);

    /* 1. 创建池 */
    wsp_pool_t *pool = wsp_pool_create();
    if (!pool) { fprintf(stderr, "wsp_pool_create failed\n"); return 1; }

    /* 2. 创建多个客户端，各有独立上下文和路径 */
    static const char *paths[] = {
        "/ws/channel/alpha",
        "/ws/channel/beta",
        "/ws/channel/gamma",
        "/ws/channel/delta",
        "/ws/channel/epsilon",
    };

    wsp_client_t *clients[NUM_CLIENTS + 1]; /* +1 用于动态添加演示 */
    client_ctx_t  ctxs[NUM_CLIENTS + 1];
    memset(clients, 0, sizeof(clients));

    for (int i = 0; i < NUM_CLIENTS; i++) {
        ctxs[i].id         = i;
        ctxs[i].recv_count = 0;
        snprintf(ctxs[i].name, sizeof(ctxs[i].name), "ch-%d", i);

        clients[i] = wsp_client_create(host, port, NULL, on_message, &ctxs[i]);
        if (!clients[i]) {
            fprintf(stderr, "wsp_client_create[%d] failed\n", i);
            wsp_pool_destroy(pool);
            return 1;
        }
        wsp_set_path(clients[i], paths[i]);
        wsp_set_ssl(clients[i], use_ssl, 0);
        wsp_set_ping(clients[i], 20000, 5000);

        if (wsp_pool_add(pool, clients[i]) != LWS_OK) {
            fprintf(stderr, "wsp_pool_add[%d] failed\n", i);
            wsp_pool_destroy(pool);
            return 1;
        }
        printf("[+] client%d → %s\n", i, paths[i]);
    }

    /* 3. 启动池（1 个 service 线程驱动全部 N 个连接） */
    if (!wsp_pool_start(pool)) {
        fprintf(stderr, "wsp_pool_start failed\n");
        wsp_pool_destroy(pool);
        return 1;
    }
    printf("\nPool started: 1 service thread for %d connections.\n"
           "Press Ctrl+C to quit.\n\n", NUM_CLIENTS);

    sleep(2); /* 等待连接建立 */

    /* 4. 主循环演示 */
    int round = 0;
    while (g_running) {
        round++;

        /* 广播：所有活跃客户端收到相同消息 */
        char bcast[64];
        snprintf(bcast, sizeof(bcast), "broadcast round=%d", round);
        broadcast(clients, NUM_CLIENTS, bcast);
        printf("[bcast] %s → all clients\n", bcast);

        /* 精准：只向 client0 发送 */
        if (clients[0]) {
            char direct[64];
            snprintf(direct, sizeof(direct), "direct→client0 round=%d", round);
            wsp_send(clients[0], direct, strlen(direct));
            printf("[direct] %s\n", direct);
        }

        /* round=3: 停止 client2，其余继续运行 */
        if (round == 3 && clients[2]) {
            printf("\n[demo] Stopping client2 while others keep running...\n");
            wsp_client_stop(clients[2]);
            clients[2] = NULL; /* wsp_pool_destroy 会释放内存，不要再次 destroy */
            printf("[demo] client2 stopped.\n\n");
        }

        /* round=5: 动态添加一个新客户端（pool 已运行，无需重启） */
        if (round == 5 && !clients[NUM_CLIENTS]) {
            printf("\n[demo] Adding new client dynamically...\n");
            ctxs[NUM_CLIENTS].id         = NUM_CLIENTS;
            ctxs[NUM_CLIENTS].recv_count = 0;
            snprintf(ctxs[NUM_CLIENTS].name, sizeof(ctxs[NUM_CLIENTS].name), "dynamic");

            clients[NUM_CLIENTS] = wsp_client_create(
                host, port, NULL, on_message, &ctxs[NUM_CLIENTS]);
            if (clients[NUM_CLIENTS]) {
                wsp_set_path(clients[NUM_CLIENTS], "/ws/channel/dynamic");
                wsp_set_ssl(clients[NUM_CLIENTS], use_ssl, 0);
                if (wsp_pool_add(pool, clients[NUM_CLIENTS]) == LWS_OK)
                    printf("[+] dynamic client added → /ws/channel/dynamic\n\n");
                else
                    printf("[!] wsp_pool_add failed for dynamic client\n\n");
            }
        }

        sleep(3);
    }

    /* 5. 打印统计 */
    printf("\n--- recv stats ---\n");
    for (int i = 0; i <= NUM_CLIENTS; i++) {
        if (clients[i] || ctxs[i].recv_count > 0)
            printf("  client%d (%-8s): %d messages  %s\n",
                   ctxs[i].id, ctxs[i].name, ctxs[i].recv_count,
                   clients[i] ? "[active]" : "[stopped]");
    }

    /* 6. 销毁池（自动停止并释放所有客户端） */
    printf("\nShutting down...\n");
    wsp_pool_destroy(pool);
    printf("Done.\n");
    return 0;
}
