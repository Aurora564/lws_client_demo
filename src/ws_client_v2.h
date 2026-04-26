/**
 * ws_client_v2.h - 方案一 v2: lws_cancel_service + mutex 链表队列
 *
 * v2 相对于 v1 的优化:
 *   - 新增连接/错误事件回调, 业务无需再包装 rx_cb
 *   - 重连退避改用 lws_sul 定时器, 消除 service 循环轮询
 *   - msg_node_t 改用柔性数组成员, 单次 malloc/free
 *   - 分片缓冲使用 ws_frag_buf_t 抽象
 *
 * 线程模型:
 *      - 任意线程调用 wsl_send() -> 加锁入队 -> lws_cancel_service(ctx) 唤醒 service 线程
 *      - Service 线程收到 LWS_CALLBACK_EVENT_WAIT_CANCELLED -> lws_callback_on_writable(wsi)
 *      - Service 线程收到 LWS_CALLBACK_CLIENT_WRITABLE -> 出队 -> lws_write()
 *
 * 重连模型 (lws_sul 定时器, 指数退避)
 *      - 断开/失败后通过 lws_sul_schedule 注册定时器, 到期自动触发重连
 *      - 连接成功(ESTABLISHED)后重置为 reconnect_init_ms
 *
 * 心跳模型
 *      - 连接建立成功后 lws_set_timer_usecs() 注册 lws 内部计时器
 *      - LWS_CALLBACK_TIMER 触发 -> 设置 send_ping 标志 -> 请求 WRITABLE -> 发 PING
 *      - LWS_CALLBACK_CLIENT_RECEIVE_PONG -> 清 ping_pending -> 重置间隔定时器
 *      - TIMER 再次触发时若 ping_pending 仍置位 -> pong 超时 -> return -1 关闭连接
 *
 * 分片接收
 *      - 单帧完整交付(first && final && remaining == 0) 直接回调, 无需额外 malloc
 *      - 其余情况追加至 ws_frag_buf_t 动态缓冲, final 帧最后一块到齐后整体回调
 *
 * 嵌入方式
 *      - 将 ws_client_v2.{h,c} + ws_internal.h 复制到项目, 链接 libwebsockets + pthread
 */

#pragma once
#include <string.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* wsl_send() 返回值 */
typedef enum {
    LWS_OK              =  0,
    LWS_ERR_PARAM       = -1,
    LWS_ERR_QUEUE_FULL  = -2,
} LwsClientRet_e;

/* 接收回调: data 在回调返回后立即失效, 需要时请自行拷贝 */
typedef void (*wsl_rx_cb_t)(const char *data, size_t len, void *user);

/* 连接事件回调: connected=1 建立, 0 断开 */
typedef void (*wsl_conn_cb_t)(int connected, void *user);

/* 错误回调: error 字符串仅在该次调用内有效 */
typedef void (*wsl_err_cb_t)(const char *error, void *user);

typedef struct wsl_client wsl_client_t;

/*
 * wsl_create - 创建客户端实例 (不发起连接)
 *
 * host:        服务端地址
 * port:        服务端端口
 * protocol:    WebSocket 子协议名 (与服务端一致)
 * rx_cb:       收到完整消息时的回调, 在 service 线程中调用
 * user:        透传给 rx_cb 的用户指针
 *
 * 返回 NULL 表示内存分配失败.
 *
 * 默认值 (可在 wsl_start() 前通过 wsl_set_* 调整):
 *      心跳: 间隔 30s, pong 超时 10s
 *      重连: 首次 1s, 最大 60s, 每次失败翻倍
 *      流控: 队列无上限
 */
wsl_client_t *wsl_create(const char *host, int port,
                            const char *protocol,
                            wsl_rx_cb_t rx_cb, void *user);

/*
 * wsl_set_ping - 设置心跳相关参数 (须在 wsl_start 前调用)
 *
 * interval_ms:     发送 ping 时间间隔 (ms)
 * pong_timeout_ms: 等待 pong 的超时时间 (ms), 超时关闭连接并重连
 */
void wsl_set_ping(wsl_client_t *c, int interval_ms, int pong_timeout_ms);

/*
 * wsl_set_reconnect - 设置重连参数 (须在 wsl_start 前调用)
 *
 * init_ms: 首次重连最大等待时间 (ms)
 * max_ms:  最大重连等待时间 (ms)
 */
void wsl_set_reconnect(wsl_client_t *c, int init_ms, int max_ms);

/*
 * wsl_set_queue_limit - 设置发送队列上限 (须在 wsl_start 前调用)
 *
 * max_msgs:  最大消息条数, 0 表示不限制
 * max_bytes: 最大总字节数, 0 表示不限制
 */
void wsl_set_queue_limit(wsl_client_t *c, int max_msgs, int max_bytes);

/*
 * wsl_set_ssl - 启用/禁用 SSL/TLS (须在 wsl_start 前调用)
 *
 * enabled:      1 = 使用 WSS (加密), 0 = 使用 WS (明文)
 * skip_verify:  1 = 跳过证书验证 (自签名/测试环境), 0 = 严格验证 (生产环境)
 *
 * 注意: 跳过证书验证仅用于开发/测试环境, 生产环境应使用有效证书
 */
void wsl_set_ssl(wsl_client_t *c, int enabled, int skip_verify);

/*
 * wsl_set_path - 设置 WebSocket 连接路径 (须在 wsl_start 前调用)
 *
 * path: URL 路径, 如 "/ws", "/api/chat" 等, 默认为 "/"
 */
void wsl_set_path(wsl_client_t *c, const char *path);

/* ---- v2 新增: 生命周期回调 ------------------------------------ */

/*
 * wsl_set_conn_cb - 设置连接事件回调 (须在 wsl_start 前调用)
 *
 * connected=1 表示连接建立; connected=0 表示连接断开 (将触发重连)
 */
void wsl_set_conn_cb(wsl_client_t *c, wsl_conn_cb_t cb, void *user);

/*
 * wsl_set_err_cb - 设置错误回调 (须在 wsl_start 前调用)
 *
 * 当 CONNECTION_ERROR 发生时调用, 包含服务端返回的错误描述
 */
void wsl_set_err_cb(wsl_client_t *c, wsl_err_cb_t cb, void *user);

/* ---------------------------------------------------------------- */

/*
 * wsl_start - 启动 service 线程并发起首次连接
 *
 * 返回: 1 表示成功, 0 表示失败
 */
int wsl_start(wsl_client_t *c);

/*
 * wsl_stop - 优雅停止并等待 service 线程退出, 同时销毁 lws_context
 * 之后可安全调用 wsl_destroy() 释放内存
 */
void wsl_stop(wsl_client_t *c);

/*
 * wsl_destroy - 释放 wsl_client_t 及其所有内存
 * 必须在 wsl_stop() 之后调用, 否则 service 线程仍在运行, 行为未定义
 */
void wsl_destroy(wsl_client_t *c);

/*
 * wsl_send / wsl_send_binary - 线程安全发送
 * data 拷贝后立即返回, 不阻塞调用方
 * 连接断开期间消息仍会入队, 恢复连接后发送
 */
LwsClientRet_e wsl_send(wsl_client_t *c, const char *data, size_t len);
LwsClientRet_e wsl_send_binary(wsl_client_t *c, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
