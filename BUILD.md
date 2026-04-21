# 编译说明

本项目提供两种编译方式：**Shell 脚本** 和 **CMake**。

## 项目结构

```
lws_client_demo/
├── src/                    # 源代码
│   ├── ws_client.c        # WebSocket 客户端实现
│   ├── ws_client.h        # 客户端 API 头文件
│   ├── ws_server.c        # WebSocket 服务端（占位）
│   └── ws_server.h        # 服务端头文件（占位）
├── libs/                   # 第三方库源码
│   └── libwebsockets/     # libwebsockets (git submodule)
├── output/                 # 依赖库安装目录
│   ├── include/           # libwebsockets 头文件
│   └── lib/               # libwebsockets 库文件
├── build/                  # 构建输出目录
│   ├── libwebsockets/     # libwebsockets CMake 构建
│   ├── app/               # 应用构建输出（shell 脚本）
│   └── cmake/             # CMake 构建输出
├── build_deps.sh          # 依赖库编译脚本
├── build.sh               # 项目编译脚本（推荐）
├── CMakeLists.txt         # CMake 配置文件
└── example.c              # 使用示例
```

## 前置条件

确保系统已安装以下工具和库：

```bash
# Fedora/RHEL
sudo dnf install gcc make cmake openssl-devel zlib-devel git

# Ubuntu/Debian
sudo apt-get install gcc make cmake libssl-dev zlib1g-dev git
```

## 编译步骤

### 方法一：使用 Shell 脚本（推荐）

#### 1. 编译依赖库

```bash
./build_deps.sh
```

这会编译 libwebsockets 并安装到 `output/` 目录。

#### 2. 编译项目

```bash
./build.sh
```

编译成功后会生成：
- 静态库：`build/app/libwsclient.a`
- pkg-config 文件：`output/lib/pkgconfig/wsclient.pc`

### 方法二：使用 CMake

#### 1. 编译依赖库（同上）

```bash
./build_deps.sh
```

#### 2. 配置和编译

```bash
mkdir -p build/cmake
cd build/cmake
cmake ../.. -DCMAKE_BUILD_TYPE=Release
make
```

编译成功后会生成：
- 静态库：`build/cmake/app/libwsclient.a`
- 示例程序：`build/cmake/example`

## 使用示例

### 编译您的应用程序

**方式 1：直接链接**

```bash
gcc your_app.c \
    -I./output/include \
    -I./output/include/libwebsockets \
    -I./src \
    -L./build/app -lwsclient \
    -L./output/lib -lwebsockets \
    -lpthread -lssl -lcrypto -lz -lm \
    -o your_app
```

**方式 2：使用 pkg-config**

```bash
export PKG_CONFIG_PATH=./output/lib/pkgconfig:$PKG_CONFIG_PATH
gcc your_app.c $(pkg-config --cflags --libs wsclient) -o your_app
```

**方式 3：使用 CMake**

在您的 `CMakeLists.txt` 中添加：

```cmake
# 添加子目录或查找库
add_subdirectory(path/to/lws_client_demo)

# 链接到您的目标
target_link_libraries(your_target PRIVATE wsclient)
```

### 运行示例程序

```bash
# 设置库搜索路径
export LD_LIBRARY_PATH=./output/lib:$LD_LIBRARY_PATH

# 运行示例（需要 WebSocket 服务器）
./example localhost 8080 my-protocol

# WSS 模式（启用 SSL/TLS）
./example example.com 443 chat --wss

# WSS + 跳过证书验证（开发/测试环境）
./example test.local 8443 api --wss --skip-verify /ws/v1
```

## API 快速参考

### 基础用法

```c
#include "ws_client.h"

// 1. 创建客户端
wsl_client_t *client = wsl_create(
    "localhost",      // 服务器地址
    8080,             // 端口
    "my-protocol",    // 协议名称
    on_message,       // 接收回调
    user_data         // 用户数据
);

// 2. 配置参数（可选）
wsl_set_ping(client, 30000, 10000);        // 心跳：30s 间隔，10s 超时
wsl_set_reconnect(client, 1000, 60000);    // 重连：1s 初始，60s 最大
wsl_set_queue_limit(client, 100, 1048576); // 队列：100条消息或1MB

// 3. 启动客户端
if (!wsl_start(client)) {
    // 处理错误
}

// 4. 发送消息
wsl_send(client, "Hello", 5);              // 文本消息
wsl_send_binary(client, data, len);        // 二进制消息

// 5. 停止并清理
wsl_destroy(client);
```

### WSS (WebSocket Secure) 用法

```c
// 创建客户端
wsl_client_t *client = wsl_create(
    "example.com",     // HTTPS 域名
    443,               // WSS 默认端口
    "chat",            // 协议名称
    on_message,
    user_data
);

// 启用 SSL/TLS
// 参数2: skip_verify
//   0 = 严格验证证书（生产环境推荐）
//   1 = 跳过证书验证（开发/测试环境，支持自签名证书）
wsl_set_ssl(client, 1, 0);  // 启用 WSS，严格验证
// wsl_set_ssl(client, 1, 1);  // 启用 WSS，跳过验证（开发用）

// 设置自定义路径（可选，默认为 "/"）
wsl_set_path(client, "/ws/chat");

// 启动客户端
wsl_start(client);
```

### 使用场景示例

**场景 1: 连接本地开发服务器（WS）**
```c
wsl_client_t *c = wsl_create("localhost", 8080, "app", callback, NULL);
wsl_start(c);
```

**场景 2: 连接生产环境 WSS（严格验证）**
```c
wsl_client_t *c = wsl_create("api.example.com", 443, "v1", callback, NULL);
wsl_set_ssl(c, 1, 0);  // 启用 SSL，严格验证证书
wsl_set_path(c, "/websocket");
wsl_start(c);
```

**场景 3: 连接测试环境 WSS（自签名证书）**
```c
wsl_client_t *c = wsl_create("test.local", 8443, "dev", callback, NULL);
wsl_set_ssl(c, 1, 1);  // 启用 SSL，跳过证书验证
wsl_start(c);
```

## 故障排除

### 问题：找不到 libwebsockets

```
错误: libwebsockets not found. Please run build_deps.sh first.
```

**解决：** 先运行 `./build_deps.sh` 编译依赖库。

### 问题：运行时找不到共享库

```
error while loading shared libraries: libwebsockets.so.17
```

**解决：** 设置 `LD_LIBRARY_PATH` 环境变量：

```bash
export LD_LIBRARY_PATH=./output/lib:$LD_LIBRARY_PATH
```

或在编译时添加 `-Wl,-rpath,<path>` 选项。

### 问题：链接错误

```
undefined reference to `SSL_library_init'
```

**解决：** 确保链接了所有必需的库：

```bash
-lwebsockets -lpthread -lssl -lcrypto -lz -lm
```

## 清理构建

```bash
# 清理应用构建
rm -rf build/app
rm -rf build/cmake

# 清理依赖构建（需要重新下载）
rm -rf build/libwebsockets
rm -rf output/*
```

## 许可证

本项目代码遵循您选择的许可证。libwebsockets 遵循 LGPLv2.1 + 静态链接例外。
