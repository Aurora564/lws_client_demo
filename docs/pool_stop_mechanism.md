# Pool 停止机制分析与修复

## 问题现象

`pool_basic` 示例在 `wsp_pool_destroy` 处永久挂死，无法退出。

复现条件：
- echo_server 运行在 8080 端口
- pool_basic 连接 echo_server，正常收发消息
- 发送 SIGTERM（timeout 或 Ctrl+C）触发停止流程
- 程序卡在 `wsp_pool_destroy`，无响应

strace 定位到的挂死位置：

```
# 主线程 — 停在 condvar 等待
179524 futex(FUTEX_WAIT_BITSET_PRIVATE, 0, NULL, ...)

# 服务线程 — 正常 poll 循环
179525 poll([{fd=4, POLLIN}, {fd=5, POLLIN}], 2, 29999)
```

主线程停在 `wsp_client_stop` 中的 `pthread_cond_wait`，等待 `LWS_CALLBACK_CLIENT_CLOSED` 回调发送 `pthread_cond_signal`。服务线程的 poll 一直有数据可读（echo 响应），但 `CLOSED` 始终不触发。

## 根因分析

### WebSocket 关闭握手时序

WebSocket 关闭需要四次交互：

```
客户端                         服务端
  │                             │
  │──── CLOSE (close frame) ────→│  ① 客户端发起关闭
  │                             │
  │←─── CLOSE (close frame) ────│  ② 服务端回 close 帧
  │                             │
  │──── CLOSE (TCP ACK) ────────→│  ③ 客户端确认
  │                             │
  │  LWS_CALLBACK_CLIENT_CLOSED │  ④ lws 触发 CLOSED 回调
```

只有当服务端回 close 帧后，lws 才会触发 `LWS_CALLBACK_CLIENT_CLOSED`。如果服务端不回，`CLOSED` 永远不会触发。

### TCP buffer 积压导致无限等待的链条

当 `timeout` 发送 `SIGTERM` 时，主循环立即退出并调用 `wsp_pool_destroy`：

```
  时间线
  ──────
  t=0  主线程收到 SIGTERM, g_running = 0
  t=0  主线程退出 while 循环
  t=0  wsp_pool_destroy → wsp_client_stop
  t=0  stopping = 1, lws_cancel_service(ctx)
  t=0  服务线程: WAIT_CANCELLED 回调
       → lws_close_reason(wsi)
       → lws_callback_on_writable(wsi)
  t=0+ 服务线程: WRITEABLE 发出 close 帧 ─────→ 服务端
  t=0+ 服务线程: 回到 poll 等待事件
  t=ε  服务端收到 close 帧

  问题出在这里:
  服务端在收到 close 帧之前, 已经发了 echo 数据在 TCP 中:
  ┌──── TCP receive buffer ────────────────────┐
  │ [echo "ping #5"] [echo "ping #6"] [close] │
  └────────────────────────────────────────────┘
                                            ↑
                                      close 帧排在最后

  t=ε  服务端处理 close 帧, 发回 close 帧 ───→ 客户端
  t=ε  客户端的 TCP receive buffer:
  ┌──── TCP receive buffer ────────────────────┐
  │ [echo "ping #5"] [close from server]       │
  └────────────────────────────────────────────┘
                                ↑
                          还在排队等待读取

  t=ε+ 服务线程: poll 返回 POLLIN
  t=ε+ 服务线程: lws 读取 echo 数据
       → LWS_CALLBACK_CLIENT_RECEIVE (非 CLOSED!)
  t=ε+ 服务线程: 再次 poll
  t=ε+ 服务线程: poll 返回 POLLIN
  t=ε+ 服务线程: lws 读取 close 帧
       → LWS_CALLBACK_CLIENT_CLOSED  🎉
  t=ε+ CLOSED 回调: stopped = 1
       pthread_cond_signal(&stop_cond)
  t=ε+ 主线程: condvar 返回, 停止流程继续
```

**关键延迟点：** 从 `lws_cancel_service` 到 `CLOSED` 回调，需要经历：
1. 服务线程从 poll 中唤醒
2. 处理 WAIT_CANCELLED 关闭 wsi
3. lws 发出 close 帧
4. 服务端接收 close 帧（**但 TCP buffer 里还有积压 echo 数据**）
5. 服务端处理完积压数据后才读取 close 帧
6. 服务端发回 close 帧
7. 客户端接收 close 帧
8. lws 触发 CLOSED 回调

