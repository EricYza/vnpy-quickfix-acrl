# QuickFIX poll 阻塞诊断

这个目录保存 `--client=quickfix` 下 `--validate=yes/no` 性能反转的可重复实验脚本、原始数据和 acceptor 网络插桩。

## 1. 现有脚本能测什么

`run_validate_context_switch_test.sh` 使用 GNU `time` 记录整个 benchmark 进程的：

- benchmark 计时 `seconds`
- user CPU time
- system CPU time
- voluntary context switches
- involuntary context switches
- CPU 使用率
- 最大常驻内存

默认运行 20 万条 `new-order-single`，`validate=yes/no` 各重复三次。奇数轮先跑 `yes`，偶数轮先跑 `no`，避免固定运行顺序带来的温度、缓存或系统负载偏差。

从仓库根目录运行：

```bash
BUILD_DIR=build-bench-baseline \
  ./test/perf_diagnostics/run_validate_context_switch_test.sh
```

固定进程只能运行在 CPU 1、2、3：

```bash
BUILD_DIR=build-bench-baseline \
CPUSET=1,2,3 \
  ./test/perf_diagnostics/run_validate_context_switch_test.sh
```

调整消息数、重复次数和起始端口：

```bash
BUILD_DIR=build-bench-baseline \
MESSAGES=1000000 \
REPETITIONS=5 \
START_PORT=55200 \
  ./test/perf_diagnostics/run_validate_context_switch_test.sh
```

每次运行会新建：

```text
test/perf_diagnostics/results/YYYYMMDD-HHMMSS/
  metadata.txt
  summary.tsv
  validate-yes-run-1.command.txt
  validate-yes-run-1.benchmark.txt
  validate-yes-run-1.resources.txt
  ...
```

`metadata.txt` 会保存二进制哈希、系统信息以及指定 build 目录的 CMake 开关。由于当前多个 CMake build 目录共用仓库的 `lib/` 输出位置，哈希是确认实际二进制身份的重要依据。

## 2. context switch 数据能证明什么

`voluntary context switches` 表示线程主动进入等待并让出 CPU。阻塞 `poll()`、等待 mutex、条件变量等都可能增加这个值。

所以它可以强力佐证“线程频繁睡眠和唤醒”，但它不能单独证明每次切换都来自 `poll()`。需要结合以下隔离结果：

- 纯解析时 `validate=no` 更快，并且没有 context switch 暴增。
- raw client 时 `validate=no` 更快，并且没有 context switch 暴增。
- 只有 QuickFIX client 逐条发送配合阻塞 `poll()` 时发生反转。
- 在真正编译了 busy-poll 的版本中，改成 `poll(..., 0)` 后性能恢复。

单独复现 busy-poll 对照：

```bash
BUILD_DIR=build-bench-busy-poll \
  ./test/perf_diagnostics/run_busy_poll_context_switch_test.sh
```

这个脚本固定使用 `validate=no`，在同一个 busy-enabled 二进制中交替运行 `--no-busy-poll` 和 `--busy-poll`。可以通过 `VALIDATE=yes` 改成字典验证场景，但不要把不同 build 生成的结果作为唯一对照。

本次已经保存的 busy-poll 结果位于：

```text
test/perf_diagnostics/results/2026-07-21-busy-poll-validate-no/
```

## 3. 直接测 poll 和 recv

项目现在已经实现了下面描述的 acceptor 侧诊断。先构建同时启用 busy-poll 能力和网络诊断的 profile：

```bash
cmake -S . -B build-bench-busy-poll \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=ON \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=OFF \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=ON \
  -DQUICKFIX_BUSY_POLL=ON \
  -DQUICKFIX_NETWORK_DIAGNOSTICS=ON

cmake --build build-bench-busy-poll --target fix_parse_benchmark_target -j 2
```

运行完整三组直接证据测试：

```bash
BUILD_DIR=build-bench-busy-poll \
  ./test/perf_diagnostics/run_acceptor_network_diagnostics.sh
```

本次三组各三次的直接证据保存在：

```text
test/perf_diagnostics/results/2026-07-21-acceptor-direct-evidence/
```

单条命令需要增加运行期开关：

```bash
./test/run_parse_benchmark.sh \
  --mode=server \
  --client=quickfix \
  --message=new-order-single \
  --messages=100000 \
  --port=55420 \
  --validate=no \
  --no-busy-poll \
  --network-diagnostics
```

`QUICKFIX_NETWORK_DIAGNOSTICS` 默认是 `OFF`。诊断开启后，每次 `poll()` 前后需要读取线程 context-switch 计数并读取时钟，因此诊断结果适合证明机制，不应作为最终无插桩吞吐率。

实现使用下面的编译期开关：

