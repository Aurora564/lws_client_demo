# 快速开始

## 一键编译和运行

```bash
# 1. 克隆项目（包含子模块）
git clone --recursive <repository-url>
cd lws_client_demo

# 2. 编译依赖库
./build_deps.sh

# 3. 编译项目
./build.sh

# 4. 编译示例程序
gcc example.c -I./output/include -I./output/include/libwebsockets \
    -I./src -L./build/app -lwsclient \
    -L./output/lib -lwebsockets \
    -lpthread -lssl -lcrypto -lz -lm \
    -o example

# 5. 运行示例
export LD_LIBRARY_PATH=./output/lib:$LD_LIBRARY_PATH
./example localhost 8080 my-protocol
```

## 核心 API

| 函数 | 说明 |
|------|------|
| `wsl_create()` | 创建客户端实例 |
| `wsl_start()` | 启动连接和服务线程 |
| `wsl_send()` | 发送文本消息 |
| `wsl_send_binary()` | 发送二进制消息 |
| `wsl_destroy()` | 停止并清理资源 |
| `wsl_set_ping()` | 设置心跳参数 |
| `wsl_set_reconnect()` | 设置重连参数 |
| `wsl_set_queue_limit()` | 设置队列限制 |
| `wsl_set_ssl()` | **启用 SSL/TLS (WS/WSS)** ⭐ |
| `wsl_set_path()` | **设置自定义路径** ⭐ |

## 特性

- ✅ 自动重连（指数退避）
- ✅ 心跳检测（Ping/Pong）
- ✅ 线程安全发送
- ✅ 消息队列流控
- ✅ 分片消息重组
- ✅ 异步回调机制
- ✅ **支持 WS 和 WSS** ⭐ 新增
- ✅ **支持自签名证书（开发模式）** ⭐ 新增
- ✅ **自定义 WebSocket 路径** ⭐ 新增

## 更多信息

详细文档请查看 [BUILD.md](BUILD.md)
