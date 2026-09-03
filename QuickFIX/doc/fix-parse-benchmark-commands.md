# FIX Parse Benchmark 操作指引

本文档用于对比三类常用 FIX 消息的解析性能：

- 下单：`new-order-single`
- 撤单：`order-cancel-request`
- 行情快照：`market-data-snapshot`

每类消息分三种构建组合测试：

- 仅开 SIMD stream parser
- 仅开 fixed-layout parser
- SIMD stream parser 和 fixed-layout parser 都开

## 基本原则

所有命令都从仓库根目录执行：

```bash
cd /path/to/vnpy-quickfix-acrl/QuickFIX
```

`QUICKFIX_SIMD_STREAM_PARSER`、`QUICKFIX_SIMD_FIELD_SCAN`、`QUICKFIX_SIMD_PATTERN_SCAN`、
`QUICKFIX_FIXED_LAYOUT_PARSER`、`QUICKFIX_BUSY_POLL`、`QUICKFIX_DIRECT_READ_POLL` 都是编译期选项。
切换这些选项时必须重新 configure 或重新 build 对应 build 目录。

`--message`、`--messages`、`--port`、`--fixed-layout`、`--validate` 是运行期参数。只改这些参数不需要重新 build。

当前 benchmark target 会把最新构建出的程序链接到：

```bash
test/fix_parse_benchmark
```

所以 `./test/run_parse_benchmark.sh ...` 总是运行最后一次 build 出来的版本。切换测试组合时，先 build 对应 profile，再运行该 profile 下的命令。

本文档里的 SIMD 默认指 `QUICKFIX_SIMD_STREAM_PARSER=ON`，也就是用于 server/raw 流式收包后切完整 FIX 消息的 SIMD 优化。`QUICKFIX_SIMD_PATTERN_SCAN=ON` 表示额外用 SIMD 直接匹配 checksum 字段前的 `SOH10=` 四字节模式。`QUICKFIX_SIMD_FIELD_SCAN` 先保持 `OFF`，因为前面的实验结果显示它可能拖慢字段扫描。

fixed-layout parser 的快速路径触发条件是两部分：整条消息长度必须等于对应模板长度，并且固定 offset 上必须匹配自定义字段 `9001=NOS1`、`9001=CXL1` 或 `9001=MDW1`。如果任一条件不满足，就回退普通字段解析。

## `--client=raw` 是什么

`--client` 只影响 `--mode=server` 或 `--mode=both`。纯 `--mode=parse` 不走网络，也没有 client 角色。

`--client=raw` 表示 benchmark 自己打开一个 socket，直接把已经拼好的 FIX 字节串写给 QuickFIX acceptor。这个路径最适合当前实验，因为它尽量减少 client 侧 QuickFIX 封装、对象构造、字段重排、发送节奏等变量，主要观察 server 端收包、切包、解析和 session 接收应用消息的成本。

`--client=quickfix` 表示 client 端也启动一个 QuickFIX `SocketInitiator`，通过 `Session::sendToTarget()` 发送消息。这个路径更接近正常 QuickFIX 双端通信，但会额外包含 initiator 侧的 Message 构造、序列化、session 处理和发送成本，因此不适合精确比较 server 端 parser 优化。

本文档主测试都使用 `--client=raw`。原因是：

- SIMD stream parser 优化发生在 server 端从 socket buffer 切完整 FIX 消息的路径，raw client 更容易隔离这个成本。
- fixed-layout 依赖固定字段顺序和固定长度，raw client 能保证发送的 wire bytes 就是 benchmark 生成的模板。
- `--fixed-layout` 在 server 模式下目前只支持 `--client=raw`。如果使用 `--client=quickfix`，benchmark 会直接报错。

普通消息可以用 `--client=quickfix` 做参考测试，但不要把它和 raw client 的结果直接当作同一层面的 parser 对比。

## 运行参数适用范围

