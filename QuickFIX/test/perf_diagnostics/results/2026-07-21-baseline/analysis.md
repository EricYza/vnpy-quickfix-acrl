# 2026-07-21 baseline 诊断结果

## QuickFIX client 三次重复

20 万条 `new-order-single`，不开 busy-poll，不绑核：

| 指标 | `validate=yes` 平均 | `validate=no` 平均 | `no / yes` |
|---|---:|---:|---:|
| benchmark seconds | 1.115531 | 1.525554 | 1.368 |
| system CPU seconds | 0.803 | 1.237 | 1.539 |
| voluntary context switches | 1,539 | 32,511 | 21.13 |
| involuntary context switches | 51 | 106 | 2.08 |

`validate=no` 的字段解析工作更少，但完整 QuickFIX client/server 流水线慢约 36.8%，同时 voluntary context switches 增长约 21 倍。

## CPU affinity 对照

进程限制在 CPU `1,2,3` 后：

| 指标 | `validate=yes` | `validate=no` |
|---|---:|---:|
| benchmark seconds | 1.169815 | 1.331510 |
| system CPU seconds | 0.84 | 1.19 |
| voluntary context switches | 1,967 | 20,927 |

性能反转仍然存在，但差距缩小，说明调度会放大该现象，但随机迁核不是唯一原因。

## 隔离实验

| 场景 | `validate=yes` | `validate=no` | 结论 |
|---|---:|---:|---|
| parse，50 万条 | 0.602844s | 0.451201s | 单独解析时 `no` 快约 25%。 |
| raw server，20 万条 | 0.684231s | 0.426353s | 连续供数时 `no` 快约 38%。 |

纯解析和 raw server 都没有 voluntary context switches 暴增。反转只出现在 QuickFIX initiator 逐条构造、序列化和发送消息的完整流水线中。

## 当前结论

这些数据强力佐证以下机制：

```text
validate=no 让 server 更快处理完当前 recv buffer
  -> server 更早回到阻塞 poll()
  -> QuickFIX client 下一小批数据尚未到达
  -> server 线程睡眠并在数据到达后被唤醒
  -> 更碎的 recv 批次和更多上下文切换抵消了解析收益
```

`validate=yes` 则在这个并发流水线里意外形成了“自然批处理”：

```text
server 对当前 recv buffer 做数据字典验证
  -> 同一时间，QuickFIX client 线程继续构造、序列化并发送后续消息
  -> 新到达的后续消息在内核 socket receive queue 中逐渐积累
  -> server 处理完当前批次后再次调用 poll()
  -> socket 往往已经可读，poll() 立即返回而不需要睡眠
  -> 下一次 recv 更容易取得较多字节，一轮可以解析更多完整 FIX 消息
  -> poll/recv 的固定开销被更多消息分摊，上下文切换显著减少
```

这里的“批处理”不是 QuickFIX 显式把若干消息组成了一个业务 batch，也不是 `validate=yes` 修改了 TCP 协议。它只是因为 server 在当前批次上停留得稍久，而 client 是另一个并发线程，仍在持续向 socket 供数，所以接收队列自然形成 backlog。

两种模式的差异可以概括为：

| 模式 | server 回到下一次 `poll()` 时的常见状态 | 结果 |
|---|---|---|
| `validate=no` | 当前数据很快清空，下一小批可能还没到 | `poll()` 更容易阻塞，批次更碎，睡眠/唤醒更多 |
| `validate=yes` | client 已趁验证期间发送了更多数据 | `poll()` 更容易立即返回，每次 `recv` 后可处理更多消息 |

因此，`validate=yes` 更快不代表数据字典验证本身能加速解析。纯 parse 和 raw server 的隔离结果已经证明单条消息上仍然是 `validate=no` 更快。这里只是 `validate=yes` 增加的计算成本小于它偶然节省的阻塞唤醒和碎片化收包成本；如果验证成本继续增大并超过 client 的供数速度，它最终仍会成为瓶颈。

这还不是对 `poll()` 次数的直接测量，因为进程级 voluntary context switches 也可能包含 mutex 等等待。需要按照目录 README 中的插桩方案继续采集 `poll_wait_nanoseconds`、`recv_calls`、`average_bytes_per_recv` 和 `messages_per_recv`。