当服务端持续发送数据、TCP buffer 持续积压时，步骤 4-5 可能被无限推迟。

### 本质原因

`pthread_cond_wait` 依赖外部事件（服务端行为）才能完成等待。当服务端不配合（不回应 close 帧）、或 TCP buffer 积压延迟了 close 帧处理时，condvar 裸等外部事件的模式会导致永久阻塞。

这个问题不限于 WebSocket：
- TCP 的 `SO_LINGER` 设置为永久等待时同样问题
- 任何依赖对端配合的关闭协议都需要超时保护

## 修复方案

将无限制的 `pthread_cond_wait` 替换为带超时的 `pthread_cond_timedwait`：

```c
struct timespec ts;
clock_gettime(CLOCK_REALTIME, &ts);
ts.tv_sec += WSP_STOP_TIMEOUT_SEC;  // 3 秒超时

while (!c->stopped) {
    int rc = pthread_cond_timedwait(&c->stop_cond, &c->stop_mu, &ts);
    if (rc == ETIMEDOUT) {
        fprintf(stderr, "[wsp] stop timeout on %s:%d, forcing exit\n",
                c->host, c->port);
        break;  // 超时后强制退出
    }
}
```

超时后，`wsp_client_stop` 退出等待，`wsp_pool_destroy` 继续执行后面的 `lws_context_destroy`，由 context 销毁逻辑清理残余的 wsi。

### 超时宏

```c
// src/ws_pool.h
#define WSP_STOP_TIMEOUT_SEC 3
```

默认值 3 秒：
- 远长于正常 WebSocket 关闭握手（通常 < 100ms）
- 当服务端无响应时也不会让用户等待太久
- 可通过编译期宏由业务层按需调整

## 为什么 P2-A 旧 build 没有复现

调试过程中发现一个令人困惑的现象：

- `git checkout df0fa99`（P2-A 提交）+ 旧 build 目录 → pool_basic 正常退出
- 同一份源码 + `rm -rf build && cmake -B build` 全新构建 → pool_basic 挂死

根因在于旧 build 目录掩盖了问题：

`git checkout df0fa99` 恢复源码时的文件时间戳早于旧 build 中 `ws_pool.c.o` 的时间戳，cmake 认为源码未变更，**跳过了重编译**。实际跑的是之前 P2-B 版本编译的 `.o` 文件，与 P2-A 源码不同。

全新构建后，所有文件都正确编译，pool 的 condvar 等待暴露出预存的 stop 时序问题。

### 这是 pool 的预存问题，非 P2-B 引入

查看 `faa74a2 fix: fix TOCTOU data race on c->wsi in wsp_client_stop` 可知，condvar 模式的停止同步自 pool 第一版就已存在。P2-B 只是修改了 `ws_pool.h` 的 include 依赖，导致 cmake 依赖追踪变化后触发了完整重编译，暴露了原本被旧对象文件掩盖的问题。

## 经验总结

### 模式识别

所有依赖"对端协作"才能完成的关闭操作都需要超时保护：

| 场景 | 依赖 | 风险 | 保护 |
|------|------|------|------|
| WebSocket close | 服务端回 close 帧 | 服务端不配合/有延迟 | timedwait |
| TCP SO_LINGER | 对端 ACK | 对端不响应 | timeo |
| TLS shutdown | 对端 close_notify | 对端不发送 | 超时 |
| HTTP keepalive close | 对端 FIN | 对端保持连接 | idle timeout |

### 实现约束

- `pthread_cond_wait` 裸等外部事件是危险模式
- 使用 `pthread_cond_timedwait` + `ETIMEDOUT` 检查作为兜底
- 超时不是错误处理，而是死亡检测 —— 超时后应继续执行清理
- 后续的 `lws_context_destroy` / `free` 等能安全处理残余状态

### 与 v1/v2 对比

v1 和 v2 使用 `pthread_join` 而不是 condvar 来同步服务线程退出，不存在此问题。v2 的 `wsl_stop`：

```c
void wsl_stop(wsl_client_t *c) {
    c->stopping = 1;
    lws_cancel_service(c->ctx);
    pthread_join(c->thread, NULL);    // 等待线程自然退出
    lws_context_destroy(c->ctx);
}
```

pool 由于多个客户端共享一个服务线程，不能直接 join 线程 —— 需要单独停止每个客户端再销毁线程，因此引入了 condvar 同步模式。这个设计本身就比 v1/v2 脆弱，需要额外的超时保护。