| 参数 | 适用范围 | 是否需要重新 build | 说明 |
|------|----------|--------------------|------|
| `--mode=parse` | 所有 profile | 否 | 只测 `Message::setString()`，不走 socket，不经过 SIMD stream parser。 |
| `--mode=server` | 所有 profile | 否 | 启动 QuickFIX acceptor，通过 client 发消息，能覆盖 socket 收包、stream parser、Message 解析和 session 接收。 |
| `--mode=both` | 所有 profile | 否 | 先跑 parse，再跑 server。结果会输出两段。 |
| `--client=raw` | server/both | 否 | 直接 socket 发送预生成 FIX 字节串。当前主测试推荐使用。 |
| `--client=quickfix` | server/both | 否 | 用 QuickFIX initiator 发送普通消息。不能和 `--fixed-layout` 一起用于 server/both。 |
| `--message=...` | 所有 profile | 否 | 选择 `new-order-single`、`order-cancel-request`、`market-data-snapshot` 或 `quote-request`。 |
| `--messages=N` | 所有 profile | 否 | 压测消息数。建议不同 profile 使用同一个 N。 |
| `--warmup=N` | parse | 否 | parse-mode 正式计时前的预热次数。server-mode 不使用这个参数。 |
| `--fixed-layout` | fixed-layout 已编译开启的 profile | 否 | 发送带 `9001` 的固定模板消息，并允许 server 端进入 offset 快速解析。 |
| `--busy-poll` | busy-poll 已编译开启的 server/both profile | 否 | 让 acceptor 网络线程使用 `poll(..., 0)` 零等待轮询。 |
| `--busy-poll-cpu=N` | busy-poll 已编译开启的 server/both profile | 否 | 启用 busy-poll，并把 acceptor 网络线程绑定到 CPU N。 |
| `--direct-read-poll` | direct-read 已编译开启的 server/both profile | 否 | 多 Session 实验模式：绕过每轮 `poll()`，直接尝试 non-blocking `accept/recv/send`。不能与 `--busy-poll` 同时使用。 |
| `--validate=yes` | 所有 profile | 否 | 加载 FIX42 数据字典并启用验证。普通行情快照 server 测试需要这个参数来识别 repeating group。 |
| `--port=N` | server/both | 否 | 指定 acceptor 监听端口。并行跑多组测试时要用不同端口。 |
| `--send-buffer-size=N` | server/both | 否 | 设置 socket 发送缓冲区。一般先不要改，只有排查网络缓冲影响时再用。 |
| `--receive-buffer-size=N` | server/both | 否 | 设置 socket 接收缓冲区。一般先不要改，只有排查网络缓冲影响时再用。 |
| `--server-wait-seconds=N` | server/both | 否 | 等待 server 收满消息的超时时间。消息量很大或机器慢时可以调大。 |

编译期选项不属于运行参数，改它们必须重新 build：`QUICKFIX_SIMD_STREAM_PARSER`、
`QUICKFIX_SIMD_FIELD_SCAN`、`QUICKFIX_SIMD_PATTERN_SCAN`、`QUICKFIX_FIXED_LAYOUT_PARSER`、
`QUICKFIX_BUSY_POLL`、`QUICKFIX_DIRECT_READ_POLL`。

## 建议先跑正确性测试

每次切换 build profile 后，建议先跑对应 self-test，再跑性能数字。

所有 profile 都可以跑：

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
./test/run_parse_benchmark.sh --self-test-parser
./test/run_parse_benchmark.sh --self-test-correctness --messages=1000
```

fixed-layout 开启的 profile 额外跑：

```bash
./test/run_parse_benchmark.sh --self-test-correctness --fixed-layout --messages=1000
```

## Profile A：仅开 SIMD

这个 profile 用来测试普通 FIX 消息在 server/raw 路径里，只有 SIMD stream parser 生效时的性能。

### 编译

这里继续使用原来的 `build-direct-stage3` 目录以节省磁盘空间。目录名不决定代码阶段；重新执行下面的增量
build 后，该目录里的二进制已经包含阶段四实现。

```bash
cmake -S . -B build-bench-simd-only \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=ON \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=ON \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=OFF \
  -DQUICKFIX_BUSY_POLL=OFF

cmake --build build-bench-simd-only --target fix_parse_benchmark_target -j 2
```

### 正确性测试

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
./test/run_parse_benchmark.sh --self-test-parser
./test/run_parse_benchmark.sh --self-test-correctness --messages=1000
```

### 性能测试

下单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54321
```

撤单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --messages=100000 --port=54322
```

