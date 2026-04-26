/**
 * v2_hooks_demo.c - 事件钩子层使用示例
 *
 * 三个独立场景, 演示 wsl_event_hooks_t 的不同用法.
 * 所有场景连接到 ws://127.0.0.1:9000
 *
 * 编译: cmake --build build --target v2_hooks_demo
 * 运行前需启动 echo_server: ./build/app/echo_server
 */
#include "ws_client_v2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    printf("\n===== 场景 1: 基础钩子 =====\n");

    wsl_client_t *c = wsl_create("127.0.0.1", 8080, NULL, NULL, NULL);
    if (!c) {
        printf("[basic] 创建失败\n");
        return;
    }

    /* 事件钩子表可分配在栈上, caller 保证在 start 后存活 */
    const wsl_event_hooks_t hooks = {
        .on_connected = basic_on_connected,
        .on_message   = basic_on_message,
    };
    wsl_set_event_hooks(c, &hooks, NULL);

    if (!wsl_start(c)) {
        printf("[basic] 启动失败\n");
        wsl_destroy(c);
        return;
    }

    wsl_send(c, "hello from basic hooks", 22);

    sleep(3);
    wsl_stop(c);
    wsl_destroy(c);
    printf("[basic] 结束\n");
}

/* ====================================================================
 * 场景 2: 自定义重连策略 — 连续失败 5 次后停止重连
 *
 * 通过 on_reconnect_decision 实现熔断器模式,
 * 在 fail_count 达到上限时返回 -1 通知客户端停止重连.
 * ==================================================================== */
struct reconnect_ctx {
    int max_failures;
};

static void reconnect_on_connected(void *user)
{
    (void)user;
    printf("[reconnect] 连接已建立\n");
}

static void reconnect_on_disconnected(void *user)
{
    (void)user;
    printf("[reconnect] 连接已断开\n");
}

static int reconnect_decision(int fail_count, int current_delay_ms,
                               void *user)
{
    struct reconnect_ctx *ctx = (struct reconnect_ctx *)user;
    printf("[reconnect] 第 %d 次重连失败, 当前延迟 %d ms\n",
           fail_count, current_delay_ms);

    if (fail_count >= ctx->max_failures) {
        printf("[reconnect] 达到最大重连次数 %d, 停止重连\n",
               ctx->max_failures);
        return -1;  /* 停止重连, 客户端转入 STATE_STOPPED */
    }
    return current_delay_ms;  /* 沿用默认指数退避延迟 */
}

static void demo_custom_reconnect(void)
{
    printf("\n===== 场景 2: 自定义重连 (熔断) =====\n");

    struct reconnect_ctx ctx = { .max_failures = 5 };

    wsl_client_t *c = wsl_create("127.0.0.1", 9001, "ws", NULL, NULL);
    if (!c) {
        printf("[reconnect] 创建失败\n");
        return;
    }

    /* 快速重连, 便于观察 */
    wsl_set_reconnect(c, 200, 1000);

    const wsl_event_hooks_t hooks = {
        .on_connected         = reconnect_on_connected,
        .on_disconnected      = reconnect_on_disconnected,
        .on_reconnect_decision = reconnect_decision,
    };
    wsl_set_event_hooks(c, &hooks, &ctx);

    if (!wsl_start(c)) {
        printf("[reconnect] 启动失败\n");
        wsl_destroy(c);
        return;
    }

    /* 运行 10 秒, 观察重连到停止的完整过程 */
    sleep(10);
    wsl_stop(c);
    wsl_destroy(c);
    printf("[reconnect] 结束\n");
}

/* ====================================================================
 * 场景 3: 认证握手 — 连接建立后发送 token, 在 on_message 中验证
 *
 * on_connected 中调用 wsl_send 发送认证信息,
 * on_message 中根据服务端回复判断认证结果.
 * 断开时重置认证状态避免重连后状态残留.
 * ==================================================================== */
struct auth_ctx {
    wsl_client_t *client;   /* 用于在钩子回调中发送消息 */
    char          token[256];
    int           authenticated;
};

static void auth_on_connected(void *user)
{
    struct auth_ctx *ctx = (struct auth_ctx *)user;
    printf("[auth] 连接已建立, 发送认证信息...\n");
    ctx->authenticated = 0;
    /* 连接建立后立即发送 token 进行认证 */
    wsl_send(ctx->client, ctx->token, strlen(ctx->token));
}

static void auth_on_message(const char *data, size_t len,
                             int is_binary, void *user)
{
    struct auth_ctx *ctx = (struct auth_ctx *)user;
    (void)is_binary;
    printf("[auth] 收到消息 (%zu 字节): %.*s\n", len, (int)len, data);

    if (!ctx->authenticated) {
        /* 首次收到回复视为认证响应 */
        if (strncmp(data, "auth:", 5) == 0) {
            ctx->authenticated = 1;
            printf("[auth] 认证成功: %.*s\n", (int)(len - 5), data + 5);
        } else {
            printf("[auth] 认证失败, 服务端回复: %.*s\n", (int)len, data);
        }
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
    printf("\n===== 场景 3: 认证握手 =====\n");

    struct auth_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    snprintf(ctx.token, sizeof(ctx.token), "auth:demo_token_%d", 12345);

    wsl_client_t *c = wsl_create("127.0.0.1", 8080, "ws", NULL, NULL);
    if (!c) {
        printf("[auth] 创建失败\n");
        return;
    }
    ctx.client = c;  /* 钩子回调通过 ctx 间接引用 client */

    const wsl_event_hooks_t hooks = {
        .on_connected    = auth_on_connected,
        .on_message      = auth_on_message,
        .on_disconnected = auth_on_disconnected,
    };
    wsl_set_event_hooks(c, &hooks, &ctx);

    if (!wsl_start(c)) {
        printf("[auth] 启动失败\n");
        wsl_destroy(c);
        return;
    }

    sleep(3);
    wsl_stop(c);
    wsl_destroy(c);
    printf("[auth] 结束\n");
}

/* ====================================================================
 * 主函数
 * ==================================================================== */
int main(void)
{
    printf("v2_hooks_demo: 事件钩子层示例\n");

    demo_basic_hooks();
    demo_custom_reconnect();
    demo_auth_handshake();

    printf("\n所有场景运行完毕\n");
    return 0;
}
