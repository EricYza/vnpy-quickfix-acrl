# Acceptor 侧直接证据

本实验在 acceptor 侧直接记录 `poll()`、`recv()` 和 Parser 完整消息数。双方 Logon 完成后清零计数，server 收满 10 万条 application messages 后停止计数，因此不包含 initiator 网络线程和握手阶段。

`poll_blocking_returns` 的判定不是时间阈值：每次 `poll()` 前后读取 acceptor 网络线程的 `ru_nvcsw`，只要 voluntary context switch 增长，就证明本次调用期间线程主动让出过 CPU。

## 三次实验的总计数/平均值

| 指标 | `validate=no` + blocking | `validate=yes` + blocking | `validate=no` + busy-poll |
|---|---:|---:|---:|
| 平均 seconds | 0.659418 | 0.592960 | 0.566850 |
| 平均 poll calls | 78,906 | 55,393 | 237,230 |
| 平均 poll wait 总时长 | 187,779,307ns | 67,323,923ns | 63,085,382ns |
| 平均 blocking poll returns | 10,326 | 1,012 | 0 |
| 平均 recv calls | 78,907 | 55,394 | 97,008 |
| 聚合 messages/recv | 1.267 | 1.805 | 1.031 |
| 聚合 bytes/recv | 202.49 | 288.44 | 164.71 |
| 平均 voluntary context switches | 10,518 | 1,241 | 185 |
| 平均 system CPU seconds | 0.527 | 0.443 | 1.247 |

## 对结论的直接验证

与 `validate=yes` 相比，`validate=no` 的单条解析更快，但它在 blocking-poll 流水线中出现：

- `poll_calls` 多 42.4%。
- `poll_wait_nanoseconds` 总量是 2.79 倍。
- `poll_blocking_returns` 是 10.2 倍。
- `recv_calls` 多 42.4%。
- `messages_per_recv` 从 1.805 降到 1.267，低约 29.8%。
- `average_bytes_per_recv` 从 288.44 降到 202.49，低约 29.8%。

这直接验证了之前的机制判断：`validate=no` 更快清空当前数据，导致 acceptor 更早返回 `poll()`，收到的批次更碎，并更频繁地阻塞等待下一小批数据；`validate=yes` 在处理当前消息期间允许 client 继续供数，socket receive queue 自然积累出更大的批次。

同一个 `validate=no` 切换到 busy-poll 后：

- 三次实验的 `poll_blocking_returns` 都为 0。
- benchmark 时间相对 blocking-poll 降低约 14.0%。
- 进程级 voluntary context switches 降低约 98.2%。
- `poll_calls` 增加到约 3 倍，system CPU 也明显增加，说明它通过持续轮询换取不睡眠。

因此，阻塞 `poll()` 的睡眠/唤醒和碎片化 recv 是性能反转的主要原因。busy-poll 能消除这部分等待，但会增加 CPU 消耗。

## 计数边界说明

blocking-poll 的每组数据中，`recv_calls` 比 `poll_calls` 多 1。原因是启用统计时 acceptor 已经位于一次在 Logon 后开始的阻塞 `poll()` 内：这次 poll 的起点不在计数窗口内，但它唤醒后的 recv 在窗口内。十万条测试只差一个边界调用，不影响比例结论。

诊断本身会在每次 `poll()` 前后调用 `getrusage(RUSAGE_THREAD)` 并更新原子计数，因此本文件中的 seconds 用于同一诊断构建内的相对比较，最终吞吐率仍应使用 `QUICKFIX_NETWORK_DIAGNOSTICS=OFF` 的构建。
