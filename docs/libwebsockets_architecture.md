# Libwebsockets 核心架构详解

本文档详细分析 libwebsockets 库的核心实现组成，包括 context、vhost、wsi、protocol 等关键概念及其相互关系。

## 目录

- [1. 概述](#1-概述)
- [2. 核心组件](#2-核心组件)
  - [2.1 struct lws_context (上下文)](#21-struct-lws_context-上下文)
  - [2.2 struct lws_vhost (虚拟主机)](#22-struct-lws_vhost-虚拟主机)
  - [2.3 struct lws (WSI - WebSocket Instance)](#23-struct-lws-wsi---websocket-instance)
  - [2.4 struct lws_protocols (协议)](#24-struct-lws_protocols-协议)
- [3. 回调机制](#3-回调机制)
- [4. 架构层次图](#4-架构层次图)
- [5. 使用流程示例](#5-使用流程示例)
- [6. 关键设计要点](#6-关键设计要点)

---

## 1. 概述

Libwebsockets 是一个轻量级的 C 语言 WebSocket 库，支持客户端和服务器端实现。其采用分层架构设计，从全局上下文到具体连接实例，形成了清晰的对象层次结构。

**主要特点：**
- 支持 WebSocket 协议（RFC 6455）
- 支持 HTTP/1.1 和 HTTP/2
- 支持 SSL/TLS（OpenSSL、mbedTLS、WolfSSL）
- 支持多种事件循环（poll、libuv、libev、glib）
- 支持多线程服务
- 支持虚拟主机（Virtual Hosts）
- 支持插件系统

---

## 2. 核心组件

### 2.1 struct lws_context (上下文)

#### 作用
`lws_context` 是整个 libwebsockets 库的**全局管理结构**，是最高层级的容器对象。一个进程通常只创建一个 context。

#### 主要职责
- **管理所有的 vhost**：维护虚拟主机链表
- **维护全局配置和状态**：包括线程池、事件循环、插件系统等
- **管理服务线程**：支持多线程并发处理（通过 `count_threads` 配置）
- **处理事件循环**：集成 poll/libuv/libev/glib 等事件库
- **资源生命周期管理**：统一管理所有连接的创建和销毁

#### 创建与配置

```c
struct lws_context_creation_info {
    uint64_t options;                      // 全局选项标志
    const struct lws_protocols *protocols; // 默认协议列表
    unsigned int count_threads;            // 服务线程数量（默认1）
    void *user;                            // 用户自定义数据指针
    unsigned int fd_limit_per_thread;      // 每个线程的文件描述符限制
    const char *vhost_name;                // 默认 vhost 名称
    
    // 网络相关
    int port;                              // 监听端口（客户端用 CONTEXT_PORT_NO_LISTEN）
    const char *iface;                     // 绑定接口
    
    // SSL/TLS 相关
#if defined(LWS_WITH_TLS)
    const char *ssl_cert_filepath;         // 证书文件路径
    const char *ssl_private_key_filepath;  // 私钥文件路径
    const char *ssl_ca_filepath;           // CA 证书路径
#endif
    
    // 超时配置
    unsigned int timeout_secs;             // 各种操作的超时时间
    int ka_time;                           // TCP keepalive 时间
};
```

#### 常用 API

```c
// 创建上下文
struct lws_context *lws_create_context(const struct lws_context_creation_info *info);

// 销毁上下文（会清理所有 vhost 和连接）
void lws_context_destroy(struct lws_context *context);

// 获取用户自定义数据
void *lws_context_user(struct lws_context *context);

// 取消服务等待（用于跨线程通知）
void lws_cancel_service(struct lws_context *context);

// 服务循环（处理所有 pending 事件）
int lws_service(struct lws_context *context, int timeout_ms);
```

#### 重要选项标志

```c
#define LWS_SERVER_OPTION_EXPLICIT_VHOSTS      (1ll << 13)
// 不自动创建默认 vhost，需要手动调用 lws_create_vhost()

#define LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT   (1ll << 12)
// 初始化 SSL 库

#define LWS_SERVER_OPTION_LIBUV                (1ll << 10)
// 使用 libuv 事件循环

#define LWS_SERVER_OPTION_LIBEV                (1ll << 4)
// 使用 libev 事件循环
```

---

### 2.2 struct lws_vhost (虚拟主机)

#### 作用
`lws_vhost` 表示一个**虚拟主机**，可以理解为独立的"站点"配置。一个 context 中可以包含多个 vhost，每个 vhost 可以有独立的配置。

#### 主要职责
- **绑定监听端口**：每个 vhost 可以监听不同端口，或共享同一端口
- **管理该 vhost 下的所有连接**：维护 wsi 链表
- **维护独立的 SSL/TLS 上下文**：每个 vhost 可以有独立的证书
- **管理 URL 挂载点（mounts）**：配置 HTTP 文件服务、CGI、反向代理等
- **管理该 vhost 的协议列表**：不同 vhost 可以支持不同的协议

#### 创建方式

**方式一：隐式创建（默认）**
```c
// 创建 context 时会自动创建一个名为 "default" 的 vhost
struct lws_context_creation_info info = {0};
info.port = 8080;
info.protocols = my_protocols;
struct lws_context *ctx = lws_create_context(&info);
// 此时已有一个默认 vhost
```

**方式二：显式创建**
```c
// 创建 context 时不创建 vhost
struct lws_context_creation_info ctx_info = {0};
ctx_info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
struct lws_context *ctx = lws_create_context(&ctx_info);

// 手动创建多个 vhost
struct lws_context_creation_info vh_info1 = {0};
vh_info1.port = 80;
vh_info1.vhost_name = "example.com";
vh_info1.ssl_cert_filepath = "/path/to/cert.pem";
struct lws_vhost *vh1 = lws_create_vhost(ctx, &vh_info1);

struct lws_context_creation_info vh_info2 = {0};
vh_info2.port = 443;
vh_info2.vhost_name = "secure.example.com";
vh_info2.ssl_cert_filepath = "/path/to/secure-cert.pem";
struct lws_vhost *vh2 = lws_create_vhost(ctx, &vh_info2);
```

#### 常用 API

```c
// 创建 vhost
struct lws_vhost *lws_create_vhost(struct lws_context *context,
                                    const struct lws_context_creation_info *info);

// 销毁 vhost
void lws_vhost_destroy(struct lws_vhost *vh);

// 从 wsi 获取所属 vhost
struct lws_vhost *lws_get_vhost(struct lws *wsi);

// 获取 vhost 名称
const char *lws_get_vhost_name(struct lws_vhost *vhost);

// 获取 vhost 监听端口
int lws_get_vhost_port(struct lws_vhost *vhost);

// 获取 vhost 用户数据
void *lws_vhost_user(struct lws_vhost *vhost);

// 根据名称查找 vhost
struct lws_vhost *lws_get_vhost_by_name(struct lws_context *context, 
                                         const char *name);
```

#### Vhost 配置示例

```c
struct lws_context_creation_info vh_info = {0};
vh_info.port = 443;
vh_info.vhost_name = "myserver.com";

// SSL 配置
vh_info.ssl_cert_filepath = "/etc/ssl/certs/server.crt";
vh_info.ssl_private_key_filepath = "/etc/ssl/private/server.key";
vh_info.ssl_ca_filepath = "/etc/ssl/certs/ca-bundle.crt";

// 协议配置
vh_info.protocols = my_protocols;

// HTTP 挂载点
vh_info.mounts = &http_mounts;

// 超时配置
vh_info.timeout_secs = 30;
vh_info.keepalive_timeout = 5;

struct lws_vhost *vh = lws_create_vhost(context, &vh_info);
```

---

### 2.3 struct lws (WSI - WebSocket Instance)

#### 作用
`struct lws`（通常称为 **wsi**）表示一个**具体的网络连接实例**，是 libwebsockets 中最核心的运行时对象。每个 TCP/WebSocket 连接都对应一个 wsi。

#### 主要职责
- **封装 socket 文件描述符**：管理底层网络连接
- **维护连接状态**：握手、已连接、关闭等状态机
- **管理读写缓冲区**：处理数据的接收和发送
- **关联到特定的 vhost 和 protocol**：确定使用哪个协议处理器
- **处理心跳、超时等**：维护连接健康状态
- **分片重组**：处理 WebSocket 消息分片

#### 内部状态（简化）

```c
struct lws {
    struct lws_vhost *vhost;                    // 所属 vhost
    const struct lws_protocols *protocol;       // 使用的协议
    lws_sockfd_type sockfd;                     // socket 文件描述符
    
    // 状态相关
    enum lws_connection_states state;           // 连接状态
    enum lws_ws_state ws_state;                 // WebSocket 状态
    
    // 缓冲区
    unsigned char *rx_buffer;                   // 接收缓冲区
    size_t rx_buffer_len;                       // 接收数据长度
    
    // 定时器
    struct lws_timeout timeout;                 // 超时管理
    
    // 用户数据
    void *user_space;                           // 协议会话数据
    
    // ... 更多内部字段
};
```

#### 连接类型

**客户端连接：**
```c
struct lws_client_connect_info conn_info = {0};
conn_info.context = context;
conn_info.address = "example.com";
conn_info.port = 443;
conn_info.path = "/ws";
conn_info.host = "example.com";
conn_info.origin = "example.com";
conn_info.protocol = "my-protocol";
conn_info.ssl_connection = LCCSCF_USE_SSL;

struct lws *wsi = lws_client_connect_via_info(&conn_info);
```

**服务器端连接：**
```c
// 服务器端 wsi 由 lws 自动创建，当客户端连接时
// 在回调中通过参数传入
static int callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len)
{
    // wsi 就是新建立的连接
}
```

#### 常用 API

```c
// 获取 wsi 所属的 protocol
const struct lws_protocols *lws_get_protocol(struct lws *wsi);

// 获取 wsi 所属的 vhost
struct lws_vhost *lws_get_vhost(struct lws *wsi);

// 获取 wsi 所属的 context
struct lws_context *lws_get_context(const struct lws *wsi);

// 触发可写回调（通知 lws 调用 WRITEABLE 回调）
void lws_callback_on_writable(struct lws *wsi);

// 发送数据
int lws_write(struct lws *wsi, unsigned char *buf, size_t len, 
              enum lws_write_protocol protocol);

// 关闭连接
void lws_close_reason(struct lws *wsi, enum lws_close_status status,
                      unsigned char *buf, size_t len);

// 设置定时器
void lws_set_timer_usecs(struct lws *wsi, lws_usec_t usecs);

// 获取用户数据（per-session data）
void *lws_get_opaque_user_data(struct lws *wsi);
```

#### 数据发送

```c
// 文本消息
unsigned char buf[LWS_PRE + 1024];
strcpy((char *)buf + LWS_PRE, "Hello WebSocket");
int n = lws_write(wsi, buf + LWS_PRE, strlen("Hello WebSocket"), 
                  LWS_WRITE_TEXT);

// 二进制消息
lws_write(wsi, binary_data + LWS_PRE, data_len, LWS_WRITE_BINARY);

// Ping（同样需要 LWS_PRE 预留空间）
unsigned char ping_buf[LWS_PRE];
lws_write(wsi, ping_buf + LWS_PRE, 0, LWS_WRITE_PING);
```

**注意：** `LWS_PRE` 是预留空间（通常为 16 字节，与平台指针对齐），用于 WebSocket 帧头。发送任何类型的消息（包括 PING/PONG）时，传入 `lws_write()` 的指针前必须有 `LWS_PRE` 字节的可写空间。

---

### 2.4 struct lws_protocols (协议)

#### 作用
`lws_protocols` 定义 **WebSocket 协议处理器**，将协议名称与回调函数绑定。它是用户代码与 lws 库交互的主要接口。

#### 结构定义

```c
struct lws_protocols {
    const char *name;                      // 协议名称
    lws_callback_function *callback;       // 回调函数指针
    size_t per_session_data_size;          // 每个会话的用户数据大小
    size_t rx_buffer_size;                 // 接收缓冲区大小
    unsigned int id;                       // 用户自定义 ID
    void *user;                            // 用户自定义指针
    size_t tx_packet_size;                 // 发送包大小限制
};
```

#### 字段说明

| 字段 | 说明 | 示例 |
|------|------|------|
| `name` | 协议名称，必须与客户端 JavaScript 中指定的协议匹配 | `"my-protocol"` |
| `callback` | 协议回调函数，处理所有事件 | `my_callback` |
| `per_session_data_size` | 为每个连接分配的用户数据空间大小 | `sizeof(my_session_data)` |
| `rx_buffer_size` | 接收缓冲区大小，0 表示使用默认值 | `4096` |
| `id` | 用户自定义 ID，可用于版本区分 | `2` 表示 v2 |
| `user` | 用户自定义指针，可在回调中访问 | `&global_config` |
| `tx_packet_size` | 单次发送大小限制，0 表示使用 rx_buffer_size | `1024` |

#### 协议定义示例

```c
// 会话数据结构
typedef struct {
    int connected;
    char buffer[4096];
    size_t buffer_len;
} my_session_data_t;

// 回调函数声明
static int my_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len);

// 协议定义
static struct lws_protocols protocols[] = {
    {
        .name = "my-protocol",
        .callback = my_callback,
        .per_session_data_size = sizeof(my_session_data_t),
        .rx_buffer_size = 4096,
        .id = 1,
    },
    { NULL, NULL, 0, 0 }  // 终止条目（必须）
};
```

#### 回调函数原型

```c
typedef int lws_callback_function(struct lws *wsi, 
                                   enum lws_callback_reasons reason,
                                   void *user, 
                                   void *in, 
                                   size_t len);
```

**参数说明：**
- `wsi`：当前连接的 wsi 指针
- `reason`：回调原因（事件类型）
- `user`：指向 per-session 数据的指针（大小为 `per_session_data_size`）
- `in`：输入数据指针（如接收到的消息）
- `len`：输入数据长度

**返回值：**
- `0`：成功
- 非零：出错，通常会关闭连接

---

## 3. 回调机制

### 3.1 回调原因枚举

Libwebsockets 通过回调函数向用户代码通知各种事件。以下是常用的回调原因：

#### 连接生命周期

```c
LWS_CALLBACK_PROTOCOL_INIT
// 协议初始化（每个 vhost 调用一次）

LWS_CALLBACK_PROTOCOL_DESTROY
// 协议销毁（vhost 销毁时调用）

LWS_CALLBACK_WSI_CREATE
// wsi 创建通知

LWS_CALLBACK_WSI_DESTROY
// wsi 销毁通知
```

#### 客户端回调

```c
LWS_CALLBACK_CLIENT_ESTABLISHED
// 客户端连接建立成功

LWS_CALLBACK_CLIENT_RECEIVE
// 接收到服务器数据
// in: 数据指针, len: 数据长度

LWS_CALLBACK_CLIENT_WRITEABLE
// 可以发送数据（调用 lws_callback_on_writable 后触发）

LWS_CALLBACK_CLIENT_CONNECTION_ERROR
// 连接失败
// in: 错误信息字符串

LWS_CALLBACK_CLIENT_CLOSED
// 连接关闭

LWS_CALLBACK_CLIENT_RECEIVE_PONG
// 收到 PONG 响应
```

#### 服务器端回调

```c
LWS_CALLBACK_HTTP
// HTTP 请求

LWS_CALLBACK_ESTABLISHED
// WebSocket 握手完成

LWS_CALLBACK_RECEIVE
// 接收到客户端数据

LWS_CALLBACK_SERVER_WRITEABLE
// 可以发送数据

LWS_CALLBACK_CLOSED
// 连接关闭
```

#### 其他回调

```c
LWS_CALLBACK_TIMER
// 定时器触发（lws_set_timer_usecs 设置）

LWS_CALLBACK_EVENT_WAIT_CANCELLED
// lws_cancel_service 调用后触发

LWS_CALLBACK_CHANGE_MODE_POLL_FD
// poll fd 模式改变
```

### 3.2 回调处理示例

```c
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    // 获取上下文用户数据
    struct lws_context *context = lws_get_context(wsi);
    my_app_data_t *app = lws_context_user(context);
    
    // 获取会话数据
    my_session_data_t *session = (my_session_data_t *)user;
    
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        fprintf(stdout, "Connection established\n");
        session->connected = 1;
        
        // 启动心跳定时器
        lws_set_timer_usecs(wsi, 30 * 1000000); // 30秒
        break;
        
    case LWS_CALLBACK_CLIENT_RECEIVE:
        // 处理接收到的数据
        fprintf(stdout, "Received %zu bytes\n", len);
        
        // 检查是否是最后一片
        if (lws_is_final_fragment(wsi)) {
            // 处理完整消息
            process_message((char *)in, len);
        }
        break;
        
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        // 发送队列中的消息
        if (has_pending_messages()) {
            message_t *msg = get_next_message();
            lws_write(wsi, msg->data + LWS_PRE, msg->len, 
                     LWS_WRITE_TEXT);
            
            // 如果还有消息，继续触发 WRITEABLE
            if (has_more_messages()) {
                lws_callback_on_writable(wsi);
            }
        }
        break;
        
    case LWS_CALLBACK_TIMER:
        // 心跳：发送 ping（需要 LWS_PRE 预留空间）
        {
            unsigned char ping_buf[LWS_PRE];
            lws_write(wsi, ping_buf + LWS_PRE, 0, LWS_WRITE_PING);
        }
        
        // 重启定时器
        lws_set_timer_usecs(wsi, 30 * 1000000);
        break;
        
    case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
        fprintf(stdout, "Pong received\n");
        break;
        
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "Connection error: %s\n", 
                in ? (char *)in : "(unknown)");
        break;
        
    case LWS_CALLBACK_CLIENT_CLOSED:
        fprintf(stdout, "Connection closed\n");
        session->connected = 0;
        break;
        
    default:
        break;
    }
    
    return 0;
}
```

### 3.3 分片处理

WebSocket 消息可能被分片传输，需要正确组装：

```c
case LWS_CALLBACK_CLIENT_RECEIVE: {
    int first = lws_is_first_fragment(wsi);
    int final = lws_is_final_fragment(wsi);
    size_t remaining = lws_remaining_packet_payload(wsi);
    
    if (first && final && remaining == 0) {
        // 单片完整消息
        process_message((char *)in, len);
    } else {
        // 多片消息
        if (first) {
            // 第一片，初始化缓冲区
            session->frag_len = 0;
        }
        
        // 追加数据
        memcpy(session->frag_buf + session->frag_len, in, len);
        session->frag_len += len;
        
        if (final && remaining == 0) {
            // 最后一片，处理完整消息
            process_message((char *)session->frag_buf, 
                          session->frag_len);
            session->frag_len = 0;
        }
    }
    break;
}
```

---

## 4. 架构层次图

### 4.1 对象层次关系

```
┌──────────────────────────────────────────────┐
│         struct lws_context                    │  ← 全局上下文（单例）
│  ┌────────────────────────────────────────┐  │
│  │  - 管理所有 vhost                       │  │
│  │  - 服务线程池                           │  │
│  │  - 事件循环 (poll/libuv/libev/glib)    │  │
│  │  - 插件系统                             │  │
│  │  - 全局配置                             │  │
│  └────────────────────────────────────────┘  │
└──────────┬───────────────────────────────────┘
           │
           ├──→ struct lws_vhost (vhost_1) ────┐  ← 虚拟主机 1
           │   ┌────────────────────────────┐  │
           │   │  - 监听端口: 80             │  │
           │   │  - SSL 上下文: cert_A      │  │
           │   │  - 协议列表                 │  │
           │   │  - HTTP mounts             │  │
           │   └────────────────────────────┘  │
           │        │                          │
           │        ├──→ struct lws (wsi_1)   │  ← 连接 1 (client A)
           │        │   - sockfd: 10           │  │
           │        │   - state: ESTABLISHED   │  │
           │        │   - protocol: ws_proto   │  │
           │        └──→ struct lws (wsi_2)   │  ← 连接 2 (client B)
           │            - sockfd: 11           │  │
           │            - state: ESTABLISHED   │  │
           │                                   │
           └──→ struct lws_vhost (vhost_2) ────┐  ← 虚拟主机 2
               ┌────────────────────────────┐  │
               │  - 监听端口: 443            │  │
               │  - SSL 上下文: cert_B      │  │
               │  - 协议列表                 │  │
               └────────────────────────────┘  │
                        │                      │
                        └──→ struct lws (wsi_3)│  ← 连接 3 (client C)
                            - sockfd: 12       │  │
                            - state: HANDSHAKE │  │
                                               │
```

### 4.2 数据流向

```
客户端应用
    │
    ├─→ lws_client_connect_via_info()
    │       │
    │       ↓
    │   创建 wsi
    │       │
    │       ↓
    │   TCP 连接 + WebSocket 握手
    │       │
    │       ↓
    │   LWS_CALLBACK_CLIENT_ESTABLISHED
    │
    ├─→ lws_write() 发送数据
    │       │
    │       ↓
    │   数据进入发送队列
    │       │
    │       ↓
    │   lws_callback_on_writable()
    │       │
    │       ↓
    │   LWS_CALLBACK_CLIENT_WRITEABLE
    │       │
    │       ↓
    │   实际通过网络发送
    │
    └─← 网络接收数据
            │
            ↓
        LWS_CALLBACK_CLIENT_RECEIVE
            │
            ↓
        应用处理数据
```

### 4.3 服务循环

```c
while (!stopping) {
    lws_service(context, timeout_ms);
    //     │
    //     ├─→ 检查所有 wsi 的事件
    //     ├─→ 处理可读事件 → RECEIVE 回调
    //     ├─→ 处理可写事件 → WRITEABLE 回调
    //     ├─→ 处理定时器 → TIMER 回调
    //     ├─→ 接受新连接 → ESTABLISHED 回调
    //     └─→ 处理关闭事件 → CLOSED 回调
}
```

---

## 5. 使用流程示例

### 5.1 客户端完整示例

```c
#include <libwebsockets.h>
#include <stdio.h>
#include <string.h>

// 应用数据
typedef struct {
    int running;
} app_data_t;

// 会话数据
typedef struct {
    int connected;
} session_data_t;

// 回调函数
static int callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len)
{
    session_data_t *session = (session_data_t *)user;
    struct lws_context *context = lws_get_context(wsi);
    app_data_t *app = lws_context_user(context);
    
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("Connected!\n");
        session->connected = 1;
        lws_callback_on_writable(wsi);
        break;
        
    case LWS_CALLBACK_CLIENT_RECEIVE:
        printf("Received: %.*s\n", (int)len, (char *)in);
        break;
        
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (session->connected) {
            unsigned char buf[LWS_PRE + 128];
            const char *msg = "Hello Server";
            memcpy(buf + LWS_PRE, msg, strlen(msg));
            lws_write(wsi, buf + LWS_PRE, strlen(msg), LWS_WRITE_TEXT);
        }
        break;
        
    case LWS_CALLBACK_CLIENT_CLOSED:
        printf("Closed\n");
        session->connected = 0;
        app->running = 0;
        break;
        
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "Error: %s\n", in ? (char *)in : "unknown");
        app->running = 0;
        break;
        
    default:
        break;
    }
    
    return 0;
}

// 协议定义
static struct lws_protocols protocols[] = {
    {
        .name = "my-protocol",
        .callback = callback,
        .per_session_data_size = sizeof(session_data_t),
        .rx_buffer_size = 4096,
    },
    { NULL, NULL, 0, 0 }
};

int main(void)
{
    app_data_t app = { .running = 1 };
    
    // 1. 创建 context
    struct lws_context_creation_info info = {0};
    info.port = CONTEXT_PORT_NO_LISTEN;  // 客户端不监听
    info.protocols = protocols;
    info.user = &app;
    
    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    // 2. 发起连接
    struct lws_client_connect_info conn_info = {0};
    conn_info.context = context;
    conn_info.address = "echo.websocket.org";
    conn_info.port = 80;
    conn_info.path = "/";
    conn_info.host = "echo.websocket.org";
    conn_info.origin = "echo.websocket.org";
    conn_info.protocol = protocols[0].name;
    
    struct lws *wsi = lws_client_connect_via_info(&conn_info);
    if (!wsi) {
        fprintf(stderr, "Failed to connect\n");
        lws_context_destroy(context);
        return 1;
    }
    
    // 3. 服务循环
    while (app.running) {
        lws_service(context, 100);
    }
    
    // 4. 清理
    lws_context_destroy(context);
    return 0;
}
```

### 5.2 服务器端完整示例

```c
#include <libwebsockets.h>
#include <stdio.h>
#include <string.h>

// 会话数据
typedef struct {
    int connected;
} session_data_t;

// 回调函数
static int callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len)
{
    session_data_t *session = (session_data_t *)user;
    
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        printf("Client connected\n");
        session->connected = 1;
        break;
        
    case LWS_CALLBACK_RECEIVE:
        printf("Received: %.*s\n", (int)len, (char *)in);
        
        // Echo back（必须使用带 LWS_PRE 空间的独立缓冲区，不能直接转发 in 指针）
        {
            unsigned char *echo_buf = malloc(LWS_PRE + len);
            if (echo_buf) {
                memcpy(echo_buf + LWS_PRE, in, len);
                lws_write(wsi, echo_buf + LWS_PRE, len, LWS_WRITE_TEXT);
                free(echo_buf);
            }
        }
        break;
        
    case LWS_CALLBACK_CLOSED:
        printf("Client disconnected\n");
        session->connected = 0;
        break;
        
    default:
        break;
    }
    
    return 0;
}

// 协议定义
static struct lws_protocols protocols[] = {
    {
        .name = "my-protocol",
        .callback = callback,
        .per_session_data_size = sizeof(session_data_t),
        .rx_buffer_size = 4096,
    },
    { NULL, NULL, 0, 0 }
};

int main(void)
{
    // 1. 创建 context（同时创建默认 vhost）
    struct lws_context_creation_info info = {0};
    info.port = 8080;  // 监听端口
    info.protocols = protocols;
    
    struct lws_context *context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    printf("Server listening on port 8080\n");
    
    // 2. 服务循环
    while (1) {
        lws_service(context, 100);
    }
    
    // 3. 清理
    lws_context_destroy(context);
    return 0;
}
```

### 5.3 多线程服务示例

```c
// 创建支持多线程的 context
struct lws_context_creation_info info = {0};
info.port = 8080;
info.protocols = protocols;
info.count_threads = 4;  // 4 个服务线程

struct lws_context *context = lws_create_context(&info);

// lws 会自动创建 4 个线程，每个线程独立处理一部分连接
// 需要注意线程安全问题，使用 lws_context_user 等 API 时要加锁
```

---

## 6. 关键设计要点

### 6.1 内存管理

#### Per-Session 数据
```c
// 在协议定义中指定大小
.per_session_data_size = sizeof(my_session_data_t)

// 在回调中直接访问
my_session_data_t *session = (my_session_data_t *)user;
```

#### Context 用户数据
```c
// 创建时设置
info.user = &my_app_data;

// 回调中获取
void *app = lws_context_user(lws_get_context(wsi));
```

#### Vhost 用户数据
```c
// 创建时设置
vh_info.user = &my_vhost_data;

// 获取
void *vh_data = lws_vhost_user(vhost);
```

### 6.2 线程安全

**原则：**
- 每个 wsi 只在一个服务线程中被处理
- 跨线程操作需要使用 `lws_cancel_service()`
- 访问共享数据时需要加锁

**示例：**
```c
// 线程 A：添加消息到队列
pthread_mutex_lock(&queue_lock);
add_to_queue(msg);
pthread_mutex_unlock(&queue_lock);

// 通知 lws 线程处理
lws_cancel_service(context);

// 线程 B（lws 服务线程）：在 WRITEABLE 回调中发送
case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
    pthread_mutex_lock(&queue_lock);
    msg = get_from_queue();
    pthread_mutex_unlock(&queue_lock);
    
    if (msg) {
        lws_callback_on_writable(wsi);
    }
    break;
```

### 6.3 错误处理

```c
// 连接失败（case 内有变量声明时需用大括号包裹）
case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
    const char *error = in ? (char *)in : "unknown";
    fprintf(stderr, "Connection failed: %s\n", error);
    // 可以实现重连逻辑
    break;
}

// 发送失败
int n = lws_write(wsi, buf, len, LWS_WRITE_TEXT);
if (n < 0) {
    fprintf(stderr, "Write failed\n");
    return -1;  // 关闭连接
}

// 一般错误
if (some_error_condition) {
    return -1;  // 非零返回值会关闭连接
}
```

### 6.4 性能优化

#### 批量发送
```c
// 不要每次都调用 lws_callback_on_writable
// 而是累积消息，一次性触发
if (has_more_messages) {
    lws_callback_on_writable(wsi);  // 连续触发
}
```

#### 缓冲区复用
```c
// 使用静态缓冲区，避免频繁 malloc/free
static unsigned char send_buf[LWS_PRE + MAX_MSG_SIZE];
```

#### 合理设置超时
```c
// 根据应用场景调整超时
info.timeout_secs = 30;           // 连接超时
info.ka_time = 60;                // TCP keepalive
vh_info.keepalive_timeout = 5;    // HTTP keepalive
```

### 6.5 SSL/TLS 注意事项

```c
// 客户端启用 SSL
conn_info.ssl_connection = LCCSCF_USE_SSL;

// 跳过证书验证（仅用于测试）
conn_info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED |
                            LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                            LCCSCF_ALLOW_EXPIRED;

// 服务器端配置证书
vh_info.ssl_cert_filepath = "/path/to/cert.pem";
vh_info.ssl_private_key_filepath = "/path/to/key.pem";
vh_info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
```

### 6.6 调试技巧

#### 启用日志
```c
// 设置日志级别
lws_set_log_level(LLL_ERR | LLL_WARN | LLL_INFO, NULL);

// 自定义日志回调
void custom_log(int level, const char *line) {
    fprintf(stderr, "[LWS %d] %s", level, line);
}
lws_set_log_level(LLL_DEBUG, custom_log);
```

#### 查看连接状态
```c
// 生成 JSON 格式的状态信息
char buf[4096];
lws_json_dump_context(context, buf, sizeof(buf), 0);
printf("%s\n", buf);
```

---

## 附录：常用宏和常量

```c
// 端口常量
#define CONTEXT_PORT_NO_LISTEN       -1  // 客户端或不监听
#define CONTEXT_PORT_NO_LISTEN_SERVER -2 // 服务器使用 sock_adopt

// SSL 标志
#define LCCSCF_USE_SSL               // 启用 SSL
#define LCCSCF_ALLOW_SELFSIGNED      // 允许自签名证书
#define LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK  // 跳过主机名检查
#define LCCSCF_ALLOW_EXPIRED         // 允许过期证书

// 写入类型
#define LWS_WRITE_TEXT               // 文本消息
#define LWS_WRITE_BINARY             // 二进制消息
#define LWS_WRITE_PING               // Ping
#define LWS_WRITE_PONG               // Pong
#define LWS_WRITE_CONTINUATION       // 连续帧

// 日志级别
#define LLL_ERR     (1 << 0)  // 错误
#define LLL_WARN    (1 << 1)  // 警告
#define LLL_NOTICE  (1 << 2)  // 通知
#define LLL_INFO    (1 << 3)  // 信息
#define LLL_DEBUG   (1 << 4)  // 调试
```

---

## 参考资源

- **官方文档**: https://libwebsockets.org/
- **GitHub**: https://github.com/warmcat/libwebsockets
- **API 参考**: https://libwebsockets.org/lws-api-doc-master/html/
- **示例代码**: libs/libwebsockets/minimal-examples/

---

**文档版本**: 1.0  
**最后更新**: 2026-04-21  
**适用 libwebsockets 版本**: 4.x
