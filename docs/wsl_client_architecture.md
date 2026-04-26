# wsl_client — WebSocket 客户端封装库文档

> 基于 libwebsockets 封装的轻量级 WebSocket 客户端库，提供自动重连、心跳保活、线程安全发送队列等特性。

---

## 目录

- [1. 概述](#1-概述)
- [2. 架构层次](#2-架构层次)
- [3. 核心数据结构](#3-核心数据结构)
- [4. API 参考](#4-api-参考)
  - [4.1 wsl_create()](#41-wsl_create)
  - [4.2 wsl_start()](#42-wsl_start)
  - [4.3 wsl_send() / wsl_send_binary()](#43-wsl_send--wsl_send_binary)
  - [4.4 wsl_stop()](#44-wsl_stop)
  - [4.5 wsl_destroy()](#45-wsl_destroy)
  - [4.6 配置函数](#46-配置函数)
- [5. 功能模块详解](#5-功能模块详解)
  - [5.1 连接管理](#51-连接管理)
  - [5.2 状态机](#52-状态机)
  - [5.3 心跳机制](#53-心跳机制)
  - [5.4 发送队列](#54-发送队列)
  - [5.5 消息分片](#55-消息分片)
  - [5.6 重连机制](#56-重连机制)
- [6. 线程模型](#6-线程模型)
- [7. 数据流图](#7-数据流图)
- [8. LWS 底层 API 详解](#8-lws-底层-api-详解)
- [9. 使用示例](#9-使用示例)
- [10. 性能与调试](#10-性能与调试)
- [11. 架构设计权衡：一客户端一上下文](#11-架构设计权衡一客户端一上下文)
- [附录：关键常量](#附录关键常量)

---

## 1. 概述

`wsl_client` 是对 libwebsockets（LWS）库的高级封装，屏蔽了 LWS 的回调机制、context 生命周期、跨线程通信等复杂细节，向应用层暴露简洁的 C 接口。

### 核心特性

| 特性 | 说明 |
|------|------|
| 自动重连 | 指数退避策略，可配置初始和最大间隔 |
| 心跳保活 | Ping/Pong 机制，超时自动断线重连 |
| 线程安全 | 支持跨线程安全发送消息 |
| 消息队列 | 异步发送，支持消息数和字节数背压控制 |
| 分片组装 | 自动处理 WebSocket 多帧消息 |
| SSL/TLS | 支持 WSS 加密连接，可配置证书验证策略 |

### 架构分层

```
┌─────────────────────────────────────┐
│         应用层（业务代码）            │
│   wsl_create / start / send / stop  │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│        封装层（wsl_client）           │
│  重连逻辑 · 心跳机制 · 发送队列       │
│  状态管理 · 分片组装 · 线程安全       │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│      LWS 核心层（libwebsockets）     │
│  Context/Vhost/WSI · 协议回调        │
│  事件循环 · SSL · 帧收发             │
└─────────────────────────────────────┘
```

---

## 2. 架构层次

### 层次职责

| 层次 | 组件 | 职责 |
|------|------|------|
| 应用层 | 用户代码 | 调用 wsl_client API，实现业务逻辑 |
| 封装层 | `struct wsl_client` | 封装 LWS 复杂性，提供高级功能 |
| 核心层 | libwebsockets | 底层 WebSocket 协议实现 |

### 对象层次映射

```
应用层对象                              LWS 核心对象
──────────────────                     ──────────────────
struct wsl_client {                    struct lws_context {
                                         - 管理所有 vhost
  struct lws_context *ctx;  ────────→   - 服务线程池
                                         - 事件循环
                                       }

  struct lws *wsi;  ─────────────→    struct lws {
                                         - socket 描述符
                                         - 连接状态
                                         - 读写缓冲
                                         - 定时器
                                       }

  struct lws_protocols protocols[2];  → 注册到 lws_context
}
```

---

## 3. 核心数据结构

### wsl_client 完整字段说明

```c
struct wsl_client {
    /* ── 配置参数（启动前有效）─────────────────── */
    char        *host;              // 服务器主机名或 IP
    int          port;              // 服务器端口
    char        *proto_name;        // WebSocket 子协议名称
    char        *path;              // WebSocket 路径（默认 "/"）
    wsl_rx_cb_t  rx_cb;             // 消息接收回调
    void        *user;              // 用户自定义数据（透传给回调）

    int          use_ssl;           // 是否启用 SSL（0/1）
    int          ssl_skip_verify;   // 是否跳过证书验证（0/1）

    int          ping_interval_ms;  // Ping 发送间隔（毫秒）
    int          pong_timeout_ms;   // 等待 Pong 的超时时间（毫秒）
    int          reconnect_init_ms; // 初始重连等待间隔（毫秒）
    int          reconnect_max_ms;  // 最大重连等待间隔（毫秒）
    size_t       max_queue_msgs;    // 发送队列最大消息数（0=无限）
    size_t       max_queue_bytes;   // 发送队列最大字节数（0=无限）

    /* ── LWS 核心对象 ──────────────────────────── */
    struct lws_context      *ctx;          // LWS Context
    struct lws              *wsi;          // 当前 WebSocket 连接
    struct lws_protocols     protocols[2]; // 协议描述符数组

    /* ── 线程管理 ───────────────────────────────── */
    pthread_t       thread;         // LWS 服务线程句柄
    int             thread_started; // 线程是否已启动
    volatile int    stopping;       // 停止信号（volatile 保证可见性）

    /* ── 运行时状态 ─────────────────────────────── */
    client_state_t  state;              // 当前连接状态
    int             reconnect_delay_ms; // 当前重连等待时长
    struct timespec reconnect_at;       // 下次重连的绝对时间点

    /* ── 心跳状态 ───────────────────────────────── */
    int send_ping;                  // 置 1 表示需要发送 Ping
    int ping_pending;               // 置 1 表示 Ping 已发出，等待 Pong

    /* ── 发送队列 ───────────────────────────────── */
    pthread_mutex_t     q_lock;     // 队列互斥锁
    msg_node_t         *q_head;     // 队列头节点
    msg_node_t         *q_tail;     // 队列尾节点
    size_t              q_msgs;     // 当前队列消息数
    size_t              q_bytes;    // 当前队列总字节数

    /* ── 分片缓冲 ───────────────────────────────── */
    unsigned char *frag_buf;        // 多帧消息组装缓冲区
    size_t         frag_len;        // 已写入的字节数
    size_t         frag_cap;        // 缓冲区当前容量
};
```

### 字段与 LWS API 对应关系

| wsl_client 字段 | 对应 LWS 概念 | 相关 LWS API |
|----------------|-------------|-------------|
| `ctx` | `struct lws_context` | `lws_create_context()`, `lws_context_destroy()` |
| `wsi` | `struct lws` | `lws_client_connect_via_info()`, `lws_write()` |
| `protocols` | `struct lws_protocols` | 传入 `lws_create_context()` |
| `host/port/path` | `lws_client_connect_info` | 填充后传入连接函数 |
| `use_ssl` | `ssl_connection` 标志位 | `LCCSCF_USE_SSL` 等 |
| `state` | 应用层状态机 | 由 LWS 回调驱动 |
| `send_ping/ping_pending` | 心跳状态 | `lws_write(PING)`, `lws_set_timer_usecs()` |
| `q_head/q_tail` | 发送队列 | `lws_callback_on_writable()` |
| `frag_buf` | 分片组装缓冲 | `lws_is_first/final_fragment()` |
| `thread` | LWS 服务线程 | `lws_service()` 运行于此线程 |

---

## 4. API 参考

### 4.1 wsl_create()

```c
wsl_client_t *wsl_create(const char *host, int port,
                         const char *protocol,
                         wsl_rx_cb_t rx_cb, void *user);
```

**作用：** 创建并初始化 WebSocket 客户端实例。只分配内存和设置默认配置，不建立网络连接。

#### 参数详解

**`host` — 服务器地址**

类型 `const char *`，必填。指定 WebSocket 服务器的主机名或 IP 地址。

```c
wsl_create("echo.websocket.org", ...); // 域名
wsl_create("192.168.1.100", ...);      // IPv4
wsl_create("::1", ...);               // IPv6
```

注意：值会被内部 `strdup` 复制，调用者可安全释放原始字符串；对于 WSS 连接，主机名应与 SSL 证书的 CN/SAN 匹配（除非跳过验证）。

---

**`port` — 服务器端口**

类型 `int`，必填。

| 端口 | 协议 | 说明 |
|------|------|------|
| 80 | ws:// | 标准 WebSocket |
| 443 | wss:// | 加密 WebSocket |
| 8080 | ws:// | 常见替代端口 |
| 8443 | wss:// | 常见加密替代端口 |

---

**`protocol` — WebSocket 子协议**

类型 `const char *`，可选（可传 `NULL`）。对应 HTTP 握手中的 `Sec-WebSocket-Protocol` 头部。服务器可能依据此字段选择处理逻辑。

```c
wsl_create("example.com", 80, "chat-v1", ...);    // 指定子协议
wsl_create("example.com", 80, NULL, ...);          // 不使用子协议
```

传 `NULL` 时内部转换为空字符串。常见子协议名：`mqtt`、`graphql-ws`、`soap`、`xmpp` 等。

---

**`rx_cb` — 消息接收回调**

类型 `wsl_rx_cb_t`，可选（可传 `NULL`）。收到服务器消息时在**服务线程**中被调用。

```c
typedef void (*wsl_rx_cb_t)(const char *data, size_t len, void *user);
```

| 回调参数 | 说明 |
|---------|------|
| `data` | 消息数据指针，**不保证以 `\0` 结尾** |
| `len` | 消息字节数 |
| `user` | 透传自 `wsl_create` 的 `user` 参数 |

```c
void on_message(const char *data, size_t len, void *user)
{
    // 注意：必须使用 len，不能用 strlen(data)
    printf("Received: %.*s\n", (int)len, data);
}
```

重要约束：回调在服务线程中执行，不得执行耗时操作；`data` 回调返回后可能失效，如需保存请自行复制。

---

**`user` — 用户自定义数据**

类型 `void *`，可选（可传 `NULL`）。由库透传给每次 `rx_cb` 调用，常用于传递业务上下文。

```c
typedef struct {
    int    message_count;
    time_t last_receive_time;
} AppContext;

AppContext ctx = {0};

wsl_client_t *client = wsl_create(
    "example.com", 80, "my-protocol", on_message, &ctx);

void on_message(const char *data, size_t len, void *user)
{
    AppContext *ctx = (AppContext *)user;
    ctx->message_count++;
    ctx->last_receive_time = time(NULL);
}
```

`user` 所指内存的生命周期由调用者负责，必须在 `wsl_destroy()` 完成后才可释放。

#### 返回值

成功返回 `wsl_client_t *`；`host` 为 `NULL`、`port <= 0` 或内存不足时返回 `NULL`。

#### 内部行为

`wsl_create()` 执行以下操作，**不会**创建 LWS context 或网络连接（由 `wsl_start()` 完成）：

1. 参数校验（`host` 非空，`port > 0`）
2. `calloc` 分配并清零 `wsl_client_t`
3. 初始化 `q_lock` 互斥锁
4. `strdup` 复制 `host`、`protocol`、`path`
5. 写入默认配置（见[附录](#附录关键常量)）
6. 注册 LWS 协议回调 `protocols[0]`

---

### 4.2 wsl_start()

```c
int wsl_start(wsl_client_t *c);
```

**作用：** 创建 LWS context，启动服务线程并发起首次连接。

**返回值：** `1` 成功，`0` 失败（context 创建失败或线程创建失败）。

```c
if (!wsl_start(client)) {
    fprintf(stderr, "Failed to start\n");
    wsl_destroy(client);
    return 1;
}
// 此后客户端在后台运行，自动处理连接和重连
```

---

### 4.3 wsl_send() / wsl_send_binary()

```c
LwsClientRet_e wsl_send(wsl_client_t *c, const char *data, size_t len);
LwsClientRet_e wsl_send_binary(wsl_client_t *c, const void *data, size_t len);
```

**作用：** 异步将消息加入发送队列。线程安全，可在任意线程调用。

**返回值：**

| 返回值 | 含义 |
|--------|------|
| `LWS_OK` | 成功入队 |
| `LWS_ERR_PARAM` | 参数错误（`c` 或 `data` 为 `NULL`） |
| `LWS_ERR_QUEUE_FULL` | 队列已达上限 |

```c
const char *msg = "Hello WebSocket";
if (wsl_send(client, msg, strlen(msg)) != LWS_OK) {
    fprintf(stderr, "Send failed\n");
}

// 发送二进制数据
unsigned char buf[] = {0x00, 0x01, 0x02};
wsl_send_binary(client, buf, sizeof(buf));
```

---

### 4.4 wsl_stop()

```c
void wsl_stop(wsl_client_t *c);
```

**作用：** 设置停止标志，唤醒并等待服务线程完全退出，销毁 LWS context。此函数**阻塞**直到服务线程退出。

---

### 4.5 wsl_destroy()

```c
void wsl_destroy(wsl_client_t *c);
```

**作用：** 释放客户端所有资源（字符串、缓冲区、互斥锁、结构体本身）。必须在 `wsl_stop()` 之后调用。

---

### 4.6 配置函数

所有配置函数必须在 `wsl_start()` 之前调用。

#### wsl_set_ping()

```c
void wsl_set_ping(wsl_client_t *c, int interval_ms, int pong_timeout_ms);
```

设置心跳参数。`interval_ms` 为 Ping 发送间隔，`pong_timeout_ms` 为等待 Pong 的超时时间，超时后关闭连接触发重连。

```c
wsl_set_ping(client, 30000, 10000); // 30s ping, 10s pong timeout（默认）
wsl_set_ping(client, 10000, 5000);  // 更高频率的心跳
```

#### wsl_set_reconnect()

```c
void wsl_set_reconnect(wsl_client_t *c, int init_ms, int max_ms);
```

设置指数退避重连参数。每次重连失败后间隔翻倍，最大不超过 `max_ms`。连接成功后重置为 `init_ms`。

```c
wsl_set_reconnect(client, 1000, 60000); // 1s 起步，最长 60s（默认）
wsl_set_reconnect(client, 500, 10000);  // 更激进的重连策略
```

#### wsl_set_ssl()

```c
void wsl_set_ssl(wsl_client_t *c, int enabled, int skip_verify);
```

启用 SSL/TLS（WSS）。`skip_verify=1` 跳过证书验证，**仅用于开发测试**，生产环境必须使用 `skip_verify=0`。

```c
wsl_set_ssl(client, 1, 0); // WSS，严格验证证书（生产推荐）
wsl_set_ssl(client, 1, 1); // WSS，跳过验证（仅测试）
```

#### wsl_set_path()

```c
void wsl_set_path(wsl_client_t *c, const char *path);
```

设置 WebSocket 连接路径，默认为 `"/"`。

```c
wsl_set_path(client, "/ws/chat");
wsl_set_path(client, "/api/v1/stream");
```

#### wsl_set_queue_limit()

```c
void wsl_set_queue_limit(wsl_client_t *c, size_t max_msgs, size_t max_bytes);
```

设置发送队列上限，达到上限后 `wsl_send()` 返回 `LWS_ERR_QUEUE_FULL`。传 `0` 表示不限制。

```c
wsl_set_queue_limit(client, 100, 1024 * 1024); // 最多 100 条或 1 MB
```

---

## 5. 功能模块详解

### 5.1 连接管理

#### 连接流程

```
wsl_start()
    │
    ├─ 创建 lws_context（info.user = wsl_client_t *）
    │
    └─ 启动 service_thread
           │
           ├─ do_connect()
           │     └─ lws_client_connect_via_info()
           │
           └─ LWS_CALLBACK_CLIENT_ESTABLISHED
                  └─ state = STATE_CONNECTED，启动心跳定时器
```

#### 连接参数映射

```c
static void do_connect(wsl_client_t *c)
{
    struct lws_client_connect_info info = {0};

    info.context  = c->ctx;
    info.address  = c->host;
    info.port     = c->port;
    info.path     = c->path;
    info.host     = c->host;
    info.origin   = c->host;
    info.protocol = c->proto_name;

    if (c->use_ssl) {
        info.ssl_connection = LCCSCF_USE_SSL;
        if (c->ssl_skip_verify)
            info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED
                                 | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK
                                 | LCCSCF_ALLOW_EXPIRED;
    }

    c->wsi = lws_client_connect_via_info(&info);
}
```

---

### 5.2 状态机

#### 状态定义

```c
typedef enum {
    STATE_IDLE,           // 初始状态（刚创建）
    STATE_CONNECTING,     // 连接建立中
    STATE_CONNECTED,      // 已连接，正常通信
    STATE_RECONNECT_WAIT, // 等待重连计时
} client_state_t;
```

#### 状态转换图

```
  wsl_start()
       │
  STATE_IDLE ──────────────────────────→ STATE_CONNECTING
                                               │
                              ┌────────────────┘
                              │  LWS_CALLBACK_CLIENT_ESTABLISHED
                              ↓
                        STATE_CONNECTED
                              │
               ┌──────────────┴──────────────┐
               │                             │ 连接断开 / 错误
               │                    LWS_CALLBACK_CLIENT_CLOSED
               │                    LWS_CALLBACK_CLIENT_CONNECTION_ERROR
               │                             │
               │                             ↓
               │                    STATE_RECONNECT_WAIT
               │                             │
               │                             │ 计时到期
               │                             ↓
               └──────────────────── STATE_CONNECTING
```

---

### 5.3 心跳机制

#### 流程概览

```
连接建立 → 启动定时器（ping_interval_ms）
    │
    └─ LWS_CALLBACK_TIMER 触发
           │
           ├─ ping_pending == 1 → Pong 超时 → 返回 -1 关闭连接
           │
           └─ ping_pending == 0 → send_ping = 1
                  │
                  └─ lws_callback_on_writable()
                         │
                         └─ LWS_CALLBACK_CLIENT_WRITEABLE
                                │
                                └─ lws_write(PING)
                                   ping_pending = 1
                                   启动超时定时器（pong_timeout_ms）
                                       │
                                       └─ LWS_CALLBACK_CLIENT_RECEIVE_PONG
                                              │
                                              └─ ping_pending = 0
                                                 重启心跳定时器
```

#### 关键代码片段

```c
// 定时器回调
case LWS_CALLBACK_TIMER:
    if (c->ping_pending) {
        fprintf(stderr, "[wsl] pong timeout, closing\n");
        return -1;                                   // 触发重连
    }
    c->send_ping = 1;
    lws_callback_on_writable(wsi);
    lws_set_timer_usecs(wsi, (lws_usec_t)c->pong_timeout_ms * 1000);
    break;

// 发送 Ping
case LWS_CALLBACK_CLIENT_WRITEABLE:
    if (c->send_ping) {
        unsigned char ping_buf[LWS_PRE + 4] = {0};
        lws_write(wsi, ping_buf + LWS_PRE, 0, LWS_WRITE_PING);
        c->send_ping    = 0;
        c->ping_pending = 1;
        break;
    }
    // ... 处理发送队列

// 收到 Pong
case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
    c->ping_pending = 0;
    lws_set_timer_usecs(wsi, (lws_usec_t)c->ping_interval_ms * 1000);
    break;
```

---

### 5.4 发送队列

#### 队列数据结构

```c
typedef struct msg_node {
    unsigned char   *buf;       // LWS_PRE + payload 的连续内存
    size_t           len;       // payload 字节数
    int              is_binary; // 1=二进制，0=文本
    struct msg_node *next;
} msg_node_t;
```

#### 跨线程发送流程

```
应用线程                             LWS 服务线程
──────────                          ────────────
wsl_send(data, len)
    │
    ├─ malloc msg_node
    ├─ 加锁入队（q_lock）
    └─ lws_cancel_service(ctx) ──→  中断 poll()
                                         │
                                    LWS_CALLBACK_EVENT_WAIT_CANCELLED
                                         │
                                    lws_callback_on_writable(wsi)
                                         │
                                    LWS_CALLBACK_CLIENT_WRITEABLE
                                         │
                                    加锁取队头 → lws_write()
                                         │
                                    如有更多消息 → 继续触发 WRITEABLE
```

#### 入队实现（核心逻辑）

```c
static LwsClientRet_e enqueue(wsl_client_t *c, const void *data,
                               size_t len, int is_binary)
{
    msg_node_t *node = malloc(sizeof(*node));
    node->buf = malloc(LWS_PRE + len);
    memcpy(node->buf + LWS_PRE, data, len);
    node->len = len;
    node->is_binary = is_binary;
    node->next = NULL;

    pthread_mutex_lock(&c->q_lock);

    // 背压检查
    if ((c->max_queue_msgs  && c->q_msgs >= c->max_queue_msgs) ||
        (c->max_queue_bytes && c->q_bytes + len > c->max_queue_bytes)) {
        pthread_mutex_unlock(&c->q_lock);
        free(node->buf); free(node);
        return LWS_ERR_QUEUE_FULL;
    }

    // 入队尾
    if (c->q_tail) c->q_tail->next = node;
    else           c->q_head = node;
    c->q_tail = node;
    c->q_msgs++;
    c->q_bytes += len;
    pthread_mutex_unlock(&c->q_lock);

    lws_cancel_service(c->ctx);   // 唤醒服务线程
    return LWS_OK;
}
```

---

### 5.5 消息分片

LWS 可能将大消息拆分为多个回调帧。`wsl_client` 通过动态扩容的 `frag_buf` 自动完成组装。

#### 处理逻辑

```c
case LWS_CALLBACK_CLIENT_RECEIVE: {
    int first     = lws_is_first_fragment(wsi);
    int final     = lws_is_final_fragment(wsi);
    size_t remain = lws_remaining_packet_payload(wsi);

    if (first && final && remain == 0) {
        // 单帧完整消息，直接交付
        if (c->rx_cb) c->rx_cb((const char *)in, len, c->user);
    } else {
        if (first) c->frag_len = 0;          // 重置缓冲

        frag_append(c, (const unsigned char *)in, len); // 追加

        if (final && remain == 0) {          // 最后一帧
            if (c->rx_cb)
                c->rx_cb((const char *)c->frag_buf, c->frag_len, c->user);
            c->frag_len = 0;
        }
    }
    break;
}
```

`frag_append()` 内部使用 `realloc` 按 2 倍扩容，初始容量为 `FRAG_INIT_CAP`（4 KB）。

---

### 5.6 重连机制

#### 指数退避策略

```
第 1 次失败：等待 1s
第 2 次失败：等待 2s
第 3 次失败：等待 4s
第 4 次失败：等待 8s
  ...
第 N 次失败：等待 min(2^(N-1) × init_ms, max_ms)
连接成功后：重置为 init_ms
```

#### 实现

```c
static void schedule_reconnect(wsl_client_t *c)
{
    long at = ms_now() + c->reconnect_delay_ms;
    c->reconnect_at.tv_sec  = at / 1000L;
    c->reconnect_at.tv_nsec = (at % 1000L) * 1000000L;

    c->reconnect_delay_ms *= 2;
    if (c->reconnect_delay_ms > c->reconnect_max_ms)
        c->reconnect_delay_ms = c->reconnect_max_ms;

    c->state = STATE_RECONNECT_WAIT;
}
```

---

## 6. 线程模型

### 双线程架构

```
主线程（调用方）                    LWS 服务线程
──────────────                     ─────────────
wsl_create()
wsl_set_*()
wsl_start() ──── pthread_create ──→ service_thread()
                                        │
wsl_send()  ──── lws_cancel_service → 中断 poll()
                                        │
                                    while (!stopping) {
                                        检查重连计时
                                        lws_service(ctx, 50ms)
                                        ├─ ESTABLISHED → rx_cb()
                                        ├─ RECEIVE     → rx_cb()
                                        ├─ WRITEABLE   → lws_write()
                                        ├─ TIMER       → 心跳逻辑
                                        └─ CLOSED      → 触发重连
                                    }
wsl_stop()  ──── stopping=1 ──────→ 退出循环
            ──── pthread_join ─────← 线程结束
wsl_destroy()
```

### 线程安全边界

| 资源 | 保护机制 | 访问线程 |
|------|---------|---------|
| 发送队列（`q_head/q_tail/q_msgs/q_bytes`） | `q_lock` 互斥锁 | 主线程 + 服务线程 |
| 停止标志（`stopping`） | `volatile` | 主线程写，服务线程读 |
| 连接状态（`state`、`wsi`） | 仅服务线程访问 | 服务线程 |
| 心跳标志（`send_ping`、`ping_pending`） | 仅服务线程访问 | 服务线程 |

### 跨线程通信方式

```c
// 方式一：lws_cancel_service() —— 通知有数据待发送
lws_cancel_service(c->ctx);           // 主线程调用
// → 服务线程收到 LWS_CALLBACK_EVENT_WAIT_CANCELLED

// 方式二：volatile 标志 —— 通知停止
c->stopping = 1;                      // 主线程写
// → 服务线程在下次循环判断 while (!c->stopping)
```

---

## 7. 数据流图

### 发送路径

```
wsl_send(data, len)
    ↓
enqueue() → 加锁入队 → lws_cancel_service()
    ↓
────────────── 线程边界 ──────────────
    ↓
LWS_CALLBACK_EVENT_WAIT_CANCELLED
    ↓
lws_callback_on_writable(wsi)
    ↓
LWS_CALLBACK_CLIENT_WRITEABLE
    ↓
加锁取队头 → lws_write(buf, len, type)
    ↓
释放节点内存
    ↓
队列非空 → 继续触发 WRITEABLE
```

### 接收路径

```
网络数据到达
    ↓
LWS 内部帧解析
    ↓
LWS_CALLBACK_CLIENT_RECEIVE
    ↓
lws_is_first/final_fragment() 判断
    ├─ 单帧 → rx_cb(data, len, user)
    └─ 多帧 → 追加 frag_buf
                 ↓（最后一帧）
              rx_cb(frag_buf, frag_len, user)
```

### 心跳路径

```
定时器到期 → LWS_CALLBACK_TIMER
    ├─ ping_pending → 超时关闭 → 触发重连
    └─ !ping_pending → lws_write(PING) → ping_pending=1
                           ↓
                    LWS_CALLBACK_CLIENT_RECEIVE_PONG
                           ↓
                    ping_pending=0 → 重启定时器
```

---

## 8. LWS 底层 API 详解

### 核心函数

#### lws_get_context()

```c
struct lws_context *lws_get_context(const struct lws *wsi);
```

从 WebSocket 连接实例（`wsi`）获取其所属的 LWS Context。由于 LWS 回调签名中不直接暴露 context，需要通过此函数反向获取。

#### lws_context_user()

```c
void *lws_context_user(struct lws_context *context);
```

返回创建 context 时 `info.user` 字段绑定的用户数据指针。

### 回调中获取 wsl_client 的链式调用

`ws_client.c` 中的关键桥梁代码（第 162 行）：

```c
wsl_client_t *c = lws_context_user(lws_get_context(wsi));
```

该链式调用的数据流如下：

```
wsi（回调参数）
    │
    ↓ lws_get_context(wsi)
    │
struct lws_context（context->user 在创建时绑定了 wsl_client_t *）
    │
    ↓ lws_context_user(context)
    │
void *  →  强转为  wsl_client_t *c
```

绑定发生在 `wsl_start()` 的 context 创建阶段：

```c
struct lws_context_creation_info info = {0};
info.port      = CONTEXT_PORT_NO_LISTEN;
info.protocols = c->protocols;
info.user      = c;                  // ← 在此绑定
c->ctx = lws_create_context(&info);
```

### 两种用户数据绑定方式对比

| 方式 | 绑定位置 | 获取方式 | 适用场景 |
|------|---------|---------|---------|
| **Context User**（当前方案） | `info.user` | `lws_context_user(lws_get_context(wsi))` | 单连接客户端、全局状态 |
| **Per-Session User** | `protocols[].per_session_data_size` | 直接使用回调的 `user` 参数 | 服务器端、每连接独立状态 |

### 注意事项

```c
// 1. 必须做 NULL 检查
wsl_client_t *c = lws_context_user(lws_get_context(wsi));
if (!c) return 0;

// 2. 需要显式类型转换
wsl_client_t *c = (wsl_client_t *)lws_context_user(lws_get_context(wsi));

// 3. 正确的生命周期管理
wsl_stop(client);    // ① 等待服务线程退出
wsl_destroy(client); // ② 再释放内存

// 4. 只有队列等共享数据需要加锁，纯服务线程字段不需要
pthread_mutex_lock(&c->q_lock);
msg_node_t *node = c->q_head;
pthread_mutex_unlock(&c->q_lock);
```

---

## 9. 使用示例

### 基本连接与收发

```c
#include "ws_client.h"
#include <stdio.h>
#include <string.h>

void on_message(const char *data, size_t len, void *user)
{
    printf("Received: %.*s\n", (int)len, data);
}

int main(void)
{
    // 1. 创建客户端
    wsl_client_t *client = wsl_create(
        "echo.websocket.org", 80, "my-protocol", on_message, NULL);
    if (!client) return 1;

    // 2. 可选配置
    wsl_set_ping(client, 30000, 10000);
    wsl_set_reconnect(client, 1000, 60000);
    wsl_set_queue_limit(client, 100, 1024 * 1024);

    // 3. 启动
    if (!wsl_start(client)) { wsl_destroy(client); return 1; }

    // 4. 发送消息
    const char *msg = "Hello WebSocket";
    wsl_send(client, msg, strlen(msg));

    // 5. 停止并销毁
    wsl_stop(client);
    wsl_destroy(client);
    return 0;
}
```

### WSS 加密连接

```c
wsl_client_t *client = wsl_create(
    "secure.example.com", 443, "my-protocol", on_message, NULL);

wsl_set_ssl(client, 1, 0);  // 启用 SSL，严格验证证书
wsl_start(client);
```

### 传递业务上下文

```c
typedef struct {
    int    message_count;
    time_t start_time;
} Stats;

void on_message(const char *data, size_t len, void *user)
{
    Stats *s = (Stats *)user;
    s->message_count++;
    printf("[msg #%d] %.*s\n", s->message_count, (int)len, data);
}

int main(void)
{
    Stats stats = { .start_time = time(NULL) };
    wsl_client_t *client = wsl_create(
        "example.com", 80, NULL, on_message, &stats);
    wsl_start(client);
    // ...
}
```

### 自定义路径与二进制发送

```c
wsl_client_t *client = wsl_create("example.com", 80, NULL, on_msg, NULL);
wsl_set_path(client, "/ws/stream");
wsl_start(client);

unsigned char bin[] = {0x01, 0x02, 0x03};
wsl_send_binary(client, bin, sizeof(bin));
```

---

## 10. 性能与调试

### 性能建议

**批量发送：** 短时间内多次调用 `wsl_send()` 效果良好，内部 WRITEABLE 回调会自动连续取队列发送，无需应用层合并。

**心跳频率：**

```c
wsl_set_ping(client, 60000, 15000); // 低频：节省带宽，适合稳定网络
wsl_set_ping(client, 10000,  5000); // 高频：快速检测断线，适合弱网环境
```

**队列监控：**

```c
pthread_mutex_lock(&c->q_lock);
printf("Queue: %zu msgs, %zu bytes\n", c->q_msgs, c->q_bytes);
pthread_mutex_unlock(&c->q_lock);
```

**内存优化方向（高频场景）：** 当前实现每条消息都执行 `malloc/free`，高频发送时可考虑引入内存池或环形缓冲区。

### 调试技巧

**启用 LWS 内置日志：**

```c
// 在 wsl_start() 之前调用
lws_set_log_level(LLL_ERR | LLL_WARN | LLL_INFO, NULL);
```

**连接状态日志：**

```c
case LWS_CALLBACK_CLIENT_ESTABLISHED:
    printf("[wsl] Connected to %s:%d\n", c->host, c->port);
    break;

case LWS_CALLBACK_CLIENT_CLOSED:
    printf("[wsl] Disconnected, will reconnect\n");
    break;
```

**优雅退出：**

```c
// 正确顺序
wsl_stop(client);    // 1. 阻塞等待服务线程退出
wsl_destroy(client); // 2. 释放所有资源

// 错误示例（导致 use-after-free）
wsl_destroy(client); // 内存已释放
// 服务线程仍在运行，访问 c->host 崩溃
```

---

## 11. 架构设计权衡：一客户端一上下文

### 背景

libwebsockets 的官方推荐模式是**每进程一个 `lws_context`**，在单一上下文中复用多个 `lws`（wsi）连接：

```
推荐模式（libwebsockets 官方）：
  1 进程 → 1 lws_context → N 个 wsi（连接）
  └─ 共享 event loop、fd 管理、SSL 初始化
```

`wsl_client` 采用了不同的策略——每个 `wsl_client_t` 实例持有独立的 `lws_context` 和服务线程：

```
wsl_client 的模式：
  1 进程 → N 个 wsl_client_t
            └─ 每个：1 lws_context + 1 service thread + 1 wsi
```

### 偏离的本质

这是有意为之的设计权衡，不是 API 错误。libwebsockets 允许创建多个 context，只是在大规模场景下效率较低。

### 优势

| 优势 | 说明 |
|------|------|
| **完全隔离** | 每个连接的重连、心跳、发送队列、状态机互不干扰 |
| **故障隔离** | 一个连接异常不影响其他连接的服务线程 |
| **API 简洁** | 应用层只需持有 `wsl_client_t*`，无需关心多路复用逻辑 |
| **生命周期清晰** | `wsl_create` / `wsl_start` / `wsl_stop` / `wsl_destroy` 对应完整生命周期 |
| **无共享状态** | 回调通过 `lws_context_user()` 直接拿到对应实例，无需全局锁或映射表 |

### 代价

| 代价 | 说明 |
|------|------|
| **内存开销** | N 个 context 各自初始化 SSL、fd 集合、内存池等全局结构 |
| **线程开销** | N 个服务线程常驻，各占系统线程资源 |
| **扩展上限** | 适合小规模并发（个位数到十几个连接）；百级以上并发应使用单 context 多 wsi 架构 |

### 适用场景

当前设计适合以下场景：

- 并发连接数较少（< 20 个）
- 每个连接有独立的业务语义（如独立的 ASR 会话、独立的服务地址）
- 需要每个连接完全独立的重连策略和状态管理
- 应用层希望 API 尽量简单，无需管理连接池

### 不适用场景

如果需要以下特性，应重新设计为单 context 架构：

- 大规模并发连接（> 50 个）
- 连接动态创建销毁频率极高
- 对内存和线程资源有严格限制的嵌入式环境

### 与 libwebsockets 官方建议的对应关系

| 方面 | 官方推荐 | wsl_client 实现 |
|------|---------|----------------|
| context 数量 | 每进程一个 | 每 `wsl_client_t` 一个 |
| per-connection 状态 | `per_session_data_size` 字段 | `wsl_client_t` 结构体直接持有 |
| 连接管理 | `lws_client_connect_via_info()` 多次调用 | 每个实例独立管理 |
| 线程模型 | 共享 event loop | 每实例独立服务线程 |

---

## 附录：关键常量

```c
/* 默认配置 */
#define DEFAULT_PING_INTERVAL_MS    30000   // Ping 间隔：30 秒
#define DEFAULT_PONG_TIMEOUT_MS     10000   // Pong 超时：10 秒
#define DEFAULT_RECONNECT_INIT_MS   1000    // 初始重连间隔：1 秒
#define DEFAULT_RECONNECT_MAX_MS    60000   // 最大重连间隔：60 秒

/* 服务线程轮询间隔 */
#define SERVICE_POLL_MS             50      // 每次 lws_service() 最大等待：50 毫秒

/* 分片缓冲区 */
#define FRAG_INIT_CAP               4096    // 初始容量：4 KB（不足时 2 倍扩容）

/* LWS 帧头预留空间 */
#define LWS_PRE                     10      // WebSocket 帧头最大长度
```

---

**文档版本：** 2.0  
**最后更新：** 2026-04-21  
**维护者：** Project Team