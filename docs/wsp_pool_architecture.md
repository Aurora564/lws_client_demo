# wsp_pool — 1:N WebSocket 连接池文档

> 基于 libwebsockets 的共享 Context 连接池，单一服务线程管理最多 256 个并发 WebSocket 连接，适合 100+ 实例的大规模场景。

---

## 目录

- [1. 设计动机](#1-设计动机)
- [2. 线程架构模型](#2-线程架构模型)
  - [2.1 总体视图](#21-总体视图)
  - [2.2 与 wsl_client 的对比](#22-与-wsl_client-的对比)
  - [2.3 线程职责划分](#23-线程职责划分)
  - [2.4 线程安全边界](#24-线程安全边界)
- [3. 核心数据结构](#3-核心数据结构)
  - [3.1 wsp_pool_t](#31-wsp_pool_t)
  - [3.2 wsp_client_t](#32-wsp_client_t)
  - [3.3 对象关系图](#33-对象关系图)
- [4. 关键机制详解](#4-关键机制详解)
  - [4.1 用户数据绑定：两级指针](#41-用户数据绑定两级指针)
  - [4.2 跨线程唤醒：lws_cancel_service](#42-跨线程唤醒lws_cancel_service)
  - [4.3 重连定时器：lws_sorted_usec_list_t](#43-重连定时器lws_sorted_usec_list_t)
  - [4.4 Pending 队列：线程安全的动态加入](#44-pending-队列线程安全的动态加入)
  - [4.5 优雅停止：Condvar 同步](#45-优雅停止condvar-同步)
- [5. 状态机](#5-状态机)
- [6. 数据流图](#6-数据流图)
  - [6.1 发送路径](#61-发送路径)
  - [6.2 接收路径](#62-接收路径)
  - [6.3 重连路径](#63-重连路径)
  - [6.4 停止路径](#64-停止路径)
- [7. API 参考](#7-api-参考)
  - [7.1 连接池生命周期](#71-连接池生命周期)
  - [7.2 客户端创建与配置](#72-客户端创建与配置)
  - [7.3 连接池管理](#73-连接池管理)
  - [7.4 客户端停止与释放](#74-客户端停止与释放)
  - [7.5 发送接口](#75-发送接口)
- [8. 使用示例](#8-使用示例)
- [9. 性能特征与调优](#9-性能特征与调优)
- [附录：关键常量](#附录关键常量)

---

## 1. 设计动机

`wsl_client`（方案一）为每个连接创建独立的 `lws_context` 和 `pthread`：

```
方案一（wsl_client）：
  N 个连接 → N 个 lws_context → N 个 pthread
```

这在连接数超过 50 时会遇到明显瓶颈：

| 资源 | N=10 | N=100 | N=256 |
|------|------|-------|-------|
| 系统线程 | 10 | 100 | 256 |
| lws_context（各含 SSL 初始化、fd 集合） | 10 | 100 | 256 |
| 每线程默认栈 (~8 MB) | 80 MB | 800 MB | >2 GB |

`wsp_pool`（方案二）将全部连接收归一个 `lws_context` 和一个 service 线程：

```
方案二（wsp_pool）：
  N 个连接 → 1 个 lws_context → 1 个 pthread
             └─ lws 内部用 poll/epoll 多路复用 N 个 wsi
```

libwebsockets 的事件循环本身是 I/O 多路复用的：一次 `lws_service()` 调用可以同时处理所有活跃连接上的读写事件，单线程即可支撑数百个并发 WebSocket 会话。

---

## 2. 线程架构模型

### 2.1 总体视图

```
╔════════════════════════════════════════════════════════════════╗
║  应用层（可在任意线程调用）                                     ║
║                                                                ║
║  wsp_pool_create()  wsp_pool_start()  wsp_pool_destroy()      ║
║  wsp_client_create()  wsp_pool_add()  wsp_client_stop()       ║
║  wsp_send()  wsp_send_binary()                                 ║
╚══════════════════╤═════════════════════════════════════════════╝
                   │  mutex (pool_lock / q_lock / stop_mu)
                   │  lws_cancel_service()
                   ▼
╔════════════════════════════════════════════════════════════════╗
║  wsp_pool service 线程（单线程）                               ║
║                                                                ║
║  while (!pool->destroying) {                                   ║
║      lws_service(ctx, 50ms)   ← 内部 poll/epoll 所有 fd       ║
║  }                                                             ║
║                                                                ║
║  回调路由：ws_pool_callback()                                  ║
║  ┌───────────────────────────────────────────────────────┐    ║
║  │ WAIT_CANCELLED → 消费 pending[] → do_connect()        │    ║
║  │                → 遍历 clients[]  → 请求 WRITABLE       │    ║
║  │ ESTABLISHED    → 重置重连参数、启动心跳定时器           │    ║
║  │ RECEIVE        → 分片组装 → rx_cb()                    │    ║
║  │ WRITEABLE      → 出队 → lws_write()                    │    ║
║  │ TIMER          → 心跳逻辑（Ping / Pong 超时）           │    ║
║  │ CLOSED         → sul 重连 / condvar signal             │    ║
║  └───────────────────────────────────────────────────────┘    ║
╚══════════════════╤═════════════════════════════════════════════╝
                   │
╔══════════════════▼═════════════════════════════════════════════╗
║  lws_context（单实例，管理全部 wsi）                           ║
║                                                                ║
║  wsi[0] ─── wsp_client_t[0]   (lws_wsi_user)                  ║
║  wsi[1] ─── wsp_client_t[1]                                    ║
║  wsi[2] ─── wsp_client_t[2]                                    ║
║  ...                                                           ║
║  wsi[N] ─── wsp_client_t[N]                                    ║
╚════════════════════════════════════════════════════════════════╝
```

### 2.2 与 wsl_client 的对比

| 维度 | wsl_client（方案一）| wsp_pool（方案二）|
|------|-------------------|--------------------|
| lws_context 数量 | N 个（每连接一个）| 1 个（全局共享）|
| service 线程数 | N 个 | 1 个 |
| per-连接状态获取 | `lws_context_user()` | `lws_wsi_user()` |
| 重连定时器 | `clock_gettime` 轮询 | `lws_sorted_usec_list_t`（lws 内置）|
| 动态加入新连接 | 不支持（需重启）| `wsp_pool_add()` 随时可用 |
| 单个连接停止 | 需停止整个线程 | `wsp_client_stop()` 独立停止 |
| 适用规模 | < 20 个连接 | 100~256 个连接 |
| 内存占用（N=100）| ~800 MB 线程栈 + 100×ctx | ~8 MB 线程栈 + 1×ctx |

### 2.3 线程职责划分

```
应用线程（可多个）              service 线程（唯一）
─────────────────              ─────────────────────
wsp_pool_create()              lws_service() 事件循环
wsp_pool_start()               ws_pool_callback() 分发
wsp_pool_add()   ─ pool_lock → 写入 pending[]
                               ← WAIT_CANCELLED 消费
wsp_send()       ─ q_lock   → 写入 client->q_head
                               ← WRITEABLE 出队发送
wsp_client_stop()─ stop_mu  → 设置 stopping=1
                               ← CLOSED signal condvar
pthread_join(stop_cond)
wsp_client_destroy()
wsp_pool_destroy()
```

### 2.4 线程安全边界

| 数据 | 保护机制 | 写入方 | 读取方 |
|------|---------|--------|--------|
| `pool->clients[]`、`n_clients` | `pool_lock` | 应用线程（`wsp_pool_add`）| service 线程（`WAIT_CANCELLED`）|
| `pool->pending[]`、`n_pending` | `pool_lock` | 应用线程（`wsp_pool_add`）| service 线程（`WAIT_CANCELLED`）|
| `client->q_head/tail/msgs/bytes` | `client->q_lock` | 应用线程（`wsp_send`）| service 线程（`WRITEABLE`）|
| `client->stopped` | `client->stop_mu` + condvar | service 线程（`CLOSED`）| 应用线程（`wsp_client_stop`）|
| `client->stopping` | `client->stop_mu` | 应用线程 | service 线程 |
| `client->wsi`、`state`、`sul`、心跳标志 | 无锁（仅 service 线程访问）| service 线程 | service 线程 |
| `pool->destroying` | 写后 `lws_cancel_service` 隐含 barrier | `wsp_pool_destroy` | service 线程循环条件 |

---

## 3. 核心数据结构

### 3.1 wsp_pool_t

```c
struct wsp_pool {
    /* 已注册客户端（pool_lock 保护）*/
    pthread_mutex_t  pool_lock;
    wsp_client_t    *clients[WSP_MAX_CLIENTS];  // 活跃客户端数组
    int              n_clients;                  // 当前客户端数量

    /* 待加入队列（pool_lock 保护，service 线程消费）*/
    wsp_client_t    *pending[WSP_MAX_CLIENTS];  // 等待首次连接的客户端
    int              n_pending;

    /* LWS */
    struct lws_context   *ctx;                  // 全局唯一 context
    struct lws_protocols  protocols[2];          // [0]=业务，[1]=哨兵

    /* Service 线程 */
    pthread_t  thread;
    int        thread_started;
    int        destroying;                       // 1 = 通知 service 线程退出
};
```

`pending[]` 的存在是因为 `do_connect()` 必须在 service 线程内调用（lws 不允许跨线程操作 wsi），而 `wsp_pool_add()` 可在任意线程调用。

### 3.2 wsp_client_t

```c
struct wsp_client {
    /* 配置（wsp_pool_add 前只读）*/
    char        *host;
    int          port;
    char        *proto_name;
    char        *path;
    wsl_rx_cb_t  rx_cb;
    void        *user;
    int          use_ssl;
    int          ssl_skip_verify;
    int          ping_interval_ms;
    int          pong_timeout_ms;
    int          reconnect_init_ms;
    int          reconnect_max_ms;
    int          reconnect_delay_ms;    // 当前退避间隔（指数增长）
    size_t       max_queue_msgs;
    size_t       max_queue_bytes;

    /* LWS（仅 service 线程访问）*/
    struct lws     *wsi;
    client_state_t  state;
    int             send_ping;
    int             ping_pending;

    /* lws 内置定时器（用于重连退避）*/
    lws_sorted_usec_list_t sul;         // 替代 clock_gettime 轮询

    /* 发送队列（q_lock 保护）*/
    pthread_mutex_t  q_lock;
    msg_node_t      *q_head;
    msg_node_t      *q_tail;
    size_t           q_msgs;
    size_t           q_bytes;

    /* 分片缓冲（仅 service 线程访问）*/
    unsigned char   *frag_buf;
    size_t           frag_len;
    size_t           frag_cap;

    /* 停止同步 */
    pthread_mutex_t  stop_mu;
    pthread_cond_t   stop_cond;
    int              stopping;          // 应用线程写，service 线程读
    int              stopped;           // CLOSED 回调置 1，condvar signal

    /* 反向指针 */
    struct wsp_pool *pool;              // 所属连接池
};
```

### 3.3 对象关系图

```
wsp_pool_t
├── ctx ────────────────────────────────────────────→ lws_context
│    └── lws_context_user(ctx) == pool              （WAIT_CANCELLED 中取 pool）
│
├── clients[0] ──→ wsp_client_t[0]
│    │              └── wsi ──→ lws
│    │                   └── lws_wsi_user(wsi) == &clients[0]
│    │
│    ├── clients[1] ──→ wsp_client_t[1]
│    │              └── wsi ──→ lws
│    │
│    └── clients[N] ──→ wsp_client_t[N]
│
└── pending[0..M]   （尚未首次连接的客户端，WAIT_CANCELLED 后消费）
```

**两级用户数据绑定：**

- `lws_context_user(ctx)` → `wsp_pool_t*`：用于 `WAIT_CANCELLED` 中遍历所有客户端
- `lws_wsi_user(wsi)` → `wsp_client_t*`：用于其余所有回调的 per-connection 分发

这是与 `wsl_client` 最核心的区别：后者只用 context_user，因为一个 context 只有一个连接；前者需要 wsi_user 来区分同一个 context 下的 N 个连接。

---

## 4. 关键机制详解

### 4.1 用户数据绑定：两级指针

```c
/* pool 级别：创建 context 时绑定 */
info.user = pool;
pool->ctx = lws_create_context(&info);
// 取法：lws_context_user(lws_get_context(wsi))

/* client 级别：发起连接时通过 userdata 字段绑定 */
info.userdata = c;
c->wsi = lws_client_connect_via_info(&info);
// 取法：lws_wsi_user(wsi)
```

回调函数中的分发逻辑：

```c
static int ws_pool_callback(struct lws *wsi, enum lws_callback_reasons reason,
                             void *user_data, void *in, size_t len)
{
    wsp_client_t *c = (wsp_client_t *)lws_wsi_user(wsi);  // per-connection

    switch (reason) {
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
        /* 这里 wsi 是"哑"wsi，不对应任何连接，需要 pool 级别指针 */
        wsp_pool_t *pool = lws_context_user(lws_get_context(wsi));
        // ...遍历 pool->clients[]
        break;
    }
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
    case LWS_CALLBACK_CLIENT_RECEIVE:
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        /* c 有效，直接操作 per-connection 状态 */
        break;
    }
}
```

### 4.2 跨线程唤醒：lws_cancel_service

`lws_service()` 在没有 I/O 事件时会阻塞在 `poll()/epoll_wait()` 上。外部线程通过 `lws_cancel_service()` 向 context 内部管道写一个字节，立即唤醒 service 线程，并触发 `LWS_CALLBACK_EVENT_WAIT_CANCELLED`。

```
应用线程                          service 线程
──────────                        ─────────────
wsp_send()
  pthread_mutex_lock(q_lock)
  入队
  pthread_mutex_unlock(q_lock)
  lws_cancel_service(ctx) ──────→ 内核管道字节
                                   poll() 返回
                                   lws_service() 内部处理
                                   LWS_CALLBACK_EVENT_WAIT_CANCELLED
                                     → 遍历 clients[]
                                     → lws_callback_on_writable(wsi)
                                   LWS_CALLBACK_CLIENT_WRITEABLE
                                     → 出队 → lws_write()
```

`lws_cancel_service` 是 wsp_pool 中跨线程通信的**唯一**机制，所有需要通知 service 线程的操作（发送数据、添加新连接、停止单个客户端）都通过它触发。

### 4.3 重连定时器：lws_sorted_usec_list_t

方案一（`wsl_client`）用 `clock_gettime` 在每次 `lws_service` 后轮询检查是否到重连时间，每个线程都要做这个检查：

```c
/* wsl_client 方式（不适合 1:N）*/
while (!c->stopping) {
    if (c->state == STATE_RECONNECT_WAIT && ms_now() >= reconnect_at)
        do_connect(c);
    lws_service(c->ctx, 50ms);
}
```

方案二使用 lws 内置的有序微秒定时器链表（`lws_sorted_usec_list_t`），完全在 service 线程内驱动，无需额外轮询：

```c
/* wsp_pool 方式 */
static void schedule_reconnect(wsp_client_t *c)
{
    lws_sul_schedule(c->pool->ctx, 0, &c->sul,
                     sul_reconnect_cb,
                     (lws_usec_t)c->reconnect_delay_ms * 1000);
    // 退避翻倍...
}

static void sul_reconnect_cb(lws_sorted_usec_list_t *sul)
{
    wsp_client_t *c = lws_container_of(sul, wsp_client_t, sul);
    do_connect(c);
}
```

`lws_sorted_usec_list_t` 的实现是一个按触发时间排序的侵入式链表，lws 事件循环在每次 `lws_service()` 时检查链表头，到期则调用回调。N 个客户端的重连定时器合并到一个链表中，service 线程只需 O(1) 检查链表头，与连接数无关。

```
lws_sorted_usec_list（最小堆/有序链表）：
  ┌─ client[2].sul  到期时间: now+1s   ← 最近
  ├─ client[7].sul  到期时间: now+3s
  ├─ client[0].sul  到期时间: now+8s
  └─ client[5].sul  到期时间: now+60s
```

### 4.4 Pending 队列：线程安全的动态加入

`wsp_pool_add()` 可在 service 线程启动前后的任意时刻调用。新客户端首先写入 `pool->pending[]`，service 线程在下一次 `WAIT_CANCELLED` 中消费：

```
应用线程                              service 线程
──────────                            ─────────────
wsp_pool_add(pool, client)
  lock(pool_lock)
  clients[n++] = client              （WAIT_CANCELLED 触发）
  pending[m++] = client
  unlock(pool_lock)
  lws_cancel_service(ctx) ─────────→ WAIT_CANCELLED
                                       lock(pool_lock)
                                       取出 pending[]，清空
                                       unlock(pool_lock)
                                       do_connect(client[0])
                                       do_connect(client[1])
                                       ...
```

`clients[]` 和 `pending[]` 是独立的数组：前者是永久注册列表（用于 `WAIT_CANCELLED` 发送轮询和 `wsp_pool_destroy` 清理），后者是一次性消费队列（只用于触发首次连接）。

### 4.5 优雅停止：Condvar 同步

`wsp_client_stop()` 需要等待 service 线程实际关闭连接后才能返回，防止调用方过早释放内存。

```
应用线程                              service 线程
──────────                            ─────────────
wsp_client_stop(c)
  lock(stop_mu)
  c->stopping = 1
  unlock(stop_mu)
  lws_sul_cancel(c->sul)             （取消未触发的重连定时器）
  if (c->wsi != NULL):
    lws_cancel_service(ctx) ────────→ WAIT_CANCELLED
                                        （检测 stopping，
                                         lws 会在 service 内
                                         关闭 wsi）
                                      LWS_CALLBACK_CLIENT_CLOSED
                                        lock(stop_mu)
                                        c->stopped = 1
                                        pthread_cond_signal(stop_cond)
                                        unlock(stop_mu)
  lock(stop_mu)
  while (!c->stopped)
    pthread_cond_wait(stop_cond, stop_mu)
  unlock(stop_mu)
  （返回，此时 wsi 已关闭）
```

若 `wsi` 为 NULL（连接断开、重连等待中），则直接设置 `stopped=1` 跳过 condvar 等待。

---

## 5. 状态机

```c
typedef enum {
    STATE_IDLE,            // 初始状态（刚创建，未加入池）
    STATE_CONNECTING,      // 连接握手中
    STATE_CONNECTED,       // 已建立，正常通信
    STATE_RECONNECT_WAIT,  // 等待 sul 定时器到期后重连
    STATE_STOPPED,         // wsp_client_stop() 已调用，不再重连
} client_state_t;
```

```
wsp_pool_add()
      │
   STATE_IDLE ──────────── do_connect() ──────────────→ STATE_CONNECTING
                                                               │
                                                    LWS_CALLBACK_CLIENT_ESTABLISHED
                                                               │
                                                               ▼
                                                        STATE_CONNECTED
                                                               │
                           ┌───────────────────────────────────┤
                           │ 连接断开 / Pong 超时              │
                     CLOSED / CONNECTION_ERROR                  │
                           │                                    │
                           ▼                                    │
                   STATE_RECONNECT_WAIT                         │
                    lws_sul_schedule()                          │
                           │                                    │
                    sul_reconnect_cb()                          │
                           │                                    │
                           └────── do_connect() ────────────────┘

   wsp_client_stop()
      │
      └─────────────────────────────────────────────→ STATE_STOPPED
                                                      （停止重连，condvar signal）
```

---

## 6. 数据流图

### 6.1 发送路径

```
任意线程
  wsp_send(client, data, len)
       │
       ├─ malloc msg_node（LWS_PRE + payload 连续内存）
       ├─ lock(q_lock) → 检查队列上限 → 入队尾 → unlock
       └─ lws_cancel_service(pool->ctx)
                │
════════════════╪═══════ 线程边界 ════════════════════
                │
       service 线程
       LWS_CALLBACK_EVENT_WAIT_CANCELLED
            │
            └─ 遍历 clients[]
                 ├─ client[i].state == CONNECTED?
                 ├─ lock(q_lock) → q_head != NULL? → unlock
                 └─ lws_callback_on_writable(client[i].wsi)
                          │
               LWS_CALLBACK_CLIENT_WRITEABLE
                          │
                          ├─ send_ping? → lws_write(PING) → break
                          ├─ lock(q_lock) → 取队头 → unlock
                          ├─ lws_write(buf+LWS_PRE, len, TEXT/BINARY)
                          ├─ free(node)
                          └─ 队列仍有数据? → lws_callback_on_writable()
```

### 6.2 接收路径

```
网络数据到达（TCP 分包透明处理）
       │
LWS_CALLBACK_CLIENT_RECEIVE
  c = lws_wsi_user(wsi)
       │
       ├─ lws_is_first_fragment()
       ├─ lws_is_final_fragment()
       └─ lws_remaining_packet_payload()
              │
    ┌─────────┴─────────┐
    │ 单帧完整消息        │ 多帧分片
    ↓                   ↓
rx_cb(in, len, user)  frag_append() → 动态扩容缓冲
                            │
                    最后一帧且 remaining==0?
                            │
                       rx_cb(frag_buf, frag_len, user)
                       frag_len = 0（缓冲复用）
```

### 6.3 重连路径

```
LWS_CALLBACK_CLIENT_CLOSED 或 CLIENT_CONNECTION_ERROR
  c = lws_wsi_user(wsi)
       │
       ├─ c->stopping? → lock(stop_mu) → stopped=1 → signal → unlock
       └─ !stopping → schedule_reconnect(c)
                           │
                    lws_sul_schedule(ctx, 0, &c->sul,
                                     sul_reconnect_cb,
                                     delay_us)
                    reconnect_delay_ms *= 2（指数退避）
                    state = STATE_RECONNECT_WAIT
                           │
                    （delay 毫秒后，service 线程内触发）
                           │
                    sul_reconnect_cb(&c->sul)
                    lws_container_of → c
                           │
                    do_connect(c)
                    state = STATE_CONNECTING
                    lws_client_connect_via_info()
```

### 6.4 停止路径

```
应用线程                              service 线程
wsp_client_stop(c)
  stopping = 1
  lws_sul_cancel(&c->sul)
  lws_cancel_service(ctx) ─────────→ WAIT_CANCELLED
                                       （检测到 stopping，
                                        lws 不再 WRITEABLE）
                                      CLIENT_CLOSED
                                        stopped = 1
                                        pthread_cond_signal
  ← pthread_cond_wait ────────────────
  返回（wsi 已关闭）
  flush_queue()

wsp_client_destroy(c)
  free resources
```

---

## 7. API 参考

### 7.1 连接池生命周期

#### wsp_pool_create()

```c
wsp_pool_t *wsp_pool_create(void);
```

分配并初始化连接池结构体，不创建 `lws_context`。返回 `NULL` 表示内存不足。

#### wsp_pool_start()

```c
int wsp_pool_start(wsp_pool_t *pool);
```

创建 `lws_context` 并启动 service 线程。若调用前已通过 `wsp_pool_add()` 加入客户端，start 后会立即触发首次连接。返回 `1` 成功，`0` 失败。

#### wsp_pool_destroy()

```c
void wsp_pool_destroy(wsp_pool_t *pool);
```

按以下顺序销毁连接池：

1. 对所有已注册客户端调用 `wsp_client_stop()`（阻塞等待各连接关闭）
2. 设置 `destroying=1`，`lws_cancel_service()` 唤醒 service 线程
3. `pthread_join()` 等待 service 线程退出
4. `lws_context_destroy()` 销毁 context
5. 对所有客户端调用 `wsp_client_destroy()` 释放内存
6. 释放 pool 结构体

调用后不得再访问 pool 或其客户端指针。

### 7.2 客户端创建与配置

#### wsp_client_create()

```c
wsp_client_t *wsp_client_create(const char *host, int port,
                                  const char *protocol,
                                  wsl_rx_cb_t rx_cb, void *user);
```

创建客户端实例，不发起连接。默认值与 `wsl_client` 相同（心跳 30s/10s，重连 1s~60s，队列无上限）。

#### 配置函数（须在 wsp_pool_add 前调用）

```c
void wsp_set_ping(wsp_client_t *c, int interval_ms, int pong_timeout_ms);
void wsp_set_reconnect(wsp_client_t *c, int init_ms, int max_ms);
void wsp_set_queue_limit(wsp_client_t *c, int max_msgs, int max_bytes);
void wsp_set_ssl(wsp_client_t *c, int enabled, int skip_verify);
void wsp_set_path(wsp_client_t *c, const char *path);
```

配置语义与 `wsl_client` 对应函数完全一致，参见 `wsl_client_architecture.md` 第 4.6 节。

### 7.3 连接池管理

#### wsp_pool_add()

```c
LwsClientRet_e wsp_pool_add(wsp_pool_t *pool, wsp_client_t *c);
```

将客户端注册到连接池并发起连接（线程安全）。可在 `wsp_pool_start()` 前后任意时刻调用。

| 返回值 | 含义 |
|--------|------|
| `LWS_OK` | 成功注册，连接将在 service 线程中发起 |
| `LWS_ERR_PARAM` | `pool` 或 `c` 为 NULL，或池已满（n_clients + n_pending ≥ 256），或客户端已属于某个池 |

### 7.4 客户端停止与释放

#### wsp_client_stop()

```c
void wsp_client_stop(wsp_client_t *c);
```

优雅关闭单个连接，**阻塞**直到连接实际关闭。关闭后该客户端不再触发自动重连。仍可调用 `wsp_send()`，但消息会因 `stopping=1` 被立即丢弃。

#### wsp_client_destroy()

```c
void wsp_client_destroy(wsp_client_t *c);
```

释放客户端所有内存（字符串、缓冲区、互斥锁等）。必须在 `wsp_client_stop()` 之后调用。

> `wsp_pool_destroy()` 已自动对所有客户端调用这两个函数，无需重复调用。

### 7.5 发送接口

```c
LwsClientRet_e wsp_send(wsp_client_t *c, const char *data, size_t len);
LwsClientRet_e wsp_send_binary(wsp_client_t *c, const void *data, size_t len);
```

线程安全，可在任意线程调用。`data` 在函数内部复制，调用返回后可安全修改原缓冲区。连接断开期间消息入队，重连成功后发送。

| 返回值 | 含义 |
|--------|------|
| `LWS_OK` | 成功入队 |
| `LWS_ERR_PARAM` | 参数为 NULL，或客户端已 stopping |
| `LWS_ERR_QUEUE_FULL` | 超出 `max_msgs` 或 `max_bytes` 限制 |

---

## 8. 使用示例

### 基本用法

```c
#include "ws_pool.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void on_message(const char *data, size_t len, void *user)
{
    int id = *(int *)user;
    printf("[conn %d] %.*s\n", id, (int)len, data);
}

int main(void)
{
    /* 1. 创建并启动连接池 */
    wsp_pool_t *pool = wsp_pool_create();
    wsp_pool_start(pool);

    /* 2. 批量创建客户端 */
    static int ids[5] = {0, 1, 2, 3, 4};
    wsp_client_t *clients[5];

    for (int i = 0; i < 5; i++) {
        clients[i] = wsp_client_create(
            "echo.example.com", 80, NULL, on_message, &ids[i]);
        wsp_set_reconnect(clients[i], 1000, 30000);
        wsp_pool_add(pool, clients[i]);
    }

    /* 3. 任意线程发送 */
    sleep(1);
    for (int i = 0; i < 5; i++) {
        char msg[32];
        snprintf(msg, sizeof(msg), "hello from conn %d", i);
        wsp_send(clients[i], msg, strlen(msg));
    }

    sleep(5);

    /* 4. 销毁（自动停止所有客户端）*/
    wsp_pool_destroy(pool);
    return 0;
}
```

### 动态加入（pool start 后再 add）

```c
wsp_pool_t *pool = wsp_pool_create();
wsp_pool_start(pool);

/* 之后的任意时刻，从任意线程 */
wsp_client_t *late = wsp_client_create("new-server.com", 8080,
                                         "my-proto", rx_cb, NULL);
wsp_pool_add(pool, late);  /* 线程安全，立即触发连接 */
```

### 停止单个连接而不影响其他

```c
/* 只停一个 */
wsp_client_stop(clients[2]);      /* 阻塞直到连接关闭 */
wsp_client_destroy(clients[2]);   /* 释放内存 */
clients[2] = NULL;

/* 其余连接不受影响，pool 继续运行 */
wsp_send(clients[0], "still alive", 11);
```

### 大规模连接（100 个）

```c
#define N 100
wsp_pool_t *pool = wsp_pool_create();
wsp_pool_start(pool);

wsp_client_t *conns[N];
static int ids[N];
for (int i = 0; i < N; i++) {
    ids[i] = i;
    conns[i] = wsp_client_create("asr.internal", 8080,
                                   "asr-v2", on_asr_result, &ids[i]);
    wsp_set_ping(conns[i], 15000, 5000);      /* 心跳更频繁 */
    wsp_set_queue_limit(conns[i], 50, 512*1024);
    wsp_pool_add(pool, conns[i]);
}

/* 1 个线程 + 1 个 context 管理 100 个连接 */
```

---

## 9. 性能特征与调优

### 线程与内存开销

| 规模 | 线程数 | lws_context | 估算内存（不含业务数据）|
|------|--------|-------------|----------------------|
| 10 连接 | 1 | 1 | ~12 MB |
| 100 连接 | 1 | 1 | ~14 MB |
| 256 连接 | 1 | 1 | ~18 MB |

相比 `wsl_client`（N=100 时约 800 MB 线程栈），内存降低约 98%。

### SERVICE_POLL_MS 的影响

`lws_service(ctx, 50ms)` 的第二参数是无事件时最长阻塞时间。减小此值可降低延迟（接近 0 时退化为忙等），增大此值可降低 CPU 使用：

```c
/* 低延迟场景（如实时音频流）*/
#define SERVICE_POLL_MS 5

/* 普通场景（默认）*/
#define SERVICE_POLL_MS 50

/* 低频消息场景（降低 CPU）*/
#define SERVICE_POLL_MS 200
```

实际上，`lws_cancel_service()` 会立即唤醒 service 线程，poll 超时只影响没有外部触发时的最大响应延迟（如 sul 定时器精度）。

### 发送队列背压

```c
/* 生产者消费者场景：限制每个连接的队列，防止内存无限增长 */
wsp_set_queue_limit(c, 200, 2 * 1024 * 1024);  /* 200条 / 2MB */

/* 应用层处理背压 */
if (wsp_send(c, data, len) == LWS_ERR_QUEUE_FULL) {
    /* 丢弃、降频、或等待 */
}
```

### rx_cb 的注意事项

`rx_cb` 在 service 线程中调用，N 个连接共享这一个线程。回调必须尽快返回：

```c
/* 正确：仅拷贝数据，提交到业务队列 */
void on_message(const char *data, size_t len, void *user)
{
    SessionCtx *s = user;
    pthread_mutex_lock(&s->mu);
    ring_buffer_push(&s->rb, data, len);
    pthread_mutex_unlock(&s->mu);
    sem_post(&s->sem);
}

/* 错误：在 service 线程内做耗时解析 */
void on_message(const char *data, size_t len, void *user)
{
    json_parse(data, len);   /* 阻塞 service 线程，影响所有连接！ */
}
```

---

## 附录：关键常量

```c
/* 连接池上限 */
#define WSP_MAX_CLIENTS      256

/* 默认配置（可通过 wsp_set_* 修改）*/
#define DEFAULT_PING_INTERVAL_MS   30000   // Ping 间隔：30 秒
#define DEFAULT_PONG_TIMEOUT_MS    10000   // Pong 超时：10 秒
#define DEFAULT_RECONNECT_INIT_MS  1000    // 初始重连间隔：1 秒
#define DEFAULT_RECONNECT_MAX_MS   60000   // 最大重连间隔：60 秒

/* service 线程轮询间隔 */
#define SERVICE_POLL_MS            50      // 无事件时最长阻塞 50ms

/* 分片缓冲初始容量（不足时 2 倍扩容）*/
#define FRAG_INIT_CAP              4096    // 4 KB
```

---

**文档版本：** 1.0  
**对应源文件：** `src/ws_pool.h`、`src/ws_pool.c`  
**最后更新：** 2026-04-22
