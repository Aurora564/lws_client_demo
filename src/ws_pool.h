/**
 * ws_pool.h - 方案二: 共享 lws_context 的 1:N 连接池
 *
 * 线程模型:
 *      - 一个 lws_context + 一个 service 线程管理 N 个 WebSocket 连接
 *      - 任意线程调用 wsp_send() -> 加锁入队 -> lws_cancel_service() 唤醒
 *      - Service 线程在 LWS_CALLBACK_EVENT_WAIT_CANCELLED 内遍历所有有数据的 wsi
 *
 * 回调分发:
 *      - 通过 lws_wsi_user(wsi) 获取每个连接的 wsp_client_t 指针
 *      - lws_context_user() 保留给 pool 指针 (用于 WAIT_CANCELLED 事件)
 *
 * 重连模型:
 *      - 基于 lws_sorted_usec_list_t (lws 内置定时器), 无需额外 clock 轮询
 *      - lws_sul_schedule() 在 service 线程内安全触发重连
 *
 * 停止同步:
 *      - wsp_client_stop() 设置 stopping 标志 + lws_cancel_service()
 *      - Service 线程在 CANCELLED 内关闭 wsi, CLOSED 回调中通过 condvar 通知
 *
 * 线程安全的 wsp_pool_add():
 *      - 新客户端放入 pending 队列, 由 service 线程在下次循环时调用 do_connect
 *
 * 嵌入方式:
 *      - 将 ws_pool.{h,c} 复制到项目, 链接 libwebsockets + pthread
 */

#pragma once
#include "ws_client.h"   /* LwsClientRet_e, wsld_rx_cb_t */
#include "ws_internal.h" /* wsl_event_hooks_t */

#ifdef __cplusplus
extern "C" {
#endif

#define WSP_MAX_CLIENTS 256

/* 停止等待超时: wsp_client_stop 等待 CLOSED 回调的最大秒数.
 * 若服务端因 TCP buffer 残留数据导致 close 帧处理延迟,
 * 超时后可防止 condvar 永久挂死. */
#define WSP_STOP_TIMEOUT_SEC 3

typedef struct wsp_pool   wsp_pool_t;
typedef struct wsp_client wsp_client_t;

/* ---- 连接池生命周期 ------------------------------------------- */

/*
 * wsp_pool_create - 分配并初始化连接池 (不创建 lws_context)
 * 返回 NULL 表示内存分配失败
 */
wsp_pool_t *wsp_pool_create(void);

/*
 * wsp_pool_start - 创建 lws_context 并启动 service 线程
 * 可在 start 前或后调用 wsp_pool_add(); 均能正确处理
 * 返回 1 成功, 0 失败
 */
int wsp_pool_start(wsp_pool_t *pool);

/*
 * wsp_pool_destroy - 停止所有客户端, 销毁 lws_context, 释放池内存
 * 会调用 wsp_client_stop() + wsp_client_destroy() 处理全部已注册客户端
 * 调用后不得再访问 pool 或其客户端指针
 */
void wsp_pool_destroy(wsp_pool_t *pool);

/* ---- 客户端创建与配置 ----------------------------------------- */

/*
 * wsp_client_create - 创建客户端实例 (不发起连接)
 *
 * host:     服务端地址
 * port:     服务端端口
 * protocol: WebSocket 子协议名 (与服务端一致, 可为 NULL)
 * rx_cb:    收到完整消息时的回调, 在 service 线程中调用
 * user:     透传给 rx_cb 的用户指针
 *
 * 返回 NULL 表示内存分配失败.
 * 默认值: 心跳 30s/10s, 重连 1s~60s 指数退避, 队列无上限
 */
wsp_client_t *wsp_client_create(const char *host, int port,
                                  const char *protocol,
                                  wsld_rx_cb_t rx_cb, void *user);

/* 以下 set 函数须在 wsp_pool_add() 前调用 */
void wsp_set_ping(wsp_client_t *c, int interval_ms, int pong_timeout_ms);
void wsp_set_reconnect(wsp_client_t *c, int init_ms, int max_ms);
void wsp_set_queue_limit(wsp_client_t *c, int max_msgs, int max_bytes);
void wsp_set_ssl(wsp_client_t *c, int enabled, int skip_verify);
void wsp_set_path(wsp_client_t *c, const char *path);

/*
 * wsp_set_event_hooks - 设置事件钩子表 (须在 wsp_pool_add 前调用)
 *
 * hooks:  事件钩子表, 为 NULL 或各字段为 NULL 表示不使用对应钩子.
 *         hooks 指向的内存必须在 wsp_pool_add 之后保持有效.
 * user:   透传给各钩子回调的用户指针
 */
void wsp_set_event_hooks(wsp_client_t *c,
                          const wsl_event_hooks_t *hooks,
                          void *user);

/* ---- 连接池管理 ----------------------------------------------- */

/*
 * wsp_pool_add - 将客户端注册到连接池并发起连接 (线程安全)
 * 可在 wsp_pool_start() 前后调用
 * 返回 LWS_ERR_PARAM 若池已满或客户端已属于某个池
 */
LwsClientRet_e wsp_pool_add(wsp_pool_t *pool, wsp_client_t *c);

/* ---- 客户端停止与释放 ----------------------------------------- */

/*
 * wsp_client_stop - 优雅关闭单个连接 (阻塞至连接实际关闭)
 * 停止后客户端不再重连; 仍可调用 wsp_send() 但消息会被丢弃
 */
void wsp_client_stop(wsp_client_t *c);

/*
 * wsp_client_destroy - 释放客户端内存
 * 必须在 wsp_client_stop() 之后调用
 * 注意: wsp_pool_destroy() 已自动调用此函数, 勿重复释放
 */
void wsp_client_destroy(wsp_client_t *c);

/* ---- 发送 (线程安全) ------------------------------------------ */

LwsClientRet_e wsp_send(wsp_client_t *c, const char *data, size_t len);
LwsClientRet_e wsp_send_binary(wsp_client_t *c, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
