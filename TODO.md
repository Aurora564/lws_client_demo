# TODO

## P0 - Pool 停止逻辑重构

当前 `wsp_client_stop` 的 condvar 等待 CLOSED 模式有两个路径：
- `wsp_pool_destroy` 调用（有 `lws_context_destroy` 兜底）
- 用户单独调用 `wsp_client_stop`（无兜底）

目前的 timedwait 修复合了 `wsp_pool_destroy` 路径，但 `wsp_client_stop` 单独调用时 3 秒超时仍不能保证客户端已真正断开。

### 理想方案

```c
wsp_pool_destroy():
    对所有客户端设 stopping = 1
    取消所有 sul 定时器
    pool->destroying = 1, lws_cancel_service
    pthread_join(service 线程)       ← 不等待 CLOSED，直接 join
    lws_context_destroy(ctx)         ← lws 自己清理残余 wsi
    // 不需要 condvar，不需要超时

wsp_client_stop():
    改为纯异步：设 stopping = 1，立即返回
    或增加完成回调参数（停止完成后通知）
    // 不需要 condvar，不需要超时
```

### 改动范围

- `src/ws_pool.c`:
  - `wsp_pool_destroy`：去掉 `wsp_client_stop` 调用，直接设 flags + cancel_service + join + context_destroy
  - `wsp_client_stop`：改为异步非阻塞，或增加回调参数
  - 删除 `stop_mu` / `stop_cond` / `stopped` 相关代码（如果异步化）
- `src/ws_pool.h`:
  - 更新 `wsp_client_stop` 文档注释，说明行为变更
- `examples/`:
  - 检查使用 `wsp_client_stop` 的示例是否需要适配
- `docs/pool_stop_mechanism.md`:
  - 更新文档，说明新方案消除了 condvar 依赖

### 风险

- `lws_context_destroy` 是否能可靠清理关闭中的 wsi？需实测验证
- 异步化的 `wsp_client_stop` 可能破坏现有调用方假设（原为同步阻塞直到关闭完成）