行情快照：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --messages=100000 --port=54323 --validate=yes
```

普通行情快照有 repeating group。server 模式下不开 fixed-layout 时需要 `--validate=yes` 加载数据字典，否则 session 层不能正确识别 `268` 下面重复的 `269/270/271/273` 组。

## Profile B：仅开 fixed-layout

这个 profile 用来测试带自定义 tag `9001` 的固定模板消息。SIMD stream parser 关闭，固定模板解析开启。

### 编译

```bash
cmake -S . -B build-bench-fixed-only \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=OFF \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=OFF \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=ON \
  -DQUICKFIX_BUSY_POLL=OFF

cmake --build build-bench-fixed-only --target fix_parse_benchmark_target -j 2
```

### 正确性测试

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
./test/run_parse_benchmark.sh --self-test-parser
./test/run_parse_benchmark.sh --self-test-correctness --messages=1000
./test/run_parse_benchmark.sh --self-test-correctness --fixed-layout --messages=1000
```

### 性能测试

下单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --fixed-layout --messages=100000 --port=54331
```

撤单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --fixed-layout --messages=100000 --port=54332
```

行情快照：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --fixed-layout --messages=100000 --port=54333
```

这组命令必须带 `--fixed-layout`，否则发送的是普通消息，不会进入固定模板测试路径。

## Profile C：SIMD 和 fixed-layout 都开

这个 profile 用来测试完整目标路径：server/raw 先用 SIMD stream parser 切完整 FIX 消息，再对带 `9001` 的固定模板消息走 fixed-layout offset 解析。

### 编译

```bash
cmake -S . -B build-bench-simd-fixed \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=ON \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=ON \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=ON \
  -DQUICKFIX_BUSY_POLL=OFF

cmake --build build-bench-simd-fixed --target fix_parse_benchmark_target -j 2
```

### 正确性测试

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
./test/run_parse_benchmark.sh --self-test-parser
./test/run_parse_benchmark.sh --self-test-correctness --messages=1000
./test/run_parse_benchmark.sh --self-test-correctness --fixed-layout --messages=1000
```

### 性能测试

下单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --fixed-layout --messages=100000 --port=54341
```

撤单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --fixed-layout --messages=100000 --port=54342
```

行情快照：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --fixed-layout --messages=100000 --port=54343
```

## Profile D：SIMD、fixed-layout 和 busy-poll 都开

这个 profile 用来测试当前最完整的实验路径：server/raw 使用 busy-poll 网络等待，stream parser 使用 SIMD 切完整 FIX 消息，带 `9001` 的固定模板消息走 fixed-layout offset 解析。

### 编译

```bash
cmake -S . -B build-bench-busy-poll \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=ON \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=ON \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=ON \
  -DQUICKFIX_BUSY_POLL=ON

cmake --build build-bench-busy-poll --target fix_parse_benchmark_target -j 2
```

### 正确性测试

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
./test/run_parse_benchmark.sh --self-test-parser
./test/run_parse_benchmark.sh --self-test-correctness --messages=1000
./test/run_parse_benchmark.sh --self-test-correctness --fixed-layout --messages=1000
```

### busy-poll 性能测试

下单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54361 --busy-poll
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54362 --busy-poll --busy-poll-cpu=0
```

撤单：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --messages=100000 --port=54363 --busy-poll
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --messages=100000 --port=54364 --busy-poll --busy-poll-cpu=0
```

行情快照普通解析：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --messages=100000 --port=54365 --validate=yes --busy-poll
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --messages=100000 --port=54366 --validate=yes --busy-poll --busy-poll-cpu=0
```

固定模板解析可以继续叠加 `--fixed-layout`：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --fixed-layout --messages=100000 --port=54367 --busy-poll --busy-poll-cpu=0
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --fixed-layout --messages=100000 --port=54368 --busy-poll --busy-poll-cpu=0
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --fixed-layout --messages=100000 --port=54369 --busy-poll --busy-poll-cpu=0
```

如果要在同一个 busy-poll build 里跑运行时关闭 busy-poll 的 baseline，去掉 `--busy-poll` 即可。

### 观察绑核是否生效

busy-poll + 绑核开启后，指定 CPU 的占用率应该明显升高。建议开两个 WSL 终端观察。

终端 A 观察每个 CPU：

```bash
top
```

进入 `top` 后按：

