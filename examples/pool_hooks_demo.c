/**
 * pool_hooks_demo.c - 事件钩子层在 ws_pool 中的使用示例
 *
 * 三个独立场景, 演示 wsp_set_event_hooks 的不同用法.
 * 所有场景连接到 ws://127.0.0.1:9000
 *
 * 编译: cmake --build build --target pool_hooks_demo
 * 运行前需启动 echo_server: ./build/app/echo_server
 */
#include "ws_pool.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

/* ====================================================================
 * 场景 1: 基础钩子 — 仅注册 on_connected 和 on_message
 * ==================================================================== */
static void basic_on_connected(void *user)
{
    (void)user;
    printf("[basic] 连接已建立\n");
}

static void basic_on_message(const char *data, size_t len,
                              int is_binary, void *user)
{
    (void)user;
    printf("[basic] 收到 %s 消息 (%zu 字节): %.*s\n",
           is_binary ? "binary" : "text", len, (int)len, data);
}

static void demo_basic_hooks(void)
{
    printf("\n===== 场景 1: 基础钩子 (pool) =====\n");

    wsp_pool_t *pool = wsp_pool_create();
    if (!pool) { printf("[basic] pool create failed\n"); return; }

    wsp_client_t *c = wsp_client_create("127.0.0.1", 8080,
                                         "ws", NULL, NULL);
    if (!c) {
        printf("[basic] client create failed\n");
        wsp_pool_destroy(pool);
        return;
    }

    const wsl_event_hooks_t hooks = {
        .on_connected = basic_on_connected,
        .on_message   = basic_on_message,
    };
    wsp_set_event_hooks(c, &hooks, NULL);

    if (wsp_pool_add(pool, c) != LWS_OK) {
        printf("[basic] pool_add failed\n");
        wsp_client_destroy(c);
        wsp_pool_destroy(pool);
        return;
    }

    if (!wsp_pool_start(pool)) {
        printf("[basic] pool_start failed\n");
        wsp_pool_destroy(pool);
        return;
    }

    sleep(1);
    wsp_send(c, "hello from pool hooks demo", 26);

    sleep(3);
    wsp_pool_destroy(pool);
    printf("[basic] 结束\n");
}

/* ====================================================================
 * 场景 2: 自定义重连策略 — 连续失败 3 次后停止重连
 * ==================================================================== */
struct rc_ctx { int max; };

static void rc_on_disconnected(void *user)
{
    (void)user;
    printf("[reconnect] 连接已断开\n");
}

static int rc_reconnect_decision(int fail_count, int cur_delay, void *user)
{
    struct rc_ctx *ctx = (struct rc_ctx *)user;
    printf("[reconnect] 第 %d 次重连失败, 当前延迟 %d ms\n",
           fail_count, cur_delay);
    if (fail_count >= ctx->max) {
        printf("[reconnect] 达到最大次数 %d, 停止重连\n", ctx->max);
        return -1;
    }
    return cur_delay;
}

static void demo_custom_reconnect(void)
{
    printf("\n===== 场景 2: 自定义重连 (pool) =====\n");

    struct rc_ctx ctx = { .max = 3 };

    wsp_pool_t *pool = wsp_pool_create();
    if (!pool) { printf("[reconnect] pool create failed\n"); return; }

    wsp_client_t *c = wsp_client_create("127.0.0.1", 9001,
                                         "ws", NULL, NULL);
    if (!c) {
        printf("[reconnect] client create failed\n");
        wsp_pool_destroy(pool);
        return;
    }

    wsp_set_reconnect(c, 200, 1000);

    const wsl_event_hooks_t hooks = {
        .on_disconnected       = rc_on_disconnected,
        .on_reconnect_decision = rc_reconnect_decision,
    };
    wsp_set_event_hooks(c, &hooks, &ctx);

    wsp_pool_add(pool, c);
    wsp_pool_start(pool);

    sleep(8);
    wsp_pool_destroy(pool);
    printf("[reconnect] 结束\n");
}

/* ====================================================================
 * 场景 3: 认证握手 — 连接建立后发送 token, 在 on_message 中验证
 * ==================================================================== */
struct auth_ctx {
    wsp_client_t *client;
    char          token[256];
    int           authenticated;
};

static void auth_on_connected(void *user)
{
    struct auth_ctx *ctx = (struct auth_ctx *)user;
    printf("[auth] 连接已建立, 发送认证信息...\n");
    ctx->authenticated = 0;
    wsp_send(ctx->client, ctx->token, strlen(ctx->token));
}

static void auth_on_message(const char *data, size_t len,
                             int is_binary, void *user)
{
    struct auth_ctx *ctx = (struct auth_ctx *)user;
    (void)is_binary;
    printf("[auth] 收到消息 (%zu 字节): %.*s\n", len, (int)len, data);

    if (!ctx->authenticated && strncmp(data, "auth:", 5) == 0) {
        ctx->authenticated = 1;
        printf("[auth] 认证成功: %.*s\n", (int)(len - 5), data + 5);
    }
}

static void auth_on_disconnected(void *user)
{
    struct auth_ctx *ctx = (struct auth_ctx *)user;
    ctx->authenticated = 0;
    printf("[auth] 连接断开, 重置认证状态\n");
}

static void demo_auth_handshake(void)
{
    printf("\n===== 场景 3: 认证握手 (pool) =====\n");

    struct auth_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.token, sizeof(ctx.token), "auth:pool_token_%d", 999);

    wsp_pool_t *pool = wsp_pool_create();
    if (!pool) { printf("[auth] pool create failed\n"); return; }

    wsp_client_t *c = wsp_client_create("127.0.0.1", 8080,
                                         "ws", NULL, NULL);
    if (!c) {
        printf("[auth] client create failed\n");
        wsp_pool_destroy(pool);
        return;
    }
    ctx.client = c;

    const wsl_event_hooks_t hooks = {
        .on_connected    = auth_on_connected,
        .on_message      = auth_on_message,
        .on_disconnected = auth_on_disconnected,
    };
    wsp_set_event_hooks(c, &hooks, &ctx);

    wsp_pool_add(pool, c);
    wsp_pool_start(pool);

    sleep(4);
    wsp_pool_destroy(pool);
    printf("[auth] 结束\n");
}

/* ====================================================================
 * 主函数
 * ==================================================================== */
int main(void)
{
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    printf("pool_hooks_demo: 事件钩子层示例\n");

    demo_basic_hooks();
    demo_custom_reconnect();
    demo_auth_handshake();

    printf("\n所有场景运行完毕\n");
    return 0;
}
