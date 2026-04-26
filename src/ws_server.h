/**
 * ws_server.h - 轻量 WebSocket echo 服务器
 *
 * 用途: 本地测试 ws_client / ws_pool 示例程序，无需外部服务依赖。
 *
 * 特性:
 *   - 任意数量并发客户端 (受 fd 限制)
 *   - 自动 echo 收到的文本/二进制帧 (含分片重组)
 *   - 可选消息回调，方便观察内容
 *   - service 线程独立运行，主线程可自由控制生命周期
 *
 * 用法:
 *   wss_server_t *s = wss_create(8080, NULL);
 *   wss_set_rx_cb(s, my_rx, NULL);
 *   wss_start(s);
 *   // ... 等待退出 ...
 *   wss_stop(s);
 *   wss_destroy(s);
 */

#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wss_server wss_server_t;

/**
 * 消息到达回调 (在 service 线程中调用)
 *
 * data:      消息内容（非 NUL 结尾）
 * len:       消息字节数
 * binary:    1 = 二进制帧, 0 = 文本帧
 * client_id: 连接的编号（连接顺序递增）
 * user:      wss_set_rx_cb() 传入的用户指针
 */
typedef void (*wss_rx_cb_t)(const char *data, size_t len,
                             int binary, int client_id, void *user);

/**
 * 连接事件回调 (在 service 线程中调用)
 *
 * client_id: 连接编号
 * connected: 1 = 新连接, 0 = 断开
 * user:      wss_set_conn_cb() 传入的用户指针
 */
typedef void (*wss_conn_cb_t)(int client_id, int connected, void *user);

/* ---- 生命周期 ---- */

/**
 * wss_create - 创建服务器实例
 *
 * port:     监听端口
 * protocol: WebSocket 子协议名（NULL = 接受任意协议）
 *
 * 返回 NULL 表示内存分配失败
 */
wss_server_t *wss_create(int port, const char *protocol);

/** 设置消息回调（须在 wss_start 前调用）*/
void wss_set_rx_cb(wss_server_t *s, wss_rx_cb_t cb, void *user);

/** 设置连接事件回调（须在 wss_start 前调用）*/
void wss_set_conn_cb(wss_server_t *s, wss_conn_cb_t cb, void *user);

/**
 * wss_start - 启动 service 线程，开始监听
 * 返回 0 成功，-1 失败
 */
int wss_start(wss_server_t *s);

/**
 * wss_stop - 通知 service 线程退出并等待其结束
 */
void wss_stop(wss_server_t *s);

/**
 * wss_destroy - 释放所有资源（须在 wss_stop 之后调用）
 */
void wss_destroy(wss_server_t *s);

#ifdef __cplusplus
}
#endif
