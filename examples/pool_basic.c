/**
 * pool_basic.c - ws_pool 基础用法
 *
 * 场景: 单连接池 + 单客户端，展示完整生命周期：
 *       创建 → 配置 → 启动 → 发送 → 关闭
 *
 * 与 example.c (ws_client) 的核心区别:
 *   - 不为每个连接创建独立线程和 lws_context
 *   - 所有连接共享一个 service 线程
 *   - 添加第 2、3 ... N 个连接无额外线程开销
 *
 * 编译:
 *   cmake --build build && ./build/app/pool_basic
 *
 * 用法:
 *   ./pool_basic [host] [port] [protocol] [--wss] [--skip-verify] [path]
 */

#include "ws_pool.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void on_message(const char *data, size_t len, int is_binary, void *user)
{
    (void)user;
    (void)is_binary;
    printf("[recv] %.*s\n", (int)len, data);
}

int main(int argc, char *argv[])
{
    const char *host     = "localhost";
    int         port     = 8080;
    const char *protocol = NULL;
    const char *path     = "/";
    int         use_ssl  = 0, skip_verify = 0;

    for (int i = 1, pos = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--wss"))         { use_ssl = 1;     continue; }
        if (!strcmp(argv[i], "--skip-verify")) { skip_verify = 1; continue; }
        switch (++pos) {
        case 1: host     = argv[i]; break;
        case 2: port     = atoi(argv[i]); break;
        case 3: protocol = argv[i]; break;
        case 4: path     = argv[i]; break;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("pool_basic  %s://%s:%d%s\n\n", use_ssl ? "wss" : "ws", host, port, path);

    /* 1. 创建连接池 */
    wsp_pool_t *pool = wsp_pool_create();
    if (!pool) { fprintf(stderr, "wsp_pool_create failed\n"); return 1; }

    /* 2. 创建客户端并配置 */
    wsp_client_t *c = wsp_client_create(host, port, protocol, on_message, NULL);
    if (!c) {
        fprintf(stderr, "wsp_client_create failed\n");
        wsp_pool_destroy(pool);
        return 1;
    }
    wsp_set_path(c, path);
    wsp_set_ssl(c, use_ssl, skip_verify);
    wsp_set_ping(c, 30000, 10000);
    wsp_set_reconnect(c, 1000, 30000);

    /* 3. 注册到池（此时尚未连接） */
    if (wsp_pool_add(pool, c) != LWS_OK) {
        fprintf(stderr, "wsp_pool_add failed\n");
        wsp_client_destroy(c);
        wsp_pool_destroy(pool);
        return 1;
    }

    /* 4. 启动池（创建 lws_context + service 线程，触发连接） */
    if (!wsp_pool_start(pool)) {
        fprintf(stderr, "wsp_pool_start failed\n");
        wsp_pool_destroy(pool);
        return 1;
    }

    printf("Pool started. Press Ctrl+C to quit.\n\n");

    /* 5. 主循环：每 2 秒发一条消息 */
    int seq = 0;
    while (g_running) {
        sleep(2);

        char msg[64];
        snprintf(msg, sizeof(msg), "ping #%d", ++seq);

        LwsClientRet_e ret = wsp_send(c, msg, strlen(msg));
        if (ret == LWS_OK)
            printf("[send] %s\n", msg);
        else if (ret == LWS_ERR_QUEUE_FULL)
            printf("[warn] queue full\n");
    }

    /* 6. 销毁池（自动停止客户端并释放全部资源） */
    printf("\nShutting down...\n");
    wsp_pool_destroy(pool);
    printf("Done.\n");
    return 0;
}
