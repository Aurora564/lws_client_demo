/**
 * echo_server.c - WebSocket echo 服务器
 *
 * 用途: 本地测试 ws_client / ws_pool 示例程序。
 *       收到任意客户端的消息后原样回显。
 *
 * 用法:
 *   ./echo_server [port] [protocol]
 *   默认 port=8080，protocol 接受任意子协议
 *
 * 配合客户端示例:
 *   终端 1: ./echo_server
 *   终端 2: ./pool_basic
 *   终端 2: ./pool_multi_client
 *   终端 2: ./pool_asr_sessions --sessions 5
 *   终端 2: ./example
 */

#include "ws_server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static wss_server_t *g_server = NULL;

static void on_signal(int sig)
{
    (void)sig;
    printf("\nShutting down...\n");
    if (g_server) wss_stop(g_server);
}

static void on_conn(int client_id, int connected, void *user)
{
    (void)user;
    /* 连接/断开事件已由 ws_server.c 内部打印，这里可扩展业务逻辑 */
    (void)client_id;
    (void)connected;
}

static void on_rx(const char *data, size_t len, int binary,
                  int client_id, void *user)
{
    (void)user;
    (void)binary;
    (void)client_id;
    /* 截断过长消息只显示前 80 字节 */
    size_t show = len < 80 ? len : 80;
    printf("[server] echo: %.*s%s\n", (int)show, data, len > 80 ? "..." : "");
}

int main(int argc, char *argv[])
{
    int         port     = 8080;
    const char *protocol = NULL;

    if (argc > 1) port     = atoi(argv[1]);
    if (argc > 2) protocol = argv[2];

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    g_server = wss_create(port, protocol);
    if (!g_server) {
        fprintf(stderr, "wss_create failed\n");
        return 1;
    }

    wss_set_rx_cb(g_server,   on_rx,   NULL);
    wss_set_conn_cb(g_server, on_conn, NULL);

    if (wss_start(g_server) != 0) {
        wss_destroy(g_server);
        return 1;
    }

    printf("Echo server running. Press Ctrl+C to quit.\n\n");

    /* 主线程阻塞等待信号 */
    pause();

    wss_destroy(g_server);
    return 0;
}
