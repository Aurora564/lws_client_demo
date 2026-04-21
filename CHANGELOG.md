# 更新日志

## [1.1.0] - 2026-04-21

### 新增功能

#### ✨ WSS (WebSocket Secure) 支持

现在客户端支持加密的 WebSocket 连接（WSS），可根据业务环境灵活选择 WS 或 WSS。

**新增 API：**
```c
// 启用/禁用 SSL/TLS
void wsl_set_ssl(wsl_client_t *c, int enabled, int skip_verify);
```

**使用示例：**
```c
// 生产环境：严格验证证书
wsl_set_ssl(client, 1, 0);

// 开发环境：跳过证书验证（支持自签名证书）
wsl_set_ssl(client, 1, 1);
```

#### ✨ 自定义 WebSocket 路径

支持设置自定义连接路径，不再局限于根路径 `/`。

**新增 API：**
```c
// 设置连接路径
void wsl_set_path(wsl_client_t *c, const char *path);
```

**使用示例：**
```c
wsl_set_path(client, "/ws/chat");
wsl_set_path(client, "/api/v1/websocket");
```

### 改进

- **证书验证降级**：允许在开发/测试环境中跳过证书验证，支持自签名证书
- **向后兼容**：所有现有代码无需修改，默认行为保持不变（WS 模式）
- **示例程序增强**：example.c 支持命令行参数 `--wss` 和 `--skip-verify`

### 文档

- 新增 `WSS_USAGE.md` - 完整的 WSS 使用指南
- 更新 `BUILD.md` - 添加 WSS 配置示例
- 更新 `QUICKSTART.md` - 标注新特性

### 技术细节

**SSL/TLS 配置标志：**
- `LCCSCF_USE_SSL` - 启用 SSL/TLS
- `LCCSCF_ALLOW_SELFSIGNED` - 允许自签名证书
- `LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK` - 跳过主机名验证
- `LCCSCF_ALLOW_EXPIRED` - 允许过期证书

**内部结构变化：**
```c
struct wsl_client {
    // ... 原有字段 ...
    char *path;              // 新增：WebSocket 路径
    int use_ssl;             // 新增：是否启用 SSL
    int ssl_skip_verify;     // 新增：是否跳过证书验证
};
```

### 兼容性

- ✅ 完全向后兼容
- ✅ 现有 WS 代码无需任何修改
- ✅ 编译选项不变
- ✅ ABI 兼容（结构体扩展不影响现有字段）

### 测试

已通过以下场景测试：
- WS 连接（默认模式）
- WSS 连接（严格验证）
- WSS 连接（跳过证书验证）
- 自定义路径连接
- 组合配置测试

---

## [1.0.0] - 2026-04-10

### 初始版本

- 基础 WebSocket 客户端实现
- 自动重连（指数退避）
- 心跳检测（Ping/Pong）
- 线程安全发送队列
- 分片消息重组
- 异步回调机制
