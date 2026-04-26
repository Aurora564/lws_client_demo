/**
 * example.c - WebSocket 客户端使用示例
 *
 * 编译: gcc example.c -I./output/include -I./output/include/libwebsockets \
 *       -I./src -L./build/app -lwsclient \
 *       -L./output/lib -lwebsockets \
 *       -lpthread -lssl -lcrypto -lz -lm -o example
 *
 * 用法: ./example [host] [port] [protocol] [--wss] [--skip-verify] [path]
 * 示例:
 *   ./example localhost 8080 my-protocol                    # WS
 *   ./example example.com 443 chat --wss                    # WSS (严格验证)
 *   ./example test.com 443 api --wss --skip-verify /ws/v1  # WSS (跳过证书验证)
 */

#include "ws_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static volatile int running = 1;

/* 信号处理 */
void signal_handler(int sig) {
    printf("\n收到信号 %d，准备退出...\n", sig);
    running = 0;
}

/* 接收回调函数 */
void on_message_received(const char *data, size_t len, int is_binary, void *user) {
    (void)user;  /* 未使用的参数 */
    (void)is_binary;
    printf("[收到消息] 长度: %zu 字节\n", len);
    if (len > 0) {
        printf("  内容: %.*s\n", (int)len, data);
    }
}

int main(int argc, char *argv[]) {
    const char *host = "localhost";
    int port = 8080;
    const char *protocol = "my-protocol";
    const char *path = "/";
    int use_ssl = 0;
    int skip_verify = 0;

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wss") == 0) {
            use_ssl = 1;
        } else if (strcmp(argv[i], "--skip-verify") == 0) {
            skip_verify = 1;
        } else if (argv[i][0] != '-') {
            /* 位置参数 */
            if (host == NULL || port == 0) {
                host = argv[i];
            } else if (port == 8080) {
                port = atoi(argv[i]);
            } else if (strcmp(protocol, "my-protocol") == 0) {
                protocol = argv[i];
            } else {
                path = argv[i];
            }
        }
    }

    /* 重新解析以确保正确性 */
    host = "localhost";
    port = 8080;
    protocol = "my-protocol";
    path = "/";
    
    int pos_args = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wss") == 0) {
            use_ssl = 1;
        } else if (strcmp(argv[i], "--skip-verify") == 0) {
            skip_verify = 1;
        } else {
            pos_args++;
            switch (pos_args) {
                case 1: host = argv[i]; break;
                case 2: port = atoi(argv[i]); break;
                case 3: protocol = argv[i]; break;
                case 4: path = argv[i]; break;
            }
        }
    }

    printf("========================================\n");
    printf("  WebSocket Client Example\n");
    printf("========================================\n");
    printf("服务器地址: %s://%s:%d%s\n", use_ssl ? "wss" : "ws", host, port, path);
    printf("协议名称: %s\n", protocol);
    printf("SSL/TLS:  %s\n", use_ssl ? "启用" : "禁用");
    if (use_ssl) {
        printf("证书验证: %s\n", skip_verify ? "跳过 (不安全)" : "严格验证");
    }
    printf("\n");

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 1. 创建客户端实例 */
    wsld_client_t *client = wsld_create(host, port, protocol, on_message_received, NULL);
    if (!client) {
        fprintf(stderr, "错误: 创建客户端失败\n");
        return 1;
    }

    /* 2. 配置 SSL/TLS（如果启用）*/
    if (use_ssl) {
        wsld_set_ssl(client, 1, skip_verify);
    }

    /* 3. 配置自定义路径（如果不是默认值）*/
    if (strcmp(path, "/") != 0) {
        wsld_set_path(client, path);
    }

    /* 4. 配置心跳和重连参数（可选）*/
    wsld_set_ping(client, 30000, 10000);           // 30秒心跳，10秒超时
    wsld_set_reconnect(client, 1000, 60000);       // 初始重连1秒，最大60秒
    wsld_set_queue_limit(client, 100, 1024 * 1024); // 最多100条消息或1MB

    /* 5. 启动客户端（开始连接并启动服务线程）*/
    if (!wsld_start(client)) {
        fprintf(stderr, "错误: 启动客户端失败\n");
        wsld_destroy(client);
        return 1;
    }

    printf("客户端已启动，按 Ctrl+C 退出\n\n");

    /* 6. 主循环 - 模拟发送消息 */
    int msg_count = 0;
    while (running) {
        sleep(2);

        /* 每2秒发送一条文本消息 */
        char msg[256];
        snprintf(msg, sizeof(msg), "Hello from client! Message #%d", ++msg_count);

        LwsClientRet_e ret = wsld_send(client, msg, strlen(msg));
        if (ret == LWS_OK) {
            printf("[发送] %s\n", msg);
        } else if (ret == LWS_ERR_QUEUE_FULL) {
            printf("[警告] 发送队列已满\n");
        } else {
            printf("[错误] 发送失败\n");
        }
    }

    /* 5. 清理资源 */
    printf("\n正在关闭客户端...\n");
    wsld_destroy(client);
    printf("客户端已关闭\n");

    return 0;
}