```text
QUICKFIX_NETWORK_DIAGNOSTICS=ON/OFF
```

默认值为 `OFF`，避免原子计数、读时钟和 context-switch 采样影响正常 benchmark。

### poll 指标

插桩位置是 `src/C++/SocketMonitor_UNIX.cpp` 中调用 `poll()` 的前后：

```cpp
const auto pollStart = Clock::now();
result = poll(pfds, pfds_size, timeout);
const auto pollElapsed = Clock::now() - pollStart;
```

当前记录：

| 指标 | 定义 |
|---|---|
| `poll_calls` | 调用 `poll()` 的总次数。 |
| `poll_wait_nanoseconds` | 所有 `poll()` 调用耗费的墙钟时间总和。 |
| `poll_immediate_returns` | `poll()` 前后 acceptor 网络线程的 `ru_nvcsw` 未增加的次数。 |
| `poll_blocking_returns` | `poll()` 前后 acceptor 网络线程的 `ru_nvcsw` 增加的次数，证明调用期间线程主动让出过 CPU。 |
| `poll_context_sample_failures` | `getrusage(RUSAGE_THREAD)` 采样失败、无法分类的次数。 |

`poll()` 的返回值不会告诉调用者“本次是否真的睡眠”。当前实现没有使用耗时阈值，而是在每次调用前后读取：

```cpp
getrusage(RUSAGE_THREAD, &usage);
```

比较 `ru_nvcsw` 是否增长。这个方法能直接证明线程是否主动让出 CPU，但每次 `poll()` 前后多两次采样，会明显扰动性能，只适合证明阻塞来源，不适合报告最终吞吐率。

### recv 指标

插桩位置是 `src/C++/SocketConnection.cpp` 的 `SocketConnection::readFromSocket()`：

```cpp
ssize_t size = socket_recv(m_socket, m_buffer, sizeof(m_buffer));
```

当前记录：

| 指标 | 定义 |
|---|---|
| `recv_calls` | 成功返回正数字节的 `recv()` 次数。 |
| `recv_bytes` | 所有成功 `recv()` 返回字节数之和。 |
| `average_bytes_per_recv` | `recv_bytes / recv_calls`。 |

### messages_per_recv

`SocketConnection::readMessages()` 会把 Parser buffer 中所有完整消息送入 `Session::next()`。每当 Parser 成功切出一条完整 FIX 消息，acceptor 就将 `parsed_messages` 加一：

```text
每成功切出一条完整消息：parsed_messages += 1
messages_per_recv = parsed_messages / recv_calls
```

一条跨越两个 recv buffer 的消息会计入完成它的后一轮 recv；对总量平均值没有影响。

### 计数边界

必须在 QuickFIX initiator 和 acceptor 完成 Logon 后清零计数，并在 server 收满目标 application messages 后立刻快照。否则 Logon、Logout、Heartbeat 和停止线程时的网络事件会混入结果。

计数应保存在每个 `SocketMonitor`/`SocketConnection` 实例中，benchmark 只读取 acceptor 侧统计。不能使用一个进程级全局计数器，否则 QuickFIX initiator 的 `poll/send/recv` 会和 server 数据混在一起。

## 4. 如何判断假设成立

重点比较同一消息数下的两组结果：

```text
client=quickfix, validate=yes, busy-poll=off
client=quickfix, validate=no,  busy-poll=off
```

如果性能反转确实来自服务端过早回到阻塞 `poll()`，预期看到：

| 指标 | `validate=no` 的预期 |
|---|---|
| `poll_calls` | 更多，或者相同调用数中等待占比更高。 |
| `poll_wait_nanoseconds` | 明显更高。 |
| `poll_blocking_returns` | 明显更多。 |
| `recv_calls` | 更多。 |
| `average_bytes_per_recv` | 更小。 |
| `messages_per_recv` | 更小。 |

然后在同一个 `validate=no` 构建上开启真正生效的 busy-poll。预期 `poll_wait_nanoseconds` 和阻塞切换显著下降，同时吞吐恢复。这能形成从现象、隔离到机制的完整证据链。

## 5. 外部工具的辅助方案

安装了 `strace` 时，可以辅助统计系统调用次数和耗时：

```bash
strace -ff -tt -T \
  -e trace=poll,ppoll,recvfrom,recv,sendto,send \
  -o /tmp/quickfix-poll-trace \
  ./test/run_parse_benchmark.sh \
    --mode=server \
    --client=quickfix \
    --message=new-order-single \
    --messages=100000 \
    --port=55250 \
    --validate=no
```

`strace` 会显著降低吞吐，适合看调用次数、返回字节和阻塞时长，不应把它下面的 `seconds` 当成最终性能数据。本机当前没有安装 `strace`，因此本次保存的数据来自 GNU `time`。
