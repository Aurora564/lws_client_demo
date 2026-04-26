# lws_client_demo 项目综合指南

> C11 WebSocket 客户端库，基于 libwebsockets v4.1.6。提供三种 API 风格，覆盖 1:1 到 1:N 的连接模型。

---

## 目录

1. [项目结构](#项目结构)
2. [API 概览](#api-概览)
3. [API 选择指南](#api-选择指南)
4. [v1 (wsld_) — 基础客户端](#v1-wsld--基础客户端)
5. [v2 (wsl_) — 事件钩子客户端](#v2-wsl--事件钩子客户端)
6. [Pool (wsp_) — 连接池](#pool-wsp--连接池)
7. [Server (wss_) — 测试服务器](#server-wss--测试服务器)
8. [事件钩子层 (wsl_event_hooks_t)](#事件钩子层-wsl_event_hooks_t)
9. [线程模型与线程安全](#线程模型与线程安全)
10. [常见问题](#常见问题)

---

## 项目结构

```
lws_client_demo/
├── src/                     # 库源码
│   ├── ws_client.h / .c     # v1: 基础客户端 (wsld_ 前缀)
│   ├── ws_client_v2.h / .c  # v2: 事件钩子客户端 (wsl_ 前缀)
│   ├── ws_pool.h / .c       # Pool: 连接池 (wsp_ 前缀)
│   ├── ws_server.h / .c     # Server: 测试用 echo 服务器 (wss_ 前缀)
│   └── ws_internal.h        # 共享内部类型 (FAM 节点、分片缓冲、钩子类型)
├── examples/                # 使用示例
├── libs/libwebsockets/      # lws v4.1.6 子模块
├── output/                  # 预构建的 lws 产物
├── docs/                    # 架构文档、分析笔记
├── CMakeLists.txt
└── TODO.md
```

## API 概览

| API | 前缀 | 线程模型 | 重连 | 钩子 | 适用场景 |
|-----|------|---------|------|------|---------|
| v1 | `wsld_` | 1 连接 = 1 线程 + 1 context | clock_gettime 轮询 | 无 | 最简集成，单个连接 |
| v2 | `wsl_` | 1 连接 = 1 线程 + 1 context | lws_sul 定时器 | 7 个钩子 | 需要生命周期控制的单连接 |
| Pool | `wsp_` | N 连接共享 1 线程 + 1 context | lws_sul 定时器 | 7 个钩子 | 大量连接、低资源消耗 |
| Server | `wss_` | 1 服务线程 | — | — | 本地测试 |

### 前缀说明

- `wsld_` — v1 客户端（ws legacy deprecated），与 v2 前缀不同，可同时链接
- `wsl_` — v2 客户端（ws library）
- `wsp_` — 连接池（ws pool）
- `wss_` — 服务器（ws server）

## API 选择指南

### 选 v1（wsld_）

```c
wsl_client_t *c = wsld_create(host, port, protocol, rx_cb, user);
wsld_start(c);
wsld_send(c, "hello", 5);
wsld_destroy(c);
```

适合：
- 只需要**一个** WebSocket 连接
- 不想引入额外依赖（v1 接口最简单）
- 不关心 is_binary 区分（v1 rx_cb 没有 binary 参数）

不适合：
- 需要连接事件回调（on_connected / on_disconnected）
- 需要自定义重连策略
- 需要区分 text/binary 帧

### 选 v2（wsl_）

```c
wsl_client_t *c = wsl_create(host, port, protocol, rx_cb, user);
wsl_set_event_hooks(c, &hooks, NULL);
wsl_start(c);
wsl_send(c, "hello", 5);
wsl_stop(c);
wsl_destroy(c);
```

适合：
- 需要**一个**连接，但需要**生命周期控制**
- 认证握手（连接建立后发 token）
- 自定义心跳逻辑
- 熔断器模式（重连失败 N 次后停止）

### 选 Pool（wsp_）

```c
wsp_pool_t *pool = wsp_pool_create();
wsp_client_t *c = wsp_client_create(host, port, protocol, rx_cb, user);
wsp_pool_add(pool, c);
wsp_pool_start(pool);
wsp_send(c, "hello", 5);
wsp_pool_destroy(pool);
```

适合：
- 同时管理**多个** WebSocket 连接（如 10+ 路并发 ASR）
- 资源敏感场景（N 个连接只需 2 个线程）
- 所有连接连向同一服务端

不适合：
- 每个连接需要独立的 lws_context 配置
- 需要单个连接独立停止且精确等待关闭完成

---

## v1 (wsld_) — 基础客户端

### 生命周期

```c
wsld_create()       // 分配内存、设默认参数
    ↓
wsld_set_*()        // [可选] 调整心跳/重连/SSL/路径
    ↓
wsld_start()        // 创建 lws_context + service 线程，发起连接
    ↓
wsld_send()         // [循环] 发数据（线程安全）
    ↓
wsld_destroy()      // 停止 + 释放
```

### 回调

```c
typedef void (*wsld_rx_cb_t)(const char *data, size_t len,
                              int is_binary, void *user);
```

data 在回调返回后立即失效，需要持久化请自行拷贝。is_binary 标识 text(0) / binary(1) 帧。

### 配置函数

```c
wsld_set_ping(c, 30000, 10000);         // 心跳 30s，pong 超时 10s
wsld_set_reconnect(c, 1000, 60000);     // 初始 1s，最大 60s，指数退避
wsld_set_queue_limit(c, 100, 1048576);  // 队列上限 100 条/1MB
wsld_set_ssl(c, 1, 0);                  // 启用 WSS，严格验证证书
wsld_set_path(c, "/ws/api");            // URL 路径
```

### 示例

```bash
./app/example 127.0.0.1 8080 my-protocol
./app/example example.com 443 chat --wss
```

---

## v2 (wsl_) — 事件钩子客户端

### 生命周期

```c
wsl_create()            // 分配内存
    ↓
wsl_set_*()             // 配置参数
wsl_set_event_hooks()   // 注册事件钩子
    ↓
wsl_start()             // 创建 context + 线程，发起连接
    ↓
wsl_send()              // 发数据
    ↓
wsl_stop()              // 停止服务线程，销毁 context
    ↓
wsl_destroy()           // 释放内存
```

### 事件钩子

```c
typedef struct {
    void (*on_connected)(void *user);
    void (*on_disconnected)(void *user);
    void (*on_error)(const char *msg, void *user);
    void (*on_message)(const char *data, size_t len,
                       int is_binary, void *user);
    int  (*on_heartbeat_tick)(void *user);
    int  (*on_reconnect_decision)(int fail_count,
                                  int current_delay_ms,
                                  void *user);
    void (*on_handshake_header)(struct lws *wsi, void *user);
} wsl_event_hooks_t;
```

各钩子均为可选（NULL 表示不使用），在 service 线程中同步调用，不应执行耗时操作。

### 场景示例

**认证握手：** `on_connected` 中发 token → `on_message` 验证响应 → `on_disconnected` 重置状态

**熔断器：** `on_reconnect_decision` 返回 -1 停止重连

**自定义心跳：** `on_heartbeat_tick` 返回非 0 接管心跳

### 与 wsl_rx_cb_t 的关系

```c
// rx_cb 和 on_message 同时触发
case LWS_CALLBACK_CLIENT_RECEIVE:
    if (c->rx_cb)
        c->rx_cb(data, len, is_binary, c->rx_user);   // 原始回调
    if (c->hooks && c->hooks->on_message)
        c->hooks->on_message(data, len, is_binary,     // 钩子
                              c->hook_user);
```

### 示例

```bash
./app/v2_hooks_demo
```

三个场景：基础钩子 → 重连熔断 → 认证握手。

---

## Pool (wsp_) — 连接池

### 生命周期

```c
wsp_pool_create()               // 创建连接池
    ↓
wsp_client_create()             // 创建客户端（可创建多个）
wsp_set_*()                     // 配置各客户端
wsp_set_event_hooks()           // [可选] 注册钩子
    ↓
wsp_pool_add(pool, client)     // 将客户端注册到池
    ↓
wsp_pool_start(pool)            // 创建 1 个 context + 1 个 service 线程
    ↓                               // 对所有已注册客户端发起连接
wsp_send(client, data, len)     // 发数据
    ↓
wsp_pool_destroy(pool)          // 停止所有客户端 + 销毁
```

### 与 v2 的关键区别

| 特性 | v2 (wsl_) | Pool (wsp_) |
|------|----------|-------------|
| 线程数 | N 连接 = N+1 线程 | N 连接 = 2 线程（1 main + 1 service）|
| context 数 | N 连接 = N 个 context | N 连接 = 1 个 context |
| 内存开销 | 高 | 低 |
| 连接启动 | 立即在 service 线程中发起 | 通过 pending 队列排队，在 CANCELLED 中发起 |
| 停止同步 | pthread_join 等待线程退出 | condvar 等待 CLOSED + 3s 超时保护 |

### 停止机制说明

Pool 的停止涉及 condvar 同步，有 3 秒超时保护：

```c
wsp_pool_destroy() → wsp_client_stop()：
  1. 设 stopping = 1，lws_cancel_service 唤醒 service 线程
  2. service 线程在 WAIT_CANCELLED 中关闭 wsi
  3. 等待 CLOSED 回调（最多 3 秒）
  4. 超时后强制退出，lws_context_destroy 清理残余
```

详细分析见 `docs/pool_stop_mechanism.md`。

### 示例

```bash
./app/pool_basic              # 单连接池
./app/pool_multi_client       # 5 路并发连接
./app/pool_asr_sessions       # ASR 多路语音识别
./app/pool_hooks_demo         # 钩子示例（三场景）
```

---

## Server (wss_) — 测试服务器

轻量 echo 服务器，用于本地测试。

```c
wss_server_t *s = wss_create(8080, "ws");
wss_set_rx_cb(s, my_rx, NULL);
wss_start(s);
// ... 等待 Ctrl+C ...
wss_stop(s);
wss_destroy(s);
```

```bash
./app/echo_server 8080 ws          # 指定子协议
./app/echo_server 8080             # 默认 "default" 协议
```

---

## 事件钩子层 (wsl_event_hooks_t)

钩子类型定义在 `ws_internal.h`，v2 和 Pool 共用。

### 触发时机

| 钩子 | 触发时机 | service 线程? |
|------|---------|:---:|
| `on_connected` | WebSocket 握手完成，ESTABLISHED 回调 | ✓ |
| `on_disconnected` | 连接断开（CLOSED / CONNECTION_ERROR），重连前 | ✓ |
| `on_error` | CONNECTION_ERROR | ✓ |
| `on_message` | 收到完整消息（含分片重组后）| ✓ |
| `on_heartbeat_tick` | TIMER 触发，返回 0 走默认 ping/pong | ✓ |
| `on_reconnect_decision` | 重连前，参数为当前失败次数和默认延迟 | ✓ |
| `on_handshake_header` | 握手阶段，可调用 lws API 添加自定义 HTTP 头 | ✓ |

### 注意事项

- **不要在钩子中执行耗时操作** —— 会阻塞 service 线程的 event loop
- **不要从钩子中直接调用 lws_write** —— 使用 `wsl_send` / `wsp_send` 入队
- 钩子和 `wsl_rx_cb_t` / `wsl_conn_cb_t` 兼容，二者同时触发

---

## 线程模型与线程安全

### v1 / v2

```
[任意线程] ── wsl_send() ──→ 加锁入队 ──→ lws_cancel_service(ctx)
                                        ↑
[service 线程] ── lws_service() ──→ WAIT_CANCELLED ──→ WRITABLE ──→ lws_write()
```

- 外部线程通过**加锁入队 + cancel_service** 和 service 线程通信
- **永远不要**在 service 线程外部调用 lws 函数
- service 线程在 `wsl_stop()` 的 `pthread_join` 中等待退出

### Pool

```
[任意线程] ── wsp_send() ──→ 加锁入队 ──→ lws_cancel_service(ctx)
                                        ↑
         ┌─────────────────── WAIT_CANCELLED ──────────────┐
         │  ① 消费 pending 队列，发起新连接                  │
[service 线程]                   ② 遍历有数据的 client，请求 WRITABLE  │
         │  ③ 若 client->stopping，关闭 wsi                 │
         └────────────────────────────────────────────────┘
                ↑
          lws_service() 循环
```

- Pool 使用 `stop_mu` 保护 `wsi` / `stopping` / `stopped`
- CLOSED 回调中持有 `stop_mu` 设置 `stopped = 1` 并 signal condvar
- 停止有 3 秒超时保护，防止 TCP buffer 延迟导致永久挂死

### 线程安全规则

1. **lws_write 只能在 WRITEABLE 回调中调用**
2. **外部线程不要直接调用 lws 函数操作 wsi**
3. **发送队列受 mutex 保护**，入队后通过 `lws_cancel_service()` 唤醒 service 线程
4. **不要在钩子或 rx_cb 中执行耗时操作** —— 会阻塞 service 线程

---

## 常见问题

### Q: v1 和 v2 能同时链接吗？

能。v1 使用 `wsld_` 前缀，v2 使用 `wsl_` 前缀。类型和函数名不冲突。

### Q: Pool 的停止为什么有 3 秒超时？

当服务端 TCP buffer 中还有未消费的 echo 数据时，close 帧的处理被延迟，`CLOSED` 回调不会立即触发。3 秒超时防止 condvar 永久挂死。详细分析见 `docs/pool_stop_mechanism.md`。

### Q: 为什么设置了 NULL 协议，连接还是失败了？

- v2：NULL → 默认 `"ws"`
- Pool：NULL → 使用 context 的第一个协议（`"wsp-pool"`）

如果服务端协议是 `"ws"`，Pool 客户端需要显式传 `"ws"`。

### Q: rx_cb 的 is_binary 参数是什么？

`int is_binary`：0 = 文本帧，1 = 二进制帧。v1/v2/Pool 的 rx_cb 都支持。

### Q: 如何发送二进制数据？

```c
wsl_send_binary(c, data, len);   // v2
wsld_send_binary(c, data, len);  // v1
wsp_send_binary(c, data, len);   // Pool
```

### Q: 如何启用 WSS？

```c
wsl_set_ssl(c, 1, 0);           // 严格验证
wsl_set_ssl(c, 1, 1);           // 跳过证书验证（测试环境）
```