```text
1
```

这样会展开每个 CPU 的占用，例如：

```text
%Cpu0  :  0.3 us,  1.5 sy, 89.2 id
%Cpu1  :  1.6 us,  1.3 sy, 95.7 id
%Cpu2  : 80.1 us, 19.6 sy,  0.0 id
%Cpu3  : 31.6 us,  2.3 sy, 63.5 id
```

如果命令使用：

```bash
--busy-poll --busy-poll-cpu=2
```

并且 `Cpu2` 的 `id` 接近 `0.0`，同时 `us + sy` 接近 `100%`，说明 busy-poll 线程大概率已经绑到 CPU 2 并持续运行。

也可以用 `mpstat` 观察：

```bash
sudo apt install sysstat
mpstat -P ALL 1
```

终端 B 跑一个持续时间更长的 benchmark，否则 10 万条消息可能结束太快，肉眼看不到 CPU 占用变化：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=5000000 --port=54380 --busy-poll --busy-poll-cpu=2
```

如果把 `--busy-poll-cpu=2` 改成 `--busy-poll-cpu=1`，高占用 CPU 也应该从 `Cpu2` 转移到 `Cpu1`。这是确认绑核生效的最直接对照。

## Profile E：blocking、poll0 和 direct-read 对照

这个 profile 用同一个二进制比较三种 acceptor 网络循环。SIMD、fixed-layout 和诊断计数全部关闭，避免把解析
优化或诊断开销混入网络实验。

direct-read 当前已完成阶段四，支持普通 UNIX `SocketAcceptor` 的多个已配置 FIX Session。每轮对每个连接
最多尝试一次 non-blocking `recv()` 和一次发送队列推进；发送遇到 partial write 或 `EAGAIN` 会保留偏移并在
下一轮继续。Heartbeat、TestRequest、ResendRequest、Logout、握手超时和正常停止已有专项测试。read-drain、
scan budget 和空闲退避尚未实现。

### 编译

```bash
cmake -S . -B build-direct-stage3 \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_LIB_OUTPUT_DIR="$PWD/build-direct-stage3/out" \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_TESTS=ON \
  -DQUICKFIX_SHARED_LIBS=OFF \
  -DQUICKFIX_SIMD_STREAM_PARSER=OFF \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=OFF \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=OFF \
  -DQUICKFIX_BUSY_POLL=ON \
  -DQUICKFIX_DIRECT_READ_POLL=ON \
  -DQUICKFIX_NETWORK_DIAGNOSTICS=OFF

cmake --build build-direct-stage3 --target ut fix_parse_benchmark_target --parallel 2
```

### 正确性测试

```bash
./build-direct-stage3/out/ut DirectSocketReadTests
./build-direct-stage3/out/ut DirectSocketAcceptorTests
./build-direct-stage3/out/ut DirectMultiSocketAcceptorTests
./build-direct-stage3/out/ut DirectSocketWriteTests
./build-direct-stage3/out/ut DirectSocketWriteBackpressureTests
./build-direct-stage3/out/ut DirectSocketSessionTimerTests
./build-direct-stage3/out/ut DirectSocketResendRequestTests
./build-direct-stage3/out/ut DirectSocketLogonTimeoutTests
./build-direct-stage3/out/ut DirectSocketGracefulStopTests
./build-direct-stage3/out/ut "Direct*"
./build-direct-stage3/out/ut --quickfix-spec-path spec
```

`DirectMultiSocketAcceptorTests` 是多连接正确性入口。阶段四新增的测试覆盖真实发送缓冲区填满、部分发送、
`EAGAIN` 续发、Session 定时消息、ResendRequest、Logon/Logout 超时和 `stop(false)`。当前性能 benchmark 仍是
单 client。

这些测试会建立本机 socket；如果执行环境限制网络 syscall，应在普通 WSL 终端运行，而不是把 `EPERM` 当成
QuickFIX 逻辑失败。`--quickfix-spec-path spec` 是完整单元测试必需的字典路径参数。

### 同一二进制切换三种模式

blocking baseline：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54601
```

原 `poll(..., 0)` busy-poll：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54602 --busy-poll
```

direct-read：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54603 --direct-read-poll
```

