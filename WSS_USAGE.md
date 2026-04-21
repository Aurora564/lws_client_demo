# WSS (WebSocket Secure) 使用指南

## 概述

本客户端支持 **WS** (明文) 和 **WSS** (加密) 两种协议，可根据业务环境灵活选择。

## 快速开始

### 1. WS 模式（默认）

```c
wsl_client_t *client = wsl_create("localhost", 8080, "my-protocol", callback, NULL);
// 无需额外配置，默认使用 WS
wsl_start(client);
```

### 2. WSS 模式（生产环境）

```c
wsl_client_t *client = wsl_create("api.example.com", 443, "v1", callback, NULL);

// 启用 SSL/TLS，严格验证证书
wsl_set_ssl(client, 1, 0);

wsl_start(client);
```

### 3. WSS + 跳过证书验证（开发/测试）

```c
wsl_client_t *client = wsl_create("test.local", 8443, "dev", callback, NULL);

// 启用 SSL/TLS，跳过证书验证（支持自签名证书）
wsl_set_ssl(client, 1, 1);

wsl_start(client);
```

## API 详解

### wsl_set_ssl()

```c
void wsl_set_ssl(wsl_client_t *c, int enabled, int skip_verify);
```

**参数说明：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `c` | `wsl_client_t*` | 客户端实例 |
| `enabled` | `int` | 1 = 启用 WSS, 0 = 使用 WS |
| `skip_verify` | `int` | 1 = 跳过证书验证, 0 = 严格验证 |

**注意事项：**
- 必须在 `wsl_start()` **之前**调用
- `skip_verify=1` 仅用于开发/测试环境
- 生产环境应使用有效证书并保持 `skip_verify=0`

### wsl_set_path()

```c
void wsl_set_path(wsl_client_t *c, const char *path);
```

**参数说明：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `c` | `wsl_client_t*` | 客户端实例 |
| `path` | `const char*` | WebSocket 路径，如 `/ws`, `/api/chat` |

**默认值：** `/`

## 完整示例

```c
#include "ws_client.h"
#include <stdio.h>

void on_message(const char *data, size_t len, void *user) {
    printf("收到: %.*s\n", (int)len, data);
}

int main() {
    // ===== 示例 1: 连接公共 WSS 服务 =====
    wsl_client_t *client1 = wsl_create(
        "echo.websocket.events",  // WSS 服务器
        443,                       // WSS 端口
        "",                        // 无特定协议
        on_message,
        NULL
    );

    // 启用 WSS，严格验证证书
    wsl_set_ssl(client1, 1, 0);
    wsl_set_path(client1, "/");

    if (wsl_start(client1)) {
        wsl_send(client1, "Hello WSS!", 10);
    }

    // ===== 示例 2: 连接本地测试服务器（自签名证书）=====
    wsl_client_t *client2 = wsl_create(
        "localhost",
        8443,
        "test-protocol",
        on_message,
        NULL
    );

    // 启用 WSS，跳过证书验证
    wsl_set_ssl(client2, 1, 1);
    wsl_set_path(client2, "/ws/test");

    if (wsl_start(client2)) {
        wsl_send(client2, "Test message", 12);
    }

    // ... 业务逻辑 ...

    wsl_destroy(client1);
    wsl_destroy(client2);
    return 0;
}
```

## 证书验证模式对比

| 模式 | 适用场景 | 安全性 | 配置 |
|------|---------|--------|------|
| **WS** | 内网/本地开发 | 低（明文） | 默认 |
| **WSS + 严格验证** | 生产环境 | 高 | `wsl_set_ssl(c, 1, 0)` |
| **WSS + 跳过验证** | 开发/测试 | 中（加密但无身份验证） | `wsl_set_ssl(c, 1, 1)` |

## 常见问题

### Q1: 什么时候需要跳过证书验证？

**答：** 以下情况可以跳过证书验证：
- 使用自签名证书的测试环境
- 内部网络的私有 CA 证书
- 开发阶段的本地 HTTPS 服务

**注意：** 生产环境必须使用有效证书并开启严格验证。

### Q2: WSS 连接失败的可能原因？

**答：** 常见原因包括：
1. 证书过期或无效
2. 主机名不匹配
3. 缺少根证书
4. 防火墙阻止 443 端口
5. 服务器不支持 WSS

**调试方法：**
```bash
# 测试 WSS 连接
openssl s_client -connect example.com:443

# 查看证书信息
openssl x509 -in cert.pem -text -noout
```

### Q3: 如何从 WS 迁移到 WSS？

**答：** 只需添加一行代码：
```c
// 原有 WS 代码
wsl_client_t *c = wsl_create("example.com", 80, "app", callback, NULL);

// 改为 WSS（只需添加这一行）
wsl_set_ssl(c, 1, 0);  // ← 新增

wsl_start(c);
```

同时修改端口（通常 80 → 443）。

## 安全建议

1. **生产环境**始终使用 WSS + 严格验证
2. **不要**在代码中硬编码 `skip_verify=1`
3. 通过配置文件或环境变量控制证书验证行为
4. 定期更新系统和 OpenSSL 库
5. 监控证书到期时间，及时续期

## 性能影响

WSS 相比 WS 的性能差异：
- **握手阶段**：增加 ~1-2 RTT（TLS 握手）
- **数据传输**：加密开销 < 5%（现代 CPU 有 AES-NI 加速）
- **内存占用**：增加 ~16KB（TLS 上下文）

对于大多数应用场景，性能差异可忽略不计。

## 更多资源

- [libwebsockets 官方文档](https://libwebsockets.org/)
- [RFC 6455 - The WebSocket Protocol](https://tools.ietf.org/html/rfc6455)
- [RFC 8446 - TLS 1.3](https://tools.ietf.org/html/rfc8446)
