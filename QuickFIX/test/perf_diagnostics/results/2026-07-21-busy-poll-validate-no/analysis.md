# Busy-poll 对 validate=no 的影响

使用同一个 `QUICKFIX_BUSY_POLL=ON` 二进制，20 万条 `new-order-single`，QuickFIX client，`validate=no`，不绑核。唯一的运行期变量是是否传入 `--busy-poll`。

| 指标 | blocking poll 平均 | busy-poll 平均 | 变化 |
|---|---:|---:|---:|
| benchmark seconds | 1.472414 | 1.143888 | 降低 22.3% |
| messages/second | 136,046 | 175,173 | 提升 28.8% |
| voluntary context switches | 32,057 | 196 | 降低 99.4% |
| system CPU seconds | 1.170 | 1.917 | 增加 63.8% |
| process CPU percent | 91.3% | 145.3% | 增加约 54 个百分点 |

busy-poll 几乎消除了 voluntary context switches，并恢复了吞吐，但付出了持续占用 CPU 和更多 `poll(..., 0)` 系统调用的代价。

在 blocking-poll 实验中，`validate=yes` 也能减少上下文切换，但机制与 busy-poll 不同。它在 server 验证当前消息期间给了 QuickFIX client 继续发送的时间，使 socket 接收队列自然积累出更大的批次；server 再次进入 `poll()` 时，数据往往已经到达。busy-poll 则没有依赖这种偶然的批处理，它直接把等待时间设为 0，即使下一批数据尚未到达也不让 acceptor 线程睡眠。

所以两者都可能减少睡眠/唤醒，但代价不同：`validate=yes` 消耗 CPU 做协议验证，busy-poll 消耗 CPU 持续轮询。前者不是应当保留的性能优化，只是解释了为什么原实验会出现看似反常的结果。

这组结果与之前的隔离实验共同构成强因果证据：

```text
blocking poll + validate=no
  -> 约 3.2 万次 voluntary context switches
  -> 1.47 秒

仅把 acceptor 改为 poll(..., 0)
  -> 约 196 次 voluntary context switches
  -> 1.14 秒
```

它仍不是 `poll_calls` 和 `poll_wait_nanoseconds` 的直接计数。busy-poll 同时改变了 CPU 占用、调度方式和 socket 数据积累节奏，因此最严格的机制证明仍需要 README 中描述的 acceptor 侧插桩。