direct-read 并把 acceptor 线程绑定到 CPU 2：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54604 --direct-read-poll --busy-poll-cpu=2
```

输出中的 `socket_poll_mode` 应分别为 `blocking`、`poll0`、`direct`；每组的 `received` 都应等于
`messages`。`--busy-poll` 和 `--direct-read-poll` 是互斥参数。

### direct-read 三种消息

raw client：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54611 --direct-read-poll
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --messages=100000 --port=54612 --direct-read-poll
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --messages=100000 --port=54613 --validate=yes --direct-read-poll
```

QuickFIX initiator client：

```bash
./test/run_parse_benchmark.sh --mode=server --client=quickfix --message=new-order-single --messages=100000 --port=54621 --direct-read-poll
./test/run_parse_benchmark.sh --mode=server --client=quickfix --message=order-cancel-request --messages=100000 --port=54622 --direct-read-poll
./test/run_parse_benchmark.sh --mode=server --client=quickfix --message=market-data-snapshot --messages=100000 --port=54623 --validate=yes --direct-read-poll
```

普通行情快照包含 repeating group，因此这里保留 `--validate=yes`。上述命令切换 client、消息、网络模式和
绑核都不需要重新 build。

## 什么时候需要重新 build

需要重新 build：

- 在 Profile A、B、C、D、E 或 baseline 之间切换。
- 修改了 `Parser.cpp`、`Message.cpp`、`FastScan.cpp`、`fix_parse_benchmark.cpp` 等 C++ 源码。
- 修改了 CMake 选项，例如把 `QUICKFIX_SIMD_FIELD_SCAN`、`QUICKFIX_SIMD_PATTERN_SCAN`、
  `QUICKFIX_BUSY_POLL` 或 `QUICKFIX_DIRECT_READ_POLL` 从 `OFF` 改成 `ON`。

不需要重新 build：

- 只改 `--message`。
- 只改 `--messages`。
- 只改 `--port`。
- 只改 `--mode`。
- 只是在 fixed-layout 已编译开启的 profile 里，加或去掉运行期 `--fixed-layout`。
- 只是在 busy-poll 已编译开启的 profile 里，加或去掉运行期 `--busy-poll` 或 `--busy-poll-cpu`。
- 只是在 direct-read 已编译开启的 profile 里，加或去掉运行期 `--direct-read-poll`，或改变绑核 CPU。

## 补充：只测 Message::setString 解析层

上面的主命令使用 `--mode=server`，因为只有 server/raw 路径会经过 stream parser，也才能观察 SIMD stream parser 的收益。

如果只想看 `Message::setString` 层面的 fixed-layout 解析速度，可以在 Profile B 或 Profile C 下运行：

```bash
./test/run_parse_benchmark.sh --mode=parse --message=new-order-single --fixed-layout --messages=100000
./test/run_parse_benchmark.sh --mode=parse --message=order-cancel-request --fixed-layout --messages=100000
./test/run_parse_benchmark.sh --mode=parse --message=market-data-snapshot --fixed-layout --messages=100000
```

这三条 parse-mode 命令不会经过 SIMD stream parser，因此不能用来评估 `QUICKFIX_SIMD_STREAM_PARSER`。

## 补充：基线 profile

如果要和完全未开启优化的普通路径比较，可以单独构建一个 baseline：

```bash
cmake -S . -B build-bench-baseline \
  -DCMAKE_BUILD_TYPE=Release \
  -DQUICKFIX_EXAMPLES=ON \
  -DQUICKFIX_SIMD_STREAM_PARSER=OFF \
  -DQUICKFIX_SIMD_FIELD_SCAN=OFF \
  -DQUICKFIX_SIMD_PATTERN_SCAN=OFF \
  -DQUICKFIX_FIXED_LAYOUT_PARSER=OFF \
  -DQUICKFIX_BUSY_POLL=OFF \
  -DQUICKFIX_DIRECT_READ_POLL=OFF

cmake --build build-bench-baseline --target fix_parse_benchmark_target -j 2
```

然后跑普通消息：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=100000 --port=54351
./test/run_parse_benchmark.sh --mode=server --client=raw --message=order-cancel-request --messages=100000 --port=54352
./test/run_parse_benchmark.sh --mode=server --client=raw --message=market-data-snapshot --messages=100000 --port=54353 --validate=yes
```
