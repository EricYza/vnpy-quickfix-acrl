# QuickFIX Benchmark 与网络模式严格调用链

本文从 benchmark 表层入口开始，沿着代码中真实存在的函数调用，一直走到 socket、Parser、Session 和
`BenchmarkApplication::fromApp()`。阅读时按照 `调用位置 -> 被调用函数定义位置` 依次点击即可。

## 1. 链接与箭头规则

- 本文使用相对路径加 `#L行号`，例如
  [`SocketAcceptor::onStart()`](../src/C++/SocketAcceptor.cpp#L290)。在 VS Code 的 Markdown 预览中点击
  或按住 `Ctrl` 点击，可以打开对应文件并跳到该行。
- 行号是静态的。如果以后在目标代码上方增删了内容，链接仍会打开正确文件，但行号需要重新校准。
- 除了明确标注的“进程入口”“线程入口注册”“动态分派”和“跨线程等待”外，每个 `->` 左边都是实际调用
  语句，右边都是被调用函数的定义。
- `waitForMessages()` 不会调用 `fromApp()`。前者在 benchmark 主线程等待，后者由 acceptor 网络线程触发；
  本文会把这种关系标为“跨线程汇合”，不画成虚假的直接函数调用。

## 2. Benchmark 框架

### 2.1 构建产物和脚本入口

这三处不是运行时函数调用，而是 benchmark 可执行文件的构建关系：

- [`src/CMakeLists.txt` 第 49 行](../src/CMakeLists.txt#L49) 创建 `fix_parse_benchmark` 可执行目标。
- [`src/CMakeLists.txt` 第 54 行](../src/CMakeLists.txt#L54) 把该目标链接到当前 QuickFIX 库。
- [`src/CMakeLists.txt` 第 57 行](../src/CMakeLists.txt#L57) 到
  [`第 61 行`](../src/CMakeLists.txt#L61) 在 UNIX 下把构建产物链接为 `test/fix_parse_benchmark`。

运行入口是：

[`run_parse_benchmark.sh` 第 13 行](../test/run_parse_benchmark.sh#L13) 执行 benchmark 二进制并透传 `"$@"`<br>
-> 进程入口
[`fix_parse_benchmark main()` 第 2329 行](../src/fix_parse_benchmark.cpp#L2329)

脚本本身不编译代码。它只选择已有的 benchmark 二进制、补上 FIX 4.2 数据字典路径，然后启动进程。

### 2.2 公共参数解析与模式分派

[`main()` 第 2335 行](../src/fix_parse_benchmark.cpp#L2335) 调用 `parseOptions(argc, argv)`<br>
-> [`parseOptions()` 定义第 2181 行](../src/fix_parse_benchmark.cpp#L2181)

几个决定主链路的参数位于：

| 参数 | 解析位置 | 决定的内容 |
|---|---|---|
| `--mode` | [`第 2198 行`](../src/fix_parse_benchmark.cpp#L2198) | `parse`、`server` 或 `both` |
| `--client` | [`第 2209 行`](../src/fix_parse_benchmark.cpp#L2209) | server 模式使用 raw socket 还是 QuickFIX initiator |
| `--message` | [`第 2218 行`](../src/fix_parse_benchmark.cpp#L2218) | 下单、撤单、行情或 QuoteRequest |
| `--validate` | [`第 2235 行`](../src/fix_parse_benchmark.cpp#L2235) | 是否启用字典和消息校验 |
| `--fixed-layout` | [`第 2239 行`](../src/fix_parse_benchmark.cpp#L2239) | 是否构造带 tag 9001 的固定模板消息 |
| `--busy-poll` | [`第 2243 行`](../src/fix_parse_benchmark.cpp#L2243) | 是否选择 `poll0` |
| `--direct-read-poll` | [`第 2247 行`](../src/fix_parse_benchmark.cpp#L2247) | 是否选择 direct scan |
| `--busy-poll-cpu` | [`第 2251 行`](../src/fix_parse_benchmark.cpp#L2251) | acceptor 网络线程绑定的逻辑 CPU |

参数解析完成后有两条顶层调用：

[`main()` 第 2353 行](../src/fix_parse_benchmark.cpp#L2353) 调用 `runParseBenchmark(options)`<br>
-> [`runParseBenchmark()` 定义第 1871 行](../src/fix_parse_benchmark.cpp#L1871)

[`main()` 第 2359 行](../src/fix_parse_benchmark.cpp#L2359) 调用 `runServerBenchmark(options)`<br>
-> [`runServerBenchmark()` 定义第 2052 行](../src/fix_parse_benchmark.cpp#L2052)

因此：

```text
--mode=parse
  -> 只测 Message 字段解析，不创建 socket

--mode=server
  -> 测 client 发送、acceptor 接收、完整消息切分、字段解析、Session 和 Application

--mode=both
  -> 先执行 parse，再执行 server
```

### 2.3 `mode=parse` 严格调用链

#### 2.3.1 构造一条样本消息

[`runParseBenchmark()` 第 1881 行](../src/fix_parse_benchmark.cpp#L1881) 调用
`makeApplicationMessage(2, options)`<br>
-> [`makeApplicationMessage()` 定义第 457 行](../src/fix_parse_benchmark.cpp#L457)

`makeApplicationMessage()` 根据消息类型和 `fixedLayout` 选择字段构造器：

| 消息 | 调用位置 | 被调用函数定义 |
|---|---|---|
| 普通下单 | [`第 463 行`](../src/fix_parse_benchmark.cpp#L463) | [`newOrderSingleFields()` 第 269 行](../src/fix_parse_benchmark.cpp#L269) |
| 固定下单 | [`第 462 行`](../src/fix_parse_benchmark.cpp#L462) | [`fixedNewOrderSingleFields()` 第 295 行](../src/fix_parse_benchmark.cpp#L295) |
| 普通撤单 | [`第 467 行`](../src/fix_parse_benchmark.cpp#L467) | [`orderCancelRequestFields()` 第 323 行](../src/fix_parse_benchmark.cpp#L323) |
| 固定撤单 | [`第 466 行`](../src/fix_parse_benchmark.cpp#L466) | [`fixedOrderCancelRequestFields()` 第 346 行](../src/fix_parse_benchmark.cpp#L346) |
| 普通行情 | [`第 471 行`](../src/fix_parse_benchmark.cpp#L471) | [`marketDataSnapshotFields()` 第 393 行](../src/fix_parse_benchmark.cpp#L393) |
| 固定行情 | [`第 470 行`](../src/fix_parse_benchmark.cpp#L470) | [`fixedMarketDataSnapshotFields()` 第 411 行](../src/fix_parse_benchmark.cpp#L411) |
| QuoteRequest | [`第 473 行`](../src/fix_parse_benchmark.cpp#L473) | [`quoteRequestFields()` 第 431 行](../src/fix_parse_benchmark.cpp#L431) |

字段构造器返回有序的 `(tag, value)` 列表后，外层调用 `buildFixMessage()`：

[`makeApplicationMessage()` 第 461、465、469 或 473 行](../src/fix_parse_benchmark.cpp#L461)<br>
-> [`buildFixMessage()` 定义第 202 行](../src/fix_parse_benchmark.cpp#L202)

[`buildFixMessage()` 第 205 行](../src/fix_parse_benchmark.cpp#L205) 调用 `field(tag, value)`<br>
-> [`field()` 定义第 190 行](../src/fix_parse_benchmark.cpp#L190)

`buildFixMessage()` 在 [`第 208 行`](../src/fix_parse_benchmark.cpp#L208) 添加 `8=` 和 `9=`，在
[`第 209 行`](../src/fix_parse_benchmark.cpp#L209) 到 [`第 216 行`](../src/fix_parse_benchmark.cpp#L216)
计算并添加 `10=CheckSum`，最终得到完整 FIX wire string。

#### 2.3.2 进入计时解析循环

[`runParseBenchmark()` 第 1892 行](../src/fix_parse_benchmark.cpp#L1892) 调用
`message.setString(sample, options.validate, dictionary)`<br>
-> [`Message::setString()` 定义第 564 行](../src/C++/Message.cpp#L564)

如果构建时打开 fixed-layout，并且消息长度与 marker 命中：

[`Message::setString()` 第 572 行](../src/C++/Message.cpp#L572) 调用 `setFixedLayoutString(string)`<br>
-> [`Message::setFixedLayoutString()` 定义第 482 行](../src/C++/Message.cpp#L482)

如果 fixed-layout 没有命中，继续普通逐字段解析：

[`Message::setString()` 第 585 行](../src/C++/Message.cpp#L585) 调用 `extractField(...)`<br>
-> [`Message::extractField()` 定义第 835 行](../src/C++/Message.cpp#L835)

如果 `doValidation=true`：

[`Message::setString()` 第 642 行](../src/C++/Message.cpp#L642) 调用 `validate()`<br>
-> [`Message::validate()` 定义第 803 行](../src/C++/Message.cpp#L803)

计时边界是 [`第 1890 行`](../src/fix_parse_benchmark.cpp#L1890) 到
[`第 1895 行`](../src/fix_parse_benchmark.cpp#L1895)。消息构造、字典加载和 warmup 均在计时之外。

[`runParseBenchmark()` 第 1897 行](../src/fix_parse_benchmark.cpp#L1897) 调用 `printResult(...)`<br>
-> [`printResult()` 定义第 763 行](../src/fix_parse_benchmark.cpp#L763)

### 2.4 `mode=server` 的 client 分派

[`main()` 第 2359 行](../src/fix_parse_benchmark.cpp#L2359)<br>
-> [`runServerBenchmark()` 定义第 2052 行](../src/fix_parse_benchmark.cpp#L2052)

[`runServerBenchmark()` 第 2054 行](../src/fix_parse_benchmark.cpp#L2054) 调用 raw 路径<br>
-> [`runRawServerBenchmark()` 定义第 1911 行](../src/fix_parse_benchmark.cpp#L1911)

[`runServerBenchmark()` 第 2056 行](../src/fix_parse_benchmark.cpp#L2056) 调用 QuickFIX 路径<br>
-> [`runQuickfixServerBenchmark()` 定义第 1976 行](../src/fix_parse_benchmark.cpp#L1976)

两条路径使用同一个 `SocketAcceptor` server。差别主要在 client 端如何构造、序列化和发送消息。

### 2.5 `client=raw` 严格调用链

#### 2.5.1 启动 server 和建立会话

[`runRawServerBenchmark()` 第 1916 行](../src/fix_parse_benchmark.cpp#L1916) 调用
`makeAcceptorConfig(options, options.port)`<br>
-> [`makeAcceptorConfig()` 定义第 553 行](../src/fix_parse_benchmark.cpp#L553)

[`runRawServerBenchmark()` 第 1919 行](../src/fix_parse_benchmark.cpp#L1919) 调用 `acceptor.start()`<br>
-> [`Acceptor::start()` 定义第 146 行](../src/C++/Acceptor.cpp#L146)

`acceptor.start()` 后面的 server 网络线程链路见本文第 3 节。

[`runRawServerBenchmark()` 第 1929 行](../src/fix_parse_benchmark.cpp#L1929) 调用
`connectWithRetry(...)`<br>
-> [`connectWithRetry()` 定义第 1838 行](../src/fix_parse_benchmark.cpp#L1838)

[`runRawServerBenchmark()` 第 1930 行](../src/fix_parse_benchmark.cpp#L1930) 调用
`makeLogonMessage()`<br>
-> [`makeLogonMessage()` 定义第 483 行](../src/fix_parse_benchmark.cpp#L483)

[`makeLogonMessage()` 第 488 行](../src/fix_parse_benchmark.cpp#L488) 调用 `buildFixMessage(fields)`<br>
-> [`buildFixMessage()` 定义第 202 行](../src/fix_parse_benchmark.cpp#L202)

[`runRawServerBenchmark()` 第 1930 行](../src/fix_parse_benchmark.cpp#L1930) 把 Logon 交给 `sendAll(...)`<br>
-> [`sendAll()` 定义第 1820 行](../src/fix_parse_benchmark.cpp#L1820)

[`sendAll()` 第 1823 行](../src/fix_parse_benchmark.cpp#L1823) 调用 `socket_send(...)`<br>
-> [`socket_send()` 定义第 273 行](../src/C++/Utility.cpp#L273)

[`runRawServerBenchmark()` 第 1932 行](../src/fix_parse_benchmark.cpp#L1932) 调用
`waitForLogon(...)`<br>
-> [`waitForLogon()` 定义第 1784 行](../src/fix_parse_benchmark.cpp#L1784)

#### 2.5.2 预构造、批量发送和结束计时

[`runRawServerBenchmark()` 第 1936 行](../src/fix_parse_benchmark.cpp#L1936) 调用
`makeOutboundStream(options)`<br>
-> [`makeOutboundStream()` 定义第 645 行](../src/fix_parse_benchmark.cpp#L645)

[`makeOutboundStream()` 第 654 行](../src/fix_parse_benchmark.cpp#L654) 对每条消息调用
`makeApplicationMessage(...)`<br>
-> [`makeApplicationMessage()` 定义第 457 行](../src/fix_parse_benchmark.cpp#L457)

因此 raw client 在计时开始之前，已经把所有应用消息拼成一个连续的 `outbound` 字节串。

[`runRawServerBenchmark()` 第 1941 行](../src/fix_parse_benchmark.cpp#L1941) 调用
`sendAll(socket.get(), outbound)`<br>
-> [`sendAll()` 定义第 1820 行](../src/fix_parse_benchmark.cpp#L1820)

[`runRawServerBenchmark()` 第 1942 行](../src/fix_parse_benchmark.cpp#L1942) 调用
`waitForMessages(...)`<br>
-> [`waitForMessages()` 定义第 1803 行](../src/fix_parse_benchmark.cpp#L1803)

这里是跨线程汇合，不是直接调用：

```text
acceptor 网络线程
  -> Parser
  -> Session
  -> BenchmarkApplication::fromApp()
  -> received.fetch_add(1)

benchmark 主线程
  -> waitForMessages()
  -> 反复读取 received
  -> received >= options.messages 时结束计时
```

计数写入位置是 [`BenchmarkApplication::fromApp()` 第 156 行](../src/fix_parse_benchmark.cpp#L156) 到
[`第 159 行`](../src/fix_parse_benchmark.cpp#L159)，等待端读取位置是
[`waitForMessages()` 第 1806 行](../src/fix_parse_benchmark.cpp#L1806)。

raw server 的计时区间是 [`第 1940 行`](../src/fix_parse_benchmark.cpp#L1940) 到
[`第 1948 行`](../src/fix_parse_benchmark.cpp#L1948)，包括发送、网络接收、完整消息切分、字段解析、
Session 处理和最终 `fromApp()` 计数。

### 2.6 `client=quickfix` 严格调用链

#### 2.6.1 启动 acceptor 和 initiator

[`runQuickfixServerBenchmark()` 第 1984 行](../src/fix_parse_benchmark.cpp#L1984) 调用
`makeAcceptorConfig(...)`<br>
-> [`makeAcceptorConfig()` 定义第 553 行](../src/fix_parse_benchmark.cpp#L553)

[`runQuickfixServerBenchmark()` 第 1987 行](../src/fix_parse_benchmark.cpp#L1987) 调用 `acceptor.start()`<br>
-> [`Acceptor::start()` 定义第 146 行](../src/C++/Acceptor.cpp#L146)

[`runQuickfixServerBenchmark()` 第 1997 行](../src/fix_parse_benchmark.cpp#L1997) 调用
`makeInitiatorConfig(...)`<br>
-> [`makeInitiatorConfig()` 定义第 602 行](../src/fix_parse_benchmark.cpp#L602)

[`runQuickfixServerBenchmark()` 第 2000 行](../src/fix_parse_benchmark.cpp#L2000) 调用
`initiator->start()`<br>
-> [`Initiator::start()` 定义第 182 行](../src/C++/Initiator.cpp#L182)

[`runQuickfixServerBenchmark()` 第 2002 行](../src/fix_parse_benchmark.cpp#L2002) 调用
`waitForLogon(...)`<br>
-> [`waitForLogon()` 定义第 1784 行](../src/fix_parse_benchmark.cpp#L1784)

#### 2.6.2 计时区间内构造 typed message 并发送

[`runQuickfixServerBenchmark()` 第 2012 行](../src/fix_parse_benchmark.cpp#L2012) 调用
`makeQuickfixApplicationMessage(...)`<br>
-> [`makeQuickfixApplicationMessage()` 定义第 683 行](../src/fix_parse_benchmark.cpp#L683)

[`runQuickfixServerBenchmark()` 第 2013 行](../src/fix_parse_benchmark.cpp#L2013) 调用
`Session::sendToTarget(message, clientSessionID)`<br>
-> [`Session::sendToTarget(Message&, SessionID)` 定义第 1329 行](../src/C++/Session.cpp#L1329)

[`Session::sendToTarget()` 第 1335 行](../src/C++/Session.cpp#L1335) 调用 `pSession->send(message)`<br>
-> [`Session::send(Message&)` 定义第 538 行](../src/C++/Session.cpp#L538)

[`Session::send(Message&)` 第 541 行](../src/C++/Session.cpp#L541) 调用 `sendRaw(message)`<br>
-> [`Session::sendRaw()` 定义第 544 行](../src/C++/Session.cpp#L544)

[`Session::sendRaw()` 第 592 行](../src/C++/Session.cpp#L592) 调用 `message.toString(messageString)`<br>
-> [`Message::toString(std::string&)` 定义第 383 行](../src/C++/Message.cpp#L383)

[`Session::sendRaw()` 第 599 行](../src/C++/Session.cpp#L599) 调用 `send(messageString)`<br>
-> [`Session::send(const std::string&)` 定义第 613 行](../src/C++/Session.cpp#L613)

[`Session::send(const std::string&)` 第 618 行](../src/C++/Session.cpp#L618) 动态调用
`m_pResponder->send(string)`<br>
-> [`SocketConnection::send()` 定义第 82 行](../src/C++/SocketConnection.cpp#L82)

因此 `client=quickfix` 的计时包含 typed message 构造、QuickFIX 补齐 Session header、序列化、client 发送队列，
以及 server 端完整接收处理。它不像 raw client 那样在计时前预构造一个大字节串。

[`runQuickfixServerBenchmark()` 第 2017 行](../src/fix_parse_benchmark.cpp#L2017) 调用
`waitForMessages(...)`<br>
-> [`waitForMessages()` 定义第 1803 行](../src/fix_parse_benchmark.cpp#L1803)

计时区间是 [`第 2010 行`](../src/fix_parse_benchmark.cpp#L2010) 到
[`第 2023 行`](../src/fix_parse_benchmark.cpp#L2023)。

## 3. 网络优化：先看原始 poll，再看两个分支

网络模式有三个：

```text
blocking
  -> 原始 SocketMonitor poll()，无事件时阻塞等待

poll0
  -> 仍走原始 SocketMonitor 全链路，只把 poll() 的 timeout 改成 0

direct
  -> 从 SocketAcceptor::onStart() 分出独立循环，直接 accept()/recv()/send()
```

编译期开关位于：

- [`QUICKFIX_BUSY_POLL` 第 67 行](../CMakeLists.txt#L67)
- [`QUICKFIX_DIRECT_READ_POLL` 第 68 行](../CMakeLists.txt#L68)
- 对 QuickFIX 库添加对应宏的位置：
  [`src/C++/CMakeLists.txt` 第 115 行](../src/C++/CMakeLists.txt#L115) 和
  [`第 119 行`](../src/C++/CMakeLists.txt#L119)

### 3.1 三种模式共同的 acceptor 启动链

benchmark 中有两个真实的 `acceptor.start()` 调用点：

- raw client：[`runRawServerBenchmark()` 第 1919 行](../src/fix_parse_benchmark.cpp#L1919)
- QuickFIX client：[`runQuickfixServerBenchmark()` 第 1987 行](../src/fix_parse_benchmark.cpp#L1987)

二者都进入：

`acceptor.start()`<br>
-> [`Acceptor::start()` 定义第 146 行](../src/C++/Acceptor.cpp#L146)

[`Acceptor::start()` 第 155 行](../src/C++/Acceptor.cpp#L155) 动态调用 `onConfigure(m_settings)`<br>
-> [`SocketAcceptor::onConfigure()` 定义第 103 行](../src/C++/SocketAcceptor.cpp#L103)

[`Acceptor::start()` 第 156 行](../src/C++/Acceptor.cpp#L156) 动态调用 `onInitialize(m_settings)`<br>
-> [`SocketAcceptor::onInitialize()` 定义第 210 行](../src/C++/SocketAcceptor.cpp#L210)

[`Acceptor::start()` 第 164 行](../src/C++/Acceptor.cpp#L164) 把 `Acceptor::startThread` 注册给
`thread_spawn(...)`<br>
-> 新线程入口 [`Acceptor::startThread()` 定义第 251 行](../src/C++/Acceptor.cpp#L251)

[`Acceptor::startThread()` 第 254 行](../src/C++/Acceptor.cpp#L254) 动态调用 `pAcceptor->onStart()`<br>
-> [`SocketAcceptor::onStart()` 定义第 290 行](../src/C++/SocketAcceptor.cpp#L290)

到这里才进入真正长期运行的 acceptor 网络线程。

### 3.2 模式配置如何到达网络线程

benchmark 在 [`makeAcceptorConfig()` 第 572 行](../src/fix_parse_benchmark.cpp#L572) 写入
`SocketBusyPollMode=direct`，或在 [`第 574 行`](../src/fix_parse_benchmark.cpp#L574) 到
[`第 575 行`](../src/fix_parse_benchmark.cpp#L575) 写入 `SocketBusyPoll=Y`。

`SocketAcceptor::onInitialize()` 读取这些配置：

- [`第 235 行`](../src/C++/SocketAcceptor.cpp#L235) 到
  [`第 237 行`](../src/C++/SocketAcceptor.cpp#L237) 读取 `SocketBusyPoll`。
- [`第 238 行`](../src/C++/SocketAcceptor.cpp#L238) 到
  [`第 265 行`](../src/C++/SocketAcceptor.cpp#L265) 把文本模式转换为 `m_busyPoll` 或
  `m_directReadPoll`。

poll0 还存在一条真实的 setter 调用链：

[`SocketAcceptor::onInitialize()` 第 269 行](../src/C++/SocketAcceptor.cpp#L269) 调用
`m_pServer->setBusyPoll(m_busyPoll)`<br>
-> [`SocketServer::setBusyPoll()` 定义第 278 行](../src/C++/SocketServer.cpp#L278)

[`SocketServer::setBusyPoll()` 第 278 行](../src/C++/SocketServer.cpp#L278) 调用
`m_monitor.setBusyPoll(enabled)`<br>
-> [`SocketMonitor::setBusyPoll()` 定义第 249 行](../src/C++/SocketMonitor_UNIX.cpp#L249)

如果启用了 poll0 或 direct，并设置了 CPU：

[`SocketAcceptor::onStart()` 第 298 行](../src/C++/SocketAcceptor.cpp#L298) 调用
`setCurrentThreadAffinity(m_busyPollCpu)`<br>
-> [`setCurrentThreadAffinity()` 定义第 48 行](../src/C++/SocketAcceptor.cpp#L48)

[`setCurrentThreadAffinity()` 第 53 行](../src/C++/SocketAcceptor.cpp#L53) 调用
`pthread_setaffinity_np(...)`，绑定的正是当前 acceptor 网络线程。

### 3.3 原始 blocking poll 的接收主链

[`SocketAcceptor::onStart()` 第 314 行](../src/C++/SocketAcceptor.cpp#L314) 调用
`m_pServer->block(*this)`<br>
-> [`SocketServer::block()` 定义第 258 行](../src/C++/SocketServer.cpp#L258)

[`SocketServer::block()` 第 269 行](../src/C++/SocketServer.cpp#L269) 调用
`m_monitor.block(wrapper, poll, timeout)`<br>
-> [`SocketMonitor::block()` 定义第 305 行](../src/C++/SocketMonitor_UNIX.cpp#L305)

`SocketMonitor::block()` 先把三类 socket 填入同一个 `pollfd[]`：

[`SocketMonitor::block()` 第 316 行](../src/C++/SocketMonitor_UNIX.cpp#L316) 到
[`第 318 行`](../src/C++/SocketMonitor_UNIX.cpp#L318) 调用 `buildSet(...)`<br>
-> [`SocketMonitor::buildSet()` 定义第 446 行](../src/C++/SocketMonitor_UNIX.cpp#L446)

随后到达最核心的系统调用：

[`SocketMonitor::block()` 第 334 行](../src/C++/SocketMonitor_UNIX.cpp#L334)

```cpp
result = poll(pfds, pfds_size, m_busyPoll ? 0 : getTimeval(should_poll, timeout));
```

blocking 模式下 `m_busyPoll=false`，因此第三个参数来自：

[`poll()` 参数中的调用第 334 行](../src/C++/SocketMonitor_UNIX.cpp#L334)<br>
-> [`SocketMonitor::getTimeval()` 定义第 183 行](../src/C++/SocketMonitor_UNIX.cpp#L183)

这时 `poll()` 会等待某个 fd 就绪，或等到 timeout。`poll()` 返回后，`pfds[i].revents` 保存每个 fd
实际发生的可读、可写、断开或错误状态。

当 `result > 0`：

[`SocketMonitor::block()` 第 363 行](../src/C++/SocketMonitor_UNIX.cpp#L363) 调用
`processPollList(strategy, pfds, pfds_size)`<br>
-> [`SocketMonitor::processPollList()` 定义第 424 行](../src/C++/SocketMonitor_UNIX.cpp#L424)

可读事件继续：

[`processPollList()` 第 427 行](../src/C++/SocketMonitor_UNIX.cpp#L427) 调用
`processRead(strategy, pfds[i].fd)`<br>
-> [`SocketMonitor::processRead()` 定义第 378 行](../src/C++/SocketMonitor_UNIX.cpp#L378)

[`SocketMonitor::processRead()` 第 385 行](../src/C++/SocketMonitor_UNIX.cpp#L385) 动态调用
`strategy.onEvent(*this, s)`<br>
-> [`ServerWrapper::onEvent()` 定义第 77 行](../src/C++/SocketServer.cpp#L77)

此后根据 fd 类型分成 listener 和已连接 client 两条支路。

### 3.4 原始 poll 的 listener/accept 支路

如果可读 fd 是监听 socket：

[`ServerWrapper::onEvent()` 第 79 行](../src/C++/SocketServer.cpp#L79) 调用
`m_server.accept(socket)`<br>
-> [`SocketServer::accept()` 定义第 174 行](../src/C++/SocketServer.cpp#L174)

[`SocketServer::accept()` 第 177 行](../src/C++/SocketServer.cpp#L177) 调用 `socket_accept(socket)`<br>
-> [`socket_accept()` 定义第 258 行](../src/C++/Utility.cpp#L258)

同一条 [`ServerWrapper::onEvent()` 第 79 行](../src/C++/SocketServer.cpp#L79) 随后动态调用
`m_strategy.onConnect(...)`<br>
-> [`SocketAcceptor::onConnect()` 定义第 466 行](../src/C++/SocketAcceptor.cpp#L466)

[`SocketAcceptor::onConnect()` 第 479 行](../src/C++/SocketAcceptor.cpp#L479) 构造
`SocketConnection`<br>
-> [`SocketConnection` 构造函数定义第 39 行](../src/C++/SocketConnection.cpp#L39)

这里的 `accept()` 只负责从监听队列取出一个“新连接”，不会读取该连接中的 FIX 数据。

### 3.5 原始 poll 的已连接 client/recv 支路

如果可读 fd 是已经 accept 的 client socket：

[`ServerWrapper::onEvent()` 第 81 行](../src/C++/SocketServer.cpp#L81) 动态调用
`m_strategy.onData(m_server, socket)`<br>
-> [`SocketAcceptor::onData()` 定义第 521 行](../src/C++/SocketAcceptor.cpp#L521)

[`SocketAcceptor::onData()` 第 527 行](../src/C++/SocketAcceptor.cpp#L527) 调用
`pSocketConnection->read(*this, server)`<br>
-> [`SocketConnection::read(SocketAcceptor&, SocketServer&)` 定义第 198 行](../src/C++/SocketConnection.cpp#L198)

#### 第一条 Logon 的会话识别支路

连接尚未绑定 `Session` 时，外层 `poll()` 已经报告可读，但代码还要保证能拿到一条完整 Logon：

[`SocketConnection::read()` 第 210 行](../src/C++/SocketConnection.cpp#L210) 调用 `readMessage(message)`<br>
-> [`SocketConnection::readMessage()` 定义第 401 行](../src/C++/SocketConnection.cpp#L401)

[`SocketConnection::readMessage()` 第 403 行](../src/C++/SocketConnection.cpp#L403) 调用
`m_parser.readFixMessage(msg)`<br>
-> [`Parser::readFixMessage()` 定义第 295 行](../src/C++/Parser.cpp#L295)

若 Logon 还不完整，[`SocketConnection::read()` 第 214 行](../src/C++/SocketConnection.cpp#L214) 使用一个
最长 1000 ms 的局部 `poll()` 等待剩余字节；它只发生在尚未识别 Session 的建连阶段。

局部 `poll()` 报告可读后：

[`SocketConnection::read()` 第 221 行](../src/C++/SocketConnection.cpp#L221) 调用 `readFromSocket()`<br>
-> [`SocketConnection::readFromSocket()` 定义第 385 行](../src/C++/SocketConnection.cpp#L385)

[`SocketConnection::readFromSocket()` 第 386 行](../src/C++/SocketConnection.cpp#L386) 调用
`socket_recv(...)`<br>
-> [`socket_recv()` 定义第 265 行](../src/C++/Utility.cpp#L265)

[`SocketConnection::readFromSocket()` 第 390 行](../src/C++/SocketConnection.cpp#L390) 调用
`m_parser.addToStream(...)`<br>
-> [`Parser::addToStream()` 定义第 43 行](../include/quickfix/Parser.h#L43)

取得完整 Logon 后：

[`SocketConnection::read()` 第 230 行](../src/C++/SocketConnection.cpp#L230) 调用
`Session::lookupSession(message, true)`<br>
-> [`Session::lookupSession(string, reverse)` 定义第 1373 行](../src/C++/Session.cpp#L1373)

[`SocketConnection::read()` 第 239 行](../src/C++/SocketConnection.cpp#L239) 调用
`acceptor.getSession(message, *this)`<br>
-> [`Acceptor::getSession()` 定义第 101 行](../src/C++/Acceptor.cpp#L101)

[`SocketConnection::read()` 第 242 行](../src/C++/SocketConnection.cpp#L242) 调用
`m_pSession->next(message, now)`<br>
-> [`Session::next(const std::string&, ...)` 定义第 1170 行](../src/C++/Session.cpp#L1170)

#### Session 建立后的稳定接收支路

[`SocketConnection::read()` 第 266 行](../src/C++/SocketConnection.cpp#L266) 调用 `readFromSocket()`<br>
-> [`SocketConnection::readFromSocket()` 定义第 385 行](../src/C++/SocketConnection.cpp#L385)

[`SocketConnection::read()` 第 267 行](../src/C++/SocketConnection.cpp#L267) 调用 `readMessages(...)`<br>
-> [`SocketConnection::readMessages()` 定义第 411 行](../src/C++/SocketConnection.cpp#L411)

如果构建时打开网络诊断，对应调用点是 [`第 262 行`](../src/C++/SocketConnection.cpp#L262) 和
[`第 264 行`](../src/C++/SocketConnection.cpp#L264)，实际解析流程相同。

### 3.6 从连接缓冲区到 `fromApp()` 的共同链路

这条链被 blocking、poll0 和 direct 三种模式共同复用：

[`SocketConnection::readMessages()` 第 418 行](../src/C++/SocketConnection.cpp#L418) 调用
`readMessage(message)`<br>
-> [`SocketConnection::readMessage()` 定义第 401 行](../src/C++/SocketConnection.cpp#L401)

[`SocketConnection::readMessage()` 第 403 行](../src/C++/SocketConnection.cpp#L403) 调用
`Parser::readFixMessage(msg)`<br>
-> [`Parser::readFixMessage()` 定义第 295 行](../src/C++/Parser.cpp#L295)

`Parser::readFixMessage()` 只负责从 TCP 字节流中切出一条完整的
`8=...<SOH>9=...<SOH>...<SOH>10=...<SOH>`。打开 SIMD stream parser 后，其入口位于：

[`Parser::readFixMessage()` 第 297 行](../src/C++/Parser.cpp#L297) 调用
`tryReadFixMessageFast(str, m_buffer)`<br>
-> [`tryReadFixMessageFast()` 定义第 193 行](../src/C++/Parser.cpp#L193)

切出完整消息后：

[`SocketConnection::readMessages()` 第 425 行](../src/C++/SocketConnection.cpp#L425) 调用
`m_pSession->next(message, now)`<br>
-> [`Session::next(const std::string&, ...)` 定义第 1170 行](../src/C++/Session.cpp#L1170)

[`Session::next(string)` 第 1179 行](../src/C++/Session.cpp#L1179) 构造 `Message` 并调用重载
`next(Message, now, queued)`<br>
-> [`Session::next(const Message&, ...)` 定义第 1194 行](../src/C++/Session.cpp#L1194)

[`Session::next(Message)` 第 1250 行](../src/C++/Session.cpp#L1250) 调用 `verify(message)`<br>
-> [`Session::verify()` 定义第 980 行](../src/C++/Session.cpp#L980)

[`Session::verify()` 第 1036 行](../src/C++/Session.cpp#L1036) 调用 `fromCallback(...)`<br>
-> [`Session::fromCallback()` 定义第 1069 行](../src/C++/Session.cpp#L1069)

应用消息走：

[`Session::fromCallback()` 第 1073 行](../src/C++/Session.cpp#L1073) 动态调用
`m_application.fromApp(...)`<br>
-> [`BenchmarkApplication::fromApp()` 定义第 156 行](../src/fix_parse_benchmark.cpp#L156)

因此网络优化只改变“何时发现 socket 有工作、如何触发 recv/send”；完整消息进入 Parser 之后的 Session 和
Application 链没有另写一套。

### 3.7 原始 poll 的发送链路

当 Session 产生 Logon、Heartbeat、ExecutionReport 等待发送消息时，最终进入：

[`Session::send(const std::string&)` 第 618 行](../src/C++/Session.cpp#L618) 动态调用
`m_pResponder->send(string)`<br>
-> [`SocketConnection::send()` 定义第 82 行](../src/C++/SocketConnection.cpp#L82)

[`SocketConnection::send()` 第 85 行](../src/C++/SocketConnection.cpp#L85) 先把消息加入 `m_sendQueue`。

[`SocketConnection::send()` 第 92 行](../src/C++/SocketConnection.cpp#L92) 调用 `processQueue()`<br>
-> [`SocketConnection::processQueue()` 定义第 97 行](../src/C++/SocketConnection.cpp#L97)

[`SocketConnection::processQueue()` 第 112 行](../src/C++/SocketConnection.cpp#L112) 使用
`poll(&pfd, 1, 0)` 做一次不阻塞的 `POLLOUT` 检查。可写时：

[`SocketConnection::processQueue()` 第 119 行](../src/C++/SocketConnection.cpp#L119) 调用
`socket_send(...)`<br>
-> [`socket_send()` 定义第 273 行](../src/C++/Utility.cpp#L273)

无论首轮是否完全清空队列，发送入口随后都会：

[`SocketConnection::send()` 第 93 行](../src/C++/SocketConnection.cpp#L93) 调用 `signal()`<br>
-> [`SocketConnection::signal()` 定义第 85 行](../src/C++/SocketConnection.h#L85)

[`SocketConnection::signal()` 第 88 行](../src/C++/SocketConnection.h#L88) 调用
`m_pMonitor->signal(m_socket)`<br>
-> [`SocketMonitor::signal()` 定义第 228 行](../src/C++/SocketMonitor_UNIX.cpp#L228)

`signal()` 通过 monitor 的内部唤醒 socket 传递“这个 client 还需要写”：

[`SocketMonitor::processRead()` 第 382 行](../src/C++/SocketMonitor_UNIX.cpp#L382) 读取待写 fd，随后在
[`第 383 行`](../src/C++/SocketMonitor_UNIX.cpp#L383) 调用 `addWrite(socket)`<br>
-> [`SocketMonitor::addWrite()` 定义第 107 行](../src/C++/SocketMonitor_UNIX.cpp#L107)

下一轮主 `poll()` 报告该 fd 可写时：

[`processPollList()` 第 431 行](../src/C++/SocketMonitor_UNIX.cpp#L431) 调用 `processWrite(...)`<br>
-> [`SocketMonitor::processWrite()` 定义第 398 行](../src/C++/SocketMonitor_UNIX.cpp#L398)

[`SocketMonitor::processWrite()` 第 405 行](../src/C++/SocketMonitor_UNIX.cpp#L405) 动态调用
`strategy.onWrite(...)`<br>
-> [`ServerWrapper::onWrite()` 定义第 94 行](../src/C++/SocketServer.cpp#L94)

[`ServerWrapper::onWrite()` 第 94 行](../src/C++/SocketServer.cpp#L94) 动态调用
`m_strategy.onWrite(...)`<br>
-> [`SocketAcceptor::onWrite()` 定义第 500 行](../src/C++/SocketAcceptor.cpp#L500)

[`SocketAcceptor::onWrite()` 第 506 行](../src/C++/SocketAcceptor.cpp#L506) 再调用 `processQueue()`<br>
-> [`SocketConnection::processQueue()` 定义第 97 行](../src/C++/SocketConnection.cpp#L97)

队列清空后：

[`SocketAcceptor::onWrite()` 第 507 行](../src/C++/SocketAcceptor.cpp#L507) 调用 `unsignal()`<br>
-> [`SocketConnection::unsignal()` 定义第 92 行](../src/C++/SocketConnection.h#L92)

[`SocketConnection::unsignal()` 第 95 行](../src/C++/SocketConnection.h#L95) 调用
`m_pMonitor->unsignal(m_socket)`<br>
-> [`SocketMonitor::unsignal()` 定义第 235 行](../src/C++/SocketMonitor_UNIX.cpp#L235)

### 3.8 原始 blocking poll 的 timeout/心跳链

blocking `poll()` 超时后：

[`SocketMonitor::block()` 第 360 行](../src/C++/SocketMonitor_UNIX.cpp#L360) 动态调用
`strategy.onTimeout(*this)`<br>
-> [`ServerWrapper::onTimeout()` 定义第 119 行](../src/C++/SocketServer.cpp#L119)

[`ServerWrapper::onTimeout()` 第 119 行](../src/C++/SocketServer.cpp#L119) 动态调用
`m_strategy.onTimeout(m_server)`<br>
-> [`SocketAcceptor::onTimeout()` 定义第 556 行](../src/C++/SocketAcceptor.cpp#L556)

[`SocketAcceptor::onTimeout()` 第 559 行](../src/C++/SocketAcceptor.cpp#L559) 调用每个连接的
`onTimeout()`<br>
-> [`SocketConnection::onTimeout()` 定义第 440 行](../src/C++/SocketConnection.cpp#L440)

[`SocketConnection::onTimeout()` 第 442 行](../src/C++/SocketConnection.cpp#L442) 调用
`m_pSession->next(now)`<br>
-> [`Session::next(const UtcTimeStamp&)` 定义第 119 行](../src/C++/Session.cpp#L119)

Heartbeat、TestRequest、Logout 等状态机都在原有 `Session::next(now)` 内。

### 3.9 第一分支：poll0

poll0 没有重写 `SocketServer::block()`、`pollfd[]`、事件分发、recv、Parser 或 Session。

它仍然从：

[`SocketAcceptor::onStart()` 第 314 行](../src/C++/SocketAcceptor.cpp#L314)<br>
-> [`SocketServer::block()` 第 258 行](../src/C++/SocketServer.cpp#L258)<br>
-> [`SocketMonitor::block()` 第 305 行](../src/C++/SocketMonitor_UNIX.cpp#L305)

唯一核心差异仍是 [`SocketMonitor_UNIX.cpp` 第 334 行](../src/C++/SocketMonitor_UNIX.cpp#L334)：

```cpp
m_busyPoll ? 0 : getTimeval(should_poll, timeout)
```

`m_busyPoll=true` 时第三个参数直接为 `0`，所以 `poll()` 检查完 `pfds` 就立即返回。外层
[`SocketAcceptor::onStart()` 第 314 行](../src/C++/SocketAcceptor.cpp#L314) 的 `while` 马上进入下一轮，
形成 busy-poll。

当本轮没有事件时：

[`SocketMonitor::block()` 第 357 行](../src/C++/SocketMonitor_UNIX.cpp#L357) 调用
`busyPollTimeoutElapsed()`<br>
-> [`SocketMonitor::busyPollTimeoutElapsed()` 定义第 280 行](../src/C++/SocketMonitor_UNIX.cpp#L280)

只有累计到定时间隔才继续触发原来的 `onTimeout()` 链，避免零超时循环每一轮都执行 Session 心跳任务。

一旦 `poll()` 返回可读或可写 fd，poll0 与 blocking 都从
[`processPollList()` 调用点第 363 行](../src/C++/SocketMonitor_UNIX.cpp#L363) 进入完全相同的后续链路。

### 3.10 第二分支：direct

direct 不进入 [`SocketAcceptor::onStart()` 第 314 行](../src/C++/SocketAcceptor.cpp#L314) 的
`SocketServer::block()` 分支，而是在 [`第 309 行`](../src/C++/SocketAcceptor.cpp#L309) 命中
`m_directReadPoll` 后进入 [`第 310 行`](../src/C++/SocketAcceptor.cpp#L310) 的独立循环。

其完整调用链见下一节。

## 4. Direct 模式严格调用链

### 4.1 从 `onStart()` 进入一轮 direct scan

[`SocketAcceptor::onStart()` 第 310 行](../src/C++/SocketAcceptor.cpp#L310) 调用
`runDirectScanOnce()`<br>
-> [`SocketAcceptor::runDirectScanOnce()` 定义第 359 行](../src/C++/SocketAcceptor.cpp#L359)

一轮 `runDirectScanOnce()` 的固定顺序是：

```text
尝试 accept 一个新连接
  -> 每个现有连接尝试 recv 一次
  -> 同一个连接尝试推进 send queue 一次
  -> 检查是否该触发一秒定时任务
  -> 返回 onStart()，立刻开始下一轮
```

### 4.2 Direct accept 链

[`runDirectScanOnce()` 第 366 行](../src/C++/SocketAcceptor.cpp#L366) 调用
`m_pServer->acceptDirect()`<br>
-> [`SocketServer::acceptDirect()` 定义第 202 行](../src/C++/SocketServer.cpp#L202)

[`SocketServer::acceptDirect()` 第 209 行](../src/C++/SocketServer.cpp#L209) 调用
`socket_accept(acceptSocket)`<br>
-> [`socket_accept()` 定义第 258 行](../src/C++/Utility.cpp#L258)

`EAGAIN/EWOULDBLOCK` 在 [`第 214 行`](../src/C++/SocketServer.cpp#L214) 被解释为“本轮没有待接连接”，
不会关闭 listener。

accept 成功后：

[`runDirectScanOnce()` 第 368 行](../src/C++/SocketAcceptor.cpp#L368) 调用
`onConnect(...)`<br>
-> [`SocketAcceptor::onConnect()` 定义第 466 行](../src/C++/SocketAcceptor.cpp#L466)

[`SocketAcceptor::onConnect()` 第 479 行](../src/C++/SocketAcceptor.cpp#L479) 构造
`SocketConnection`<br>
-> [`SocketConnection` 构造函数定义第 39 行](../src/C++/SocketConnection.cpp#L39)

[`SocketAcceptor::onConnect()` 第 482 行](../src/C++/SocketAcceptor.cpp#L482) 调用
`connection->setDirectReadPoll(m_directReadPoll)`<br>
-> [`SocketConnection::setDirectReadPoll()` 定义第 81 行](../src/C++/SocketConnection.h#L81)

这个标志让该 acceptor 连接的后续发送也由 direct scan 驱动。

### 4.3 Direct recv 与 Parser 链

[`runDirectScanOnce()` 第 380 行](../src/C++/SocketAcceptor.cpp#L380) 调用
`socketConnection->readDirect(...)`<br>
-> [`SocketConnection::readDirect()` 定义第 295 行](../src/C++/SocketConnection.cpp#L295)

[`SocketConnection::readDirect()` 第 296 行](../src/C++/SocketConnection.cpp#L296) 调用
`detail::directSocketRead(...)`<br>
-> [`directSocketRead()` 定义第 37 行](../src/C++/detail/DirectSocketRead.cpp#L37)

[`directSocketRead()` 第 40 行](../src/C++/detail/DirectSocketRead.cpp#L40) 调用 `socket_recv(...)`<br>
-> [`socket_recv()` 定义第 265 行](../src/C++/Utility.cpp#L265)

返回值分类为：

- `bytes > 0`：[`DirectSocketRead.cpp` 第 43 行](../src/C++/detail/DirectSocketRead.cpp#L43)，本轮读到数据。
- `bytes == 0`：[`第 46 行`](../src/C++/detail/DirectSocketRead.cpp#L46)，对端关闭连接。
- `EAGAIN/EWOULDBLOCK`：[`第 51 行`](../src/C++/detail/DirectSocketRead.cpp#L51)，本轮没有数据，不是错误。
- 其他错误：[`第 55 行`](../src/C++/detail/DirectSocketRead.cpp#L55)，连接需要断开。

读到数据后：

[`SocketConnection::readDirect()` 第 319 行](../src/C++/SocketConnection.cpp#L319) 调用
`m_parser.addToStream(...)`<br>
-> [`Parser::addToStream()` 定义第 43 行](../include/quickfix/Parser.h#L43)

如果尚未识别 Session：

[`SocketConnection::readDirect()` 第 327 行](../src/C++/SocketConnection.cpp#L327) 调用
`readMessage(message)`<br>
-> [`SocketConnection::readMessage()` 定义第 401 行](../src/C++/SocketConnection.cpp#L401)

[`SocketConnection::readDirect()` 第 331 行](../src/C++/SocketConnection.cpp#L331) 调用
`Session::lookupSession(message, true)`<br>
-> [`Session::lookupSession()` 定义第 1373 行](../src/C++/Session.cpp#L1373)

[`SocketConnection::readDirect()` 第 340 行](../src/C++/SocketConnection.cpp#L340) 调用
`acceptor.getSession(message, *this)`<br>
-> [`Acceptor::getSession()` 定义第 101 行](../src/C++/Acceptor.cpp#L101)

[`SocketConnection::readDirect()` 第 343 行](../src/C++/SocketConnection.cpp#L343) 调用
`m_pSession->next(message, now)`<br>
-> [`Session::next(const std::string&, ...)` 定义第 1170 行](../src/C++/Session.cpp#L1170)

Session 建立后：

[`SocketConnection::readDirect()` 第 365 行](../src/C++/SocketConnection.cpp#L365) 调用
`readMessages(server.getMonitor())`<br>
-> [`SocketConnection::readMessages()` 定义第 411 行](../src/C++/SocketConnection.cpp#L411)

打开网络诊断时，对应调用点为 [`第 363 行`](../src/C++/SocketConnection.cpp#L363)，定义入口为
[`第 409 行`](../src/C++/SocketConnection.cpp#L409)。

从 `readMessages()` 开始，direct 完全复用第 3.6 节的
`Parser::readFixMessage() -> Session::next() -> verify() -> fromApp()` 链。

### 4.4 Direct send 与部分发送链

Session 仍通过原有 Responder 接口发送：

[`Session::send(const std::string&)` 第 618 行](../src/C++/Session.cpp#L618)<br>
-> [`SocketConnection::send()` 定义第 82 行](../src/C++/SocketConnection.cpp#L82)

[`SocketConnection::send()` 第 85 行](../src/C++/SocketConnection.cpp#L85) 仍把消息放进原来的
`m_sendQueue`。direct 标志命中后在 [`第 87 行`](../src/C++/SocketConnection.cpp#L87) 到
[`第 89 行`](../src/C++/SocketConnection.cpp#L89) 返回，不进入 poll-based `processQueue()` 和
`signal()`。

下一轮扫描：

[`runDirectScanOnce()` 第 386 行](../src/C++/SocketAcceptor.cpp#L386) 调用
`socketConnection->processQueueDirect()`<br>
-> [`SocketConnection::processQueueDirect()` 定义第 143 行](../src/C++/SocketConnection.cpp#L143)

[`processQueueDirect()` 第 153 行](../src/C++/SocketConnection.cpp#L153) 调用
`detail::directSocketWrite(...)`<br>
-> [`directSocketWrite()` 定义第 37 行](../src/C++/detail/DirectSocketWrite.cpp#L37)

[`directSocketWrite()` 第 44 行](../src/C++/detail/DirectSocketWrite.cpp#L44) 调用
`socket_send(...)`<br>
-> [`socket_send()` 定义第 273 行](../src/C++/Utility.cpp#L273)

部分发送时：

- [`processQueueDirect()` 第 157 行](../src/C++/SocketConnection.cpp#L157) 把成功发送的字节数累加到
  `m_sendLength`。
- 只有在 [`第 158 行`](../src/C++/SocketConnection.cpp#L158) 判断整条消息已发送完，才在
  [`第 160 行`](../src/C++/SocketConnection.cpp#L160) 从队列移除。
- `EAGAIN` 在 [`第 165 行`](../src/C++/SocketConnection.cpp#L165) 到
  [`第 167 行`](../src/C++/SocketConnection.cpp#L167) 保留消息和偏移，下一轮从未发送位置继续。

### 4.5 Direct 定时器补偿链

direct 不调用 `SocketMonitor::block()`，因此不会自然到达原 poll timeout。每轮末尾使用单调时钟补偿：

[`runDirectScanOnce()` 第 395 行](../src/C++/SocketAcceptor.cpp#L395) 调用
`onTimeout(*m_pServer)`<br>
-> [`SocketAcceptor::onTimeout()` 定义第 556 行](../src/C++/SocketAcceptor.cpp#L556)

[`SocketAcceptor::onTimeout()` 第 559 行](../src/C++/SocketAcceptor.cpp#L559)<br>
-> [`SocketConnection::onTimeout()` 定义第 440 行](../src/C++/SocketConnection.cpp#L440)

[`SocketConnection::onTimeout()` 第 442 行](../src/C++/SocketConnection.cpp#L442)<br>
-> [`Session::next(const UtcTimeStamp&)` 定义第 119 行](../src/C++/Session.cpp#L119)

因此 direct 重建的是“定时任务由谁触发”，没有重写 Heartbeat、TestRequest、Logout 和握手超时状态机。

### 4.6 Direct 断开清理链

direct read 或 write 返回不可恢复的断开状态时：

[`runDirectScanOnce()` 第 382 行](../src/C++/SocketAcceptor.cpp#L382) 或
[`第 388 行`](../src/C++/SocketAcceptor.cpp#L388) 调用 `disconnectDirect(socket)`<br>
-> [`SocketAcceptor::disconnectDirect()` 定义第 406 行](../src/C++/SocketAcceptor.cpp#L406)

[`disconnectDirect()` 第 410 行](../src/C++/SocketAcceptor.cpp#L410) 调用
`m_pServer->getMonitor().dropDirect(socket)`<br>
-> [`SocketMonitor::dropDirect()` 定义第 148 行](../src/C++/SocketMonitor_UNIX.cpp#L148)

[`disconnectDirect()` 第 411 行](../src/C++/SocketAcceptor.cpp#L411) 调用
`onDisconnect(...)`<br>
-> [`SocketAcceptor::onDisconnect()` 定义第 530 行](../src/C++/SocketAcceptor.cpp#L530)

[`SocketAcceptor::onDisconnect()` 第 539 行](../src/C++/SocketAcceptor.cpp#L539) 调用
`pSession->disconnect()`<br>
-> [`Session::disconnect()` 定义第 621 行](../src/C++/Session.cpp#L621)

最后 [`SocketAcceptor::onDisconnect()` 第 542 行](../src/C++/SocketAcceptor.cpp#L542) 删除连接对象，并在
[`第 543 行`](../src/C++/SocketAcceptor.cpp#L543) 从 `m_connections` 移除。

## 5. SIMD 消息切分

### 5.1 它位于总链路的哪一层

SIMD stream parser 优化的是 TCP 字节流中“一条完整 FIX 消息从哪里开始、在哪里结束”，不是
`MessageCracker`，也不是业务字段回调。

它位于三种网络模式共同复用的链路中：

[`SocketConnection::readMessages()` 第 418 行](../src/C++/SocketConnection.cpp#L418) 调用
`readMessage(message)`<br>
-> [`SocketConnection::readMessage()` 定义第 401 行](../src/C++/SocketConnection.cpp#L401)

[`SocketConnection::readMessage()` 第 403 行](../src/C++/SocketConnection.cpp#L403) 调用
`m_parser.readFixMessage(msg)`<br>
-> [`Parser::readFixMessage()` 定义第 295 行](../src/C++/Parser.cpp#L295)

[`Parser::readFixMessage()` 第 297 行](../src/C++/Parser.cpp#L297) 调用
`tryReadFixMessageFast(str, m_buffer)`<br>
-> [`tryReadFixMessageFast()` 定义第 193 行](../src/C++/Parser.cpp#L193)

因此 blocking、poll0 和 direct 都能进入同一个 SIMD framing；raw client 与 QuickFIX client 发来的 wire
message 也都能进入这里。

### 5.2 三个独立编译开关

编译期开关位于：

| 开关 | 定义位置 | 作用 |
|---|---|---|
| `QUICKFIX_SIMD_STREAM_PARSER` | [`CMakeLists.txt` 第 64 行](../CMakeLists.txt#L64) | 接入 `Parser::readFixMessage()`，对完整消息 framing 使用快速扫描 |
| `QUICKFIX_SIMD_PATTERN_SCAN` | [`CMakeLists.txt` 第 65 行](../CMakeLists.txt#L65) | 在 stream parser 内用四字节 SIMD 专门查找 `SOH10=` |
| `QUICKFIX_SIMD_FIELD_SCAN` | [`CMakeLists.txt` 第 63 行](../CMakeLists.txt#L63) | 在字段解析阶段查找字段末尾 SOH，是独立实验，不是当前主要结论 |

对应的 QuickFIX 库编译宏在
[`src/C++/CMakeLists.txt` 第 99 行](../src/C++/CMakeLists.txt#L99) 到
[`第 108 行`](../src/C++/CMakeLists.txt#L108) 设置。

`QUICKFIX_SIMD_PATTERN_SCAN` 对 Parser 主链有意义的前提是同时打开
`QUICKFIX_SIMD_STREAM_PARSER`，因为 Parser 中的 checksum 快速入口整体位于
[`Parser.cpp` 第 36 行](../src/C++/Parser.cpp#L36) 的 stream-parser 条件编译块内。

`SIMD_FIELD_SCAN` 的独立接入点在：

[`Message.cpp` 第 44 行](../src/C++/Message.cpp#L44) 调用 `detail::findCharFast(...)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

它优化的是每个字段 value 末尾 SOH 的查找，不参与本节重点讲解的完整消息切分。

### 5.3 第一步：查找 `8=` 消息起点

[`tryReadFixMessageFast()` 第 194 行](../src/C++/Parser.cpp#L194) 调用
`findBeginMarkerFast(buffer)`<br>
-> [`findBeginMarkerFast()` 定义第 47 行](../src/C++/Parser.cpp#L47)

传入参数：

- `buffer`：`Parser` 持久保存的 TCP 字节流，可能包含半条、一条或多条 FIX 消息。

[`findBeginMarkerFast()` 第 57 行](../src/C++/Parser.cpp#L57) 调用
`detail::findCharFast(current, lastCandidate, '8')`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

这里三个参数分别是：

- `current`：本轮开始查找的位置。
- `lastCandidate`：最后一个还能安全检查后继 `'='` 的位置，不包含在搜索范围内。
- `'8'`：SIMD 批量比较的目标字符。

打开 `SIMD_STREAM_PARSER` 后：

[`findCharFast()` 第 137 行](../src/C++/detail/FastScan.cpp#L137) 调用
`findCharSimd(begin, end, target)`<br>
-> [`findCharSimd()` 定义第 103 行](../src/C++/detail/FastScan.cpp#L103)

[`findCharSimd()` 第 109 行](../src/C++/detail/FastScan.cpp#L109) 一次加载 16 字节，
[`第 110 行`](../src/C++/detail/FastScan.cpp#L110) 同时比较 16 个字节，
[`第 111 行`](../src/C++/detail/FastScan.cpp#L111) 生成 16 位匹配 mask。

mask 非零时：

[`findCharSimd()` 第 113 行](../src/C++/detail/FastScan.cpp#L113) 调用
`firstSetBit(mask)`<br>
-> [`firstSetBit()` 定义第 77 行](../src/C++/detail/FastScan.cpp#L77)

它返回最靠左的 `'8'` 候选位置；随后
[`findBeginMarkerFast()` 第 61 行](../src/C++/Parser.cpp#L61) 检查下一个字节是否为 `'='`。

不足 16 字节的尾部：

[`findCharSimd()` 第 118 行](../src/C++/detail/FastScan.cpp#L118) 调用
`findCharScalar(current, end, target)`<br>
-> [`findCharScalar()` 定义第 55 行](../src/C++/detail/FastScan.cpp#L55)

### 5.4 第二步：查找 `SOH9=` 与 BodyLength 结束位置

[`tryReadFixMessageFast()` 第 199 行](../src/C++/Parser.cpp#L199) 调用
`findSohPatternFast(buffer, beginPos, "9=", 2)`<br>
-> [`findSohPatternFast()` 定义第 82 行](../src/C++/Parser.cpp#L82)

传入参数：

- `buffer`：当前 Parser stream。
- `beginPos`：刚找到的 `8=` 起点。
- `"9="`：SOH 后必须紧跟的两字节模式。
- `2`：模式长度。

[`findSohPatternFast()` 第 92 行](../src/C++/Parser.cpp#L92) 调用
`detail::findCharFast(current, end, SOH)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

这一层 SIMD 并行找的是 16 个字节中哪些等于 SOH。找到候选 SOH 后，
[`findSohPatternFast()` 第 97 行](../src/C++/Parser.cpp#L97) 用 `memcmp()` 检查其后是否为 `"9="`。

找到 tag 9 后：

[`tryReadFixMessageFast()` 第 205 行](../src/C++/Parser.cpp#L205) 调用
`findSohFast(buffer, lengthBegin)`<br>
-> [`findSohFast()` 定义第 113 行](../src/C++/Parser.cpp#L113)

[`findSohFast()` 第 116 行](../src/C++/Parser.cpp#L116) 调用
`detail::findCharFast(data + start, end, SOH)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

这里找到的是 `9=BodyLength` 字段末尾的 SOH。

[`tryReadFixMessageFast()` 第 211 行](../src/C++/Parser.cpp#L211) 调用
`parseLengthFast(buffer, lengthBegin, lengthEnd, length)`<br>
-> [`parseLengthFast()` 定义第 158 行](../src/C++/Parser.cpp#L158)

它不创建临时字符串，直接逐位转为整数，并拒绝空值、非数字和 `int` 溢出。

### 5.5 第三步：利用 BodyLength 跳到 checksum 附近

[`tryReadFixMessageFast()` 第 215 行](../src/C++/Parser.cpp#L215) 计算：

```text
bodyBegin = lengthEnd + 1
checksumSearchStart = bodyBegin + BodyLength
```

BodyLength 表示从 tag 9 后面的 SOH 之后开始，到 checksum 字段之前为止的 body 字节数。因此不必从消息
开头盲扫到结尾，而是直接把 checksum 搜索起点放在协议规定的位置附近。

[`tryReadFixMessageFast()` 第 226 行](../src/C++/Parser.cpp#L226) 调用
`findChecksumPatternFast(buffer, checksumSearchStart - 1)`<br>
-> [`findChecksumPatternFast()` 定义第 133 行](../src/C++/Parser.cpp#L133)

传入的 `checksumSearchStart - 1` 指向理论上位于 `10=` 前面的 SOH。

### 5.6 第四步：四字节 SIMD 查找 `SOH10=`

打开 `QUICKFIX_SIMD_PATTERN_SCAN` 时：

[`findChecksumPatternFast()` 第 137 行](../src/C++/Parser.cpp#L137) 调用
`detail::findSoh10Fast(data + start, end)`<br>
-> [`findSoh10Fast()` 定义第 210 行](../src/C++/detail/FastScan.cpp#L210)

[`findSoh10Fast()` 第 212 行](../src/C++/detail/FastScan.cpp#L212) 调用
`findSoh10Simd(begin, end)`<br>
-> [`findSoh10Simd()` 定义第 173 行](../src/C++/detail/FastScan.cpp#L173)

`findSoh10Simd()` 并行检查的是 16 个“可能的模式起点”：

```text
current + 0   加载 16 个候选位置的第 1 字节，比较 SOH
current + 1   加载同一批候选位置的第 2 字节，比较 '1'
current + 2   加载同一批候选位置的第 3 字节，比较 '0'
current + 3   加载同一批候选位置的第 4 字节，比较 '='
```

四次加载位于 [`第 182 行`](../src/C++/detail/FastScan.cpp#L182) 到
[`第 185 行`](../src/C++/detail/FastScan.cpp#L185)。

四个比较结果生成四个 16 位 mask，并在
[`第 187 行`](../src/C++/detail/FastScan.cpp#L187) 到
[`第 190 行`](../src/C++/detail/FastScan.cpp#L190) 做按位 AND：

```text
SOH mask & '1' mask & '0' mask & '=' mask
```

某一位只有在对应起点的四个连续字节全部为 `SOH10=` 时才保留下来。

mask 非零后：

[`findSoh10Simd()` 第 192 行](../src/C++/detail/FastScan.cpp#L192) 调用
`firstSetBit(mask)`<br>
-> [`firstSetBit()` 定义第 77 行](../src/C++/detail/FastScan.cpp#L77)

这样返回 16 个候选起点中最早的完整 `SOH10=`。

不足以安全完成四次 16 字节加载的尾部：

[`findSoh10Simd()` 第 197 行](../src/C++/detail/FastScan.cpp#L197) 调用
`findSoh10Scalar(current, end)`<br>
-> [`findSoh10Scalar()` 定义第 150 行](../src/C++/detail/FastScan.cpp#L150)

如果没有打开 `SIMD_PATTERN_SCAN`：

[`findChecksumPatternFast()` 第 143 行](../src/C++/Parser.cpp#L143) 调用
`findSohPatternFast(buffer, start, "10=", 3)`<br>
-> [`findSohPatternFast()` 定义第 82 行](../src/C++/Parser.cpp#L82)

此时仍可用 SIMD 找候选 SOH，但 `"10="` 本身由 `memcmp()` 逐候选确认，不使用四字节 pattern SIMD。

### 5.7 第五步：找到 checksum 结尾并切出一整条消息

[`tryReadFixMessageFast()` 第 232 行](../src/C++/Parser.cpp#L232) 调用
`findSohFast(buffer, checksumBegin)`<br>
-> [`findSohFast()` 定义第 113 行](../src/C++/Parser.cpp#L113)

这里查找 `10=xxx` 后面的最终 SOH。

成功后：

- [`tryReadFixMessageFast()` 第 238 行](../src/C++/Parser.cpp#L238) 将
  `[beginPos, messageEnd)` 复制到输出 `str`。
- [`第 239 行`](../src/C++/Parser.cpp#L239) 从持久 stream buffer 删除已经消费的所有字节。
- buffer 中如果还有下一条 FIX 消息，会留给下一次 `readFixMessage()`。

### 5.8 Fast path 失败如何回退

[`Parser::readFixMessage()` 第 297 行](../src/C++/Parser.cpp#L297) 只有在 fast path 返回 `true` 时才直接
返回。fast path 返回 `false` 后，执行流自然落到 [`第 302 行`](../src/C++/Parser.cpp#L302) 的原始实现。

原始 fallback 的关键步骤是：

- [`第 307 行`](../src/C++/Parser.cpp#L307)：`m_buffer.find("8=")`。
- [`第 316 行`](../src/C++/Parser.cpp#L316) 调用 `extractLength(...)`<br>
  -> [`Parser::extractLength()` 定义第 254 行](../src/C++/Parser.cpp#L254)。
- [`第 322 行`](../src/C++/Parser.cpp#L322)：`m_buffer.find(SOH "10=", pos - 1)`。
- [`第 327 行`](../src/C++/Parser.cpp#L327)：查找 checksum 字段末尾 SOH。
- [`第 333 行`](../src/C++/Parser.cpp#L333) 到 [`第 334 行`](../src/C++/Parser.cpp#L334)：
  输出消息并删除已消费字节。

fast path 在失败时不修改 `str` 和 `m_buffer`，所以 fallback 看到的仍是原始完整输入。这里没有运行时命令行
开关；是否编译 fast path 由 CMake 选项决定。

### 5.9 为什么可能比普通 `find()` 快

标量扫描通常逐字节判断目标字符；`findCharSimd()` 每轮用一条 128 位加载处理 16 字节，再通过 compare、
mask 和 first-set-bit 找到最早匹配位置。对于较长且大部分字节都不是目标字符的区间，可以减少循环次数和
分支判断。

`findSoh10Simd()` 更进一步：它不是“先找 SOH，再逐个检查 `10=`”，而是一次并行验证 16 个候选起点上的
四个字符。

但 SIMD 不保证在所有消息上更快：

- 消息很短或目标字符就在开头时，向量准备和 mask 提取成本可能抵消收益。
- 当前标准库的 `std::string::find()` 本身可能已经高度优化。
- SOH 非常密集时，先找候选再验证会产生更多短循环。

因此正确结论应来自 benchmark，而不是仅凭使用了 SIMD。你当前的实验拆分也保留了这种比较能力：
`SIMD_STREAM_PARSER`、`SIMD_PATTERN_SCAN` 和 `SIMD_FIELD_SCAN` 可以独立构建。

### 5.10 正确性入口

- [`runFastScanSelfTest()` 第 937 行](../src/fix_parse_benchmark.cpp#L937) 对比 scalar 与 SIMD 扫描结果。
- [`runParserSelfTest()` 第 1033 行](../src/fix_parse_benchmark.cpp#L1033) 检查完整、粘包、拆包和不完整
  stream 的消息边界。
- [`runCorrectnessSelfTest()` 第 1715 行](../src/fix_parse_benchmark.cpp#L1715) 沿真实
  `Parser -> Message` 过程比较消息与字段。

对应命令：

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
./test/run_parse_benchmark.sh --self-test-parser
./test/run_parse_benchmark.sh --self-test-correctness --messages=1000
```

### 5.11 SIMD 严格调用链总表

下面是一条不需要在前面各小节之间来回拼接的连续调用链。每个 `->` 左边是代码中真实存在的调用位置，
右边是被调用函数的定义位置。

#### A. 从 socket 接收链进入 Parser

[`SocketConnection::readMessages()` 第 418 行](../src/C++/SocketConnection.cpp#L418) 调用
`readMessage(message)`<br>
-> [`SocketConnection::readMessage()` 定义第 401 行](../src/C++/SocketConnection.cpp#L401)

[`SocketConnection::readMessage()` 第 403 行](../src/C++/SocketConnection.cpp#L403) 调用
`m_parser.readFixMessage(msg)`<br>
-> [`Parser::readFixMessage()` 定义第 295 行](../src/C++/Parser.cpp#L295)

[`Parser::readFixMessage()` 第 297 行](../src/C++/Parser.cpp#L297) 调用
`tryReadFixMessageFast(str, m_buffer)`<br>
-> [`tryReadFixMessageFast()` 定义第 193 行](../src/C++/Parser.cpp#L193)

#### B. SIMD 查找 `8=` 消息起点

[`tryReadFixMessageFast()` 第 194 行](../src/C++/Parser.cpp#L194) 调用
`findBeginMarkerFast(buffer)`<br>
-> [`findBeginMarkerFast()` 定义第 47 行](../src/C++/Parser.cpp#L47)

[`findBeginMarkerFast()` 第 57 行](../src/C++/Parser.cpp#L57) 调用
`detail::findCharFast(current, lastCandidate, '8')`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

打开 `QUICKFIX_SIMD_STREAM_PARSER` 时：

[`findCharFast()` 第 137 行](../src/C++/detail/FastScan.cpp#L137) 调用
`findCharSimd(begin, end, target)`<br>
-> [`findCharSimd()` 定义第 103 行](../src/C++/detail/FastScan.cpp#L103)

16 字节块中存在匹配时：

[`findCharSimd()` 第 113 行](../src/C++/detail/FastScan.cpp#L113) 调用
`firstSetBit(mask)`<br>
-> [`firstSetBit()` 定义第 77 行](../src/C++/detail/FastScan.cpp#L77)

不足 16 字节的尾部：

[`findCharSimd()` 第 118 行](../src/C++/detail/FastScan.cpp#L118) 调用
`findCharScalar(current, end, target)`<br>
-> [`findCharScalar()` 定义第 55 行](../src/C++/detail/FastScan.cpp#L55)

没有可用 SSE2 时：

[`findCharSimd()` 第 120 行](../src/C++/detail/FastScan.cpp#L120) 调用
`findCharScalar(begin, end, target)`<br>
-> [`findCharScalar()` 定义第 55 行](../src/C++/detail/FastScan.cpp#L55)

找到候选 `'8'` 后，
[`findBeginMarkerFast()` 第 61 行](../src/C++/Parser.cpp#L61) 直接检查 `candidate[1] == '='`。这是本地字符
判断，不是另一次函数调用。

#### C. SIMD 查找 `SOH9=`

[`tryReadFixMessageFast()` 第 199 行](../src/C++/Parser.cpp#L199) 调用
`findSohPatternFast(buffer, beginPos, "9=", 2)`<br>
-> [`findSohPatternFast()` 定义第 82 行](../src/C++/Parser.cpp#L82)

[`findSohPatternFast()` 第 92 行](../src/C++/Parser.cpp#L92) 调用
`detail::findCharFast(current, end, SOH)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

[`findCharFast()` 第 137 行](../src/C++/detail/FastScan.cpp#L137) 调用
`findCharSimd(begin, end, target)`<br>
-> [`findCharSimd()` 定义第 103 行](../src/C++/detail/FastScan.cpp#L103)

找到候选 SOH 后，
[`findSohPatternFast()` 第 97 行](../src/C++/Parser.cpp#L97) 调用标准库 `memcmp()` 检查后续两字节是否为
`"9="`。项目内没有对应的本地函数定义可继续跳转。

#### D. SIMD 查找 BodyLength 字段末尾 SOH

[`tryReadFixMessageFast()` 第 205 行](../src/C++/Parser.cpp#L205) 调用
`findSohFast(buffer, lengthBegin)`<br>
-> [`findSohFast()` 定义第 113 行](../src/C++/Parser.cpp#L113)

[`findSohFast()` 第 116 行](../src/C++/Parser.cpp#L116) 调用
`detail::findCharFast(data + start, end, SOH)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

[`findCharFast()` 第 137 行](../src/C++/detail/FastScan.cpp#L137) 调用
`findCharSimd(begin, end, target)`<br>
-> [`findCharSimd()` 定义第 103 行](../src/C++/detail/FastScan.cpp#L103)

#### E. 解析 BodyLength 并计算 checksum 搜索位置

[`tryReadFixMessageFast()` 第 211 行](../src/C++/Parser.cpp#L211) 调用
`parseLengthFast(buffer, lengthBegin, lengthEnd, length)`<br>
-> [`parseLengthFast()` 定义第 158 行](../src/C++/Parser.cpp#L158)

`parseLengthFast()` 返回后，
[`tryReadFixMessageFast()` 第 215 行](../src/C++/Parser.cpp#L215) 到
[`第 221 行`](../src/C++/Parser.cpp#L221) 直接计算：

```text
bodyBegin = lengthEnd + 1
checksumSearchStart = bodyBegin + BodyLength
```

这是整数和 offset 运算，没有新的函数调用。

#### F. 四字节 SIMD 查找 `SOH10=`

[`tryReadFixMessageFast()` 第 226 行](../src/C++/Parser.cpp#L226) 调用
`findChecksumPatternFast(buffer, checksumSearchStart - 1)`<br>
-> [`findChecksumPatternFast()` 定义第 133 行](../src/C++/Parser.cpp#L133)

打开 `QUICKFIX_SIMD_PATTERN_SCAN` 时：

[`findChecksumPatternFast()` 第 137 行](../src/C++/Parser.cpp#L137) 调用
`detail::findSoh10Fast(data + start, end)`<br>
-> [`findSoh10Fast()` 定义第 210 行](../src/C++/detail/FastScan.cpp#L210)

[`findSoh10Fast()` 第 212 行](../src/C++/detail/FastScan.cpp#L212) 调用
`findSoh10Simd(begin, end)`<br>
-> [`findSoh10Simd()` 定义第 173 行](../src/C++/detail/FastScan.cpp#L173)

`findSoh10Simd()` 在
[`第 182 行`](../src/C++/detail/FastScan.cpp#L182) 到
[`第 185 行`](../src/C++/detail/FastScan.cpp#L185) 分别加载 `current + 0/1/2/3` 的 16 字节，并在
[`第 187 行`](../src/C++/detail/FastScan.cpp#L187) 到
[`第 190 行`](../src/C++/detail/FastScan.cpp#L190) 对四个比较 mask 做 AND。

存在完整模式时：

[`findSoh10Simd()` 第 192 行](../src/C++/detail/FastScan.cpp#L192) 调用
`firstSetBit(mask)`<br>
-> [`firstSetBit()` 定义第 77 行](../src/C++/detail/FastScan.cpp#L77)

不足以完成四次 16 字节加载的尾部：

[`findSoh10Simd()` 第 197 行](../src/C++/detail/FastScan.cpp#L197) 调用
`findSoh10Scalar(current, end)`<br>
-> [`findSoh10Scalar()` 定义第 150 行](../src/C++/detail/FastScan.cpp#L150)

没有可用 SSE2 时：

[`findSoh10Simd()` 第 199 行](../src/C++/detail/FastScan.cpp#L199) 调用
`findSoh10Scalar(begin, end)`<br>
-> [`findSoh10Scalar()` 定义第 150 行](../src/C++/detail/FastScan.cpp#L150)

#### G. 没有打开 pattern SIMD 时的 checksum 分支

如果 `QUICKFIX_SIMD_PATTERN_SCAN` 未打开：

[`findChecksumPatternFast()` 第 143 行](../src/C++/Parser.cpp#L143) 调用
`findSohPatternFast(buffer, start, "10=", 3)`<br>
-> [`findSohPatternFast()` 定义第 82 行](../src/C++/Parser.cpp#L82)

[`findSohPatternFast()` 第 92 行](../src/C++/Parser.cpp#L92) 调用
`detail::findCharFast(current, end, SOH)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

这条分支仍可使用 SIMD 找 SOH，但 `"10="` 由
[`findSohPatternFast()` 第 97 行](../src/C++/Parser.cpp#L97) 的 `memcmp()` 确认。

#### H. SIMD 查找 checksum 字段末尾 SOH

[`tryReadFixMessageFast()` 第 232 行](../src/C++/Parser.cpp#L232) 调用
`findSohFast(buffer, checksumBegin)`<br>
-> [`findSohFast()` 定义第 113 行](../src/C++/Parser.cpp#L113)

[`findSohFast()` 第 116 行](../src/C++/Parser.cpp#L116) 调用
`detail::findCharFast(data + start, end, SOH)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

[`findCharFast()` 第 137 行](../src/C++/detail/FastScan.cpp#L137) 调用
`findCharSimd(begin, end, target)`<br>
-> [`findCharSimd()` 定义第 103 行](../src/C++/detail/FastScan.cpp#L103)

#### I. 成功切出完整消息

以下是成功分支中的直接数据操作，不是函数调用箭头：

- [`tryReadFixMessageFast()` 第 237 行](../src/C++/Parser.cpp#L237) 计算 `messageEnd`。
- [`第 238 行`](../src/C++/Parser.cpp#L238) 把 `[beginPos, messageEnd)` 复制到 `str`。
- [`第 239 行`](../src/C++/Parser.cpp#L239) 从 `buffer` 删除 `[0, messageEnd)`。
- [`第 240 行`](../src/C++/Parser.cpp#L240) 返回 `true`。

#### J. Fast path 失败后的原始 fallback

`tryReadFixMessageFast()` 返回 `false` 后，没有另一次函数调用；控制流从
[`Parser::readFixMessage()` 第 302 行](../src/C++/Parser.cpp#L302) 继续执行原始实现。

原始实现中唯一需要继续跳转的项目内函数调用是：

[`Parser::readFixMessage()` 第 316 行](../src/C++/Parser.cpp#L316) 调用
`extractLength(length, pos, m_buffer)`<br>
-> [`Parser::extractLength()` 定义第 254 行](../src/C++/Parser.cpp#L254)

其他 fallback 操作是标准字符串查找：

- [`第 307 行`](../src/C++/Parser.cpp#L307)：`m_buffer.find("8=")`。
- [`第 322 行`](../src/C++/Parser.cpp#L322)：`m_buffer.find(SOH "10=", pos - 1)`。
- [`第 327 行`](../src/C++/Parser.cpp#L327)：查找 checksum 末尾 SOH。
- [`第 333 行`](../src/C++/Parser.cpp#L333) 和 [`第 334 行`](../src/C++/Parser.cpp#L334)：
  输出并删除已消费消息。

#### K. 独立的 `SIMD_FIELD_SCAN` 支路

这条支路不属于完整消息 framing，只用于字段 value 末尾 SOH 的查找：

[`Message.cpp` 第 48 行](../src/C++/Message.cpp#L48) 调用
`detail::findCharFast(begin, end, SOH)`<br>
-> [`findCharFast()` 定义第 135 行](../src/C++/detail/FastScan.cpp#L135)

[`findCharFast()` 第 137 行](../src/C++/detail/FastScan.cpp#L137) 调用
`findCharSimd(begin, end, target)`<br>
-> [`findCharSimd()` 定义第 103 行](../src/C++/detail/FastScan.cpp#L103)

关闭 `QUICKFIX_SIMD_FIELD_SCAN` 时，
[`Message.cpp` 第 51 行](../src/C++/Message.cpp#L51) 直接使用 `std::find()`，不会进入 `FastScan.cpp`。

## 6. Fixed-layout 固定偏移解析

### 6.1 它位于总链路的哪一层

SIMD stream parser 和 fixed-layout 处理的是两个前后相邻但彼此独立的阶段：

```text
TCP stream
  -> Parser::readFixMessage()
     切出一条完整 FIX wire message，可使用 SIMD
  -> Message::setString()
     把完整 wire message 解析成 Header、Body、Trailer 和 Group，可尝试 fixed-layout
  -> Session
  -> Application
```

因此 fixed-layout 不负责从 TCP buffer 中判断消息边界。无论 blocking、poll0 还是 direct，只要完整消息最终
进入 `Message::setString()`，都可以尝试 fixed-layout。

编译期开关位于：

- [`QUICKFIX_FIXED_LAYOUT_PARSER` 第 66 行](../CMakeLists.txt#L66)
- 向 QuickFIX 库添加宏的位置：
  [`src/C++/CMakeLists.txt` 第 111 行](../src/C++/CMakeLists.txt#L111)

benchmark 的消息形态开关在
[`parseOptions()` 第 2240 行](../src/fix_parse_benchmark.cpp#L2240) 写入 `options.fixedLayout`。

在 benchmark server 模式中，fixed-layout 被限制为 raw client：

[`parseOptions()` 第 2312 行](../src/fix_parse_benchmark.cpp#L2312) 到
[`第 2314 行`](../src/fix_parse_benchmark.cpp#L2314)

这是 benchmark 构造方式的限制。QuickFIX typed client 会按 QuickFIX 自己的字段顺序和宽度重新序列化，
不会自然生成当前三个 byte-for-byte 固定模板。

### 6.2 模板数据结构

顶层字段模板的定义是：

[`FixedFieldSpec` 第 70 行](../src/C++/Message.cpp#L70)

```text
tag
  -> 要构造的数字 FIX tag

fieldOffset
  -> tag 第一个字符在整条消息中的绝对 offset

valueOffset
  -> value 第一个字符在整条消息中的绝对 offset

valueLength
  -> value 的固定字节数

target
  -> 字段放入 Header、Body 或 Trailer
```

例如 NOS1 中的：

[`ClOrdID 模板项第 102 行`](../src/C++/Message.cpp#L102)

```cpp
{FIELD::ClOrdID, 88, 91, 18, FixedFieldTarget::Body}
```

表示 tag 11 从整条消息 offset 88 开始，value 从 offset 91 开始、长度固定为 18 字节，构造后放入 Body。

行情 repeating group 使用：

[`FixedGroupSpec` 第 81 行](../src/C++/Message.cpp#L81)

它同样保存 `tag`、`fieldOffset`、`valueOffset` 和 `valueLength`，但 group 归属由外层
`FIXED_MDW1_GROUPS` 决定。

### 6.3 三套固定模板

| 模板 | 消息类型 | 总长度 | marker | 字段表 |
|---|---|---:|---|---|
| NOS1 | NewOrderSingle，`35=D` | 187 | `9001=NOS1<SOH>` | [`FIXED_NOS1_FIELDS` 第 93 行](../src/C++/Message.cpp#L93) |
| CXL1 | OrderCancelRequest，`35=F` | 185 | `9001=CXL1<SOH>` | [`FIXED_CXL1_FIELDS` 第 119 行](../src/C++/Message.cpp#L119) |
| MDW1 | MarketDataSnapshotFullRefresh，`35=W` | 288 | `9001=MDW1<SOH>` | [`FIXED_MDW1_FIELDS` 第 143 行](../src/C++/Message.cpp#L143) |

MDW1 的三个行情 group 单独定义在：

[`FIXED_MDW1_GROUPS` 第 160 行](../src/C++/Message.cpp#L160)

每个 group 固定包含：

```text
269 MDEntryType
270 MDEntryPx
271 MDEntrySize
273 MDEntryTime
```

marker 的固定位置和内容是：

- [`FIXED_LAYOUT_MARKER_FIELD_OFFSET 第 182 行`](../src/C++/Message.cpp#L182)：offset `78`。
- [`FIXED_LAYOUT_MARKER_FIELD_SIZE 第 184 行`](../src/C++/Message.cpp#L184)：长度 `10`，包括末尾 SOH。
- [`FIXED_NOS1_MARKER 第 187 行`](../src/C++/Message.cpp#L187)。
- [`FIXED_CXL1_MARKER 第 189 行`](../src/C++/Message.cpp#L189)。
- [`FIXED_MDW1_MARKER 第 191 行`](../src/C++/Message.cpp#L191)。

### 6.4 Benchmark 如何构造 fixed-layout 消息

#### NOS1 下单

[`makeApplicationMessage()` 第 462 行](../src/fix_parse_benchmark.cpp#L462) 调用
`fixedNewOrderSingleFields(seqNum, now, options)`<br>
-> [`fixedNewOrderSingleFields()` 定义第 296 行](../src/fix_parse_benchmark.cpp#L296)

[`fixedNewOrderSingleFields()` 第 301 行](../src/fix_parse_benchmark.cpp#L301) 调用
`fixedHeaderFields("D", seqNum, now)`<br>
-> [`fixedHeaderFields()` 定义第 250 行](../src/fix_parse_benchmark.cpp#L250)

[`fixedNewOrderSingleFields()` 第 302 行](../src/fix_parse_benchmark.cpp#L302) 添加
`9001=NOS1`，后续字段使用固定顺序和固定 value 宽度。

字段列表返回后，外层：

[`makeApplicationMessage()` 第 461 行](../src/fix_parse_benchmark.cpp#L461) 调用
`buildFixMessage(...)`<br>
-> [`buildFixMessage()` 定义第 202 行](../src/fix_parse_benchmark.cpp#L202)

#### CXL1 撤单

[`makeApplicationMessage()` 第 466 行](../src/fix_parse_benchmark.cpp#L466) 调用
`fixedOrderCancelRequestFields(seqNum, now, options)`<br>
-> [`fixedOrderCancelRequestFields()` 定义第 347 行](../src/fix_parse_benchmark.cpp#L347)

[`fixedOrderCancelRequestFields()` 第 353 行](../src/fix_parse_benchmark.cpp#L353) 调用
`fixedHeaderFields("F", seqNum, now)`<br>
-> [`fixedHeaderFields()` 定义第 250 行](../src/fix_parse_benchmark.cpp#L250)

[`fixedOrderCancelRequestFields()` 第 354 行](../src/fix_parse_benchmark.cpp#L354) 添加
`9001=CXL1`。

字段列表返回后：

[`makeApplicationMessage()` 第 465 行](../src/fix_parse_benchmark.cpp#L465) 调用
`buildFixMessage(...)`<br>
-> [`buildFixMessage()` 定义第 202 行](../src/fix_parse_benchmark.cpp#L202)

#### MDW1 行情

[`makeApplicationMessage()` 第 470 行](../src/fix_parse_benchmark.cpp#L470) 调用
`fixedMarketDataSnapshotFields(seqNum, now, options)`<br>
-> [`fixedMarketDataSnapshotFields()` 定义第 412 行](../src/fix_parse_benchmark.cpp#L412)

[`fixedMarketDataSnapshotFields()` 第 414 行](../src/fix_parse_benchmark.cpp#L414) 调用
`fixedHeaderFields("W", seqNum, now)`<br>
-> [`fixedHeaderFields()` 定义第 250 行](../src/fix_parse_benchmark.cpp#L250)

[`fixedMarketDataSnapshotFields()` 第 415 行](../src/fix_parse_benchmark.cpp#L415) 添加
`9001=MDW1`。

[`fixedMarketDataSnapshotFields()` 第 419 行](../src/fix_parse_benchmark.cpp#L419) 调用
`appendMarketDataEntries(fields, options.fixedLayout)`<br>
-> [`appendMarketDataEntries()` 定义第 370 行](../src/fix_parse_benchmark.cpp#L370)

字段列表返回后：

[`makeApplicationMessage()` 第 469 行](../src/fix_parse_benchmark.cpp#L469) 调用
`buildFixMessage(...)`<br>
-> [`buildFixMessage()` 定义第 202 行](../src/fix_parse_benchmark.cpp#L202)

### 6.5 完整消息如何进入 `Message::setString()`

#### `mode=parse`

[`runParseBenchmark()` 第 1892 行](../src/fix_parse_benchmark.cpp#L1892) 调用
`message.setString(sample, options.validate, dictionary)`<br>
-> [`Message::setString()` 定义第 564 行](../src/C++/Message.cpp#L564)

#### `mode=server`

server 首先经过第 3.6 节的共同网络链路和 `Parser::readFixMessage()`，得到一条完整 wire message。

[`SocketConnection::readMessages()` 第 425 行](../src/C++/SocketConnection.cpp#L425) 调用
`m_pSession->next(message, now)`<br>
-> [`Session::next(const std::string&, ...)` 定义第 1170 行](../src/C++/Session.cpp#L1170)

FIX 4.2 benchmark 走：

[`Session::next(string)` 第 1179 行](../src/C++/Session.cpp#L1179) 构造
`Message(msg, sessionDD, m_validateLengthAndChecksum)`<br>
-> [`Message` 字典构造函数定义第 225 行](../src/C++/Message.cpp#L225)

[`Message` 构造函数第 228 行](../src/C++/Message.cpp#L228) 调用
`setString(string, validate, &dataDictionary, &dataDictionary)`<br>
-> [`Message::setString()` 定义第 564 行](../src/C++/Message.cpp#L564)

FIXT 双字典路径对应：

[`Session::next(string)` 第 1177 行](../src/C++/Session.cpp#L1177) 构造双字典 `Message`<br>
-> [`Message` 双字典构造函数定义第 231 行](../src/C++/Message.cpp#L231)

[`Message` 双字典构造函数第 238 行](../src/C++/Message.cpp#L238) 调用 `setString(...)`<br>
-> [`Message::setString()` 定义第 564 行](../src/C++/Message.cpp#L564)

### 6.6 严格匹配链：只判断总长度和 marker

[`Message::setString()` 第 572 行](../src/C++/Message.cpp#L572) 调用
`setFixedLayoutString(string)`<br>
-> [`Message::setFixedLayoutString()` 定义第 482 行](../src/C++/Message.cpp#L482)

NOS1 判断：

[`setFixedLayoutString()` 第 487 行](../src/C++/Message.cpp#L487) 在长度等于 `187` 后调用
`hasFixedLayoutMarker(string, FIXED_NOS1_MARKER)`<br>
-> [`hasFixedLayoutMarker()` 定义第 200 行](../src/C++/Message.cpp#L200)

CXL1 判断：

[`setFixedLayoutString()` 第 490 行](../src/C++/Message.cpp#L490) 在长度等于 `185` 后调用
`hasFixedLayoutMarker(string, FIXED_CXL1_MARKER)`<br>
-> [`hasFixedLayoutMarker()` 定义第 200 行](../src/C++/Message.cpp#L200)

MDW1 判断：

[`setFixedLayoutString()` 第 493 行](../src/C++/Message.cpp#L493) 在长度等于 `288` 后调用
`hasFixedLayoutMarker(string, FIXED_MDW1_MARKER)`<br>
-> [`hasFixedLayoutMarker()` 定义第 200 行](../src/C++/Message.cpp#L200)

[`hasFixedLayoutMarker()` 第 201 行](../src/C++/Message.cpp#L201) 调用
`string.compare(offset=78, count=10, marker, count=10)`。这是标准库调用，项目中没有本地定义继续跳转。

资格判断到此结束。这里不会额外检查：

- 其他 tag 是否真的位于模板声称的位置。
- 每个字段后面是否存在正确的 `=` 或 SOH。
- value 类型和内容是否合法。
- tag 9 BodyLength 是否正确。
- tag 10 CheckSum 是否正确。

三个长度和 marker 组合都不匹配时，
[`setFixedLayoutString()` 第 498 行](../src/C++/Message.cpp#L498) 直接返回 `false`，此时尚未向
`Message` 添加任何模板字段。

### 6.7 命中后按 offset 构造普通字段

选择模板后：

[`setFixedLayoutString()` 第 520 行](../src/C++/Message.cpp#L520) 调用局部
`makeField(field.tag, field.fieldOffset, field.valueOffset, field.valueLength)`<br>
-> [`makeField` lambda 定义第 510 行](../src/C++/Message.cpp#L510)

`makeField` 的四个参数是：

```text
tag
  -> 模板中预先写死的数字 tag

fieldOffset
  -> 用于计算该字段 metrics 的 tag 起始位置

valueOffset
  -> value 起始位置

valueLength
  -> 从 valueOffset 开始复制的固定字节数
```

[`makeField` 第 515 行](../src/C++/Message.cpp#L515) 调用
`FieldBase(tag, valueStart, valueEnd, tagStart, valueEnd + 1)`<br>
-> [`FieldBase` iterator 构造函数定义第 69 行](../include/quickfix/Field.h#L69)

`FieldBase` 构造函数从 iterator 范围复制 value，并根据 tag 到 SOH 的范围计算字段 metrics。它不再扫描
tag、`=` 或 SOH 的位置。

根据模板中的 `target`：

[`setFixedLayoutString()` 第 522 行](../src/C++/Message.cpp#L522) 调用
`m_header.appendField(fieldBase)`<br>
-> [`FieldMap::appendField()` 定义第 282 行](../include/quickfix/FieldMap.h#L282)

[`setFixedLayoutString()` 第 524 行](../src/C++/Message.cpp#L524) 调用
`m_trailer.appendField(fieldBase)`<br>
-> [`FieldMap::appendField()` 定义第 282 行](../include/quickfix/FieldMap.h#L282)

[`setFixedLayoutString()` 第 526 行](../src/C++/Message.cpp#L526) 调用 body 的
`appendField(fieldBase)`<br>
-> [`FieldMap::appendField()` 定义第 282 行](../include/quickfix/FieldMap.h#L282)

### 6.8 MDW1 repeating groups 调用链

只有 MDW1 在 [`setFixedLayoutString()` 第 496 行](../src/C++/Message.cpp#L496) 把
`hasMarketDataGroups` 设为 `true`。

随后 [`第 531 行`](../src/C++/Message.cpp#L531) 遍历 `FIXED_MDW1_GROUPS`。

每个 group 字段：

[`setFixedLayoutString()` 第 539 行](../src/C++/Message.cpp#L539) 调用
`makeField(...)`<br>
-> [`makeField` lambda 定义第 510 行](../src/C++/Message.cpp#L510)

[`setFixedLayoutString()` 第 539 行](../src/C++/Message.cpp#L539) 随后调用
`group.appendField(...)`<br>
-> [`FieldMap::appendField()` 定义第 282 行](../include/quickfix/FieldMap.h#L282)

一个 group 的四个字段构造完后：

[`setFixedLayoutString()` 第 541 行](../src/C++/Message.cpp#L541) 调用
`addGroup(group)`<br>
-> [`Message::addGroup()` 定义第 163 行](../src/C++/Message.h#L163)

[`Message::addGroup()` 第 163 行](../src/C++/Message.h#L163) 调用
`FieldMap::addGroup(group.field(), group)`<br>
-> [`FieldMap::addGroup()` 定义第 79 行](../src/C++/FieldMap.cpp#L79)

[`FieldMap::addGroup()` 第 82 行](../src/C++/FieldMap.cpp#L82) 调用
`addGroupPtr(field, pGroup, setCount)`<br>
-> [`FieldMap::addGroupPtr()` 定义第 85 行](../src/C++/FieldMap.cpp#L85)

[`FieldMap::addGroupPtr()` 第 94 行](../src/C++/FieldMap.cpp#L94) 更新 group count，因此 MDW1 顶层的
`NoMDEntries(268)` 会随着三个 group 被添加而得到值 `3`。

### 6.9 成功返回与普通解析 fallback

模板字段构造完后：

[`setFixedLayoutString()` 第 545 行](../src/C++/Message.cpp#L545) 到
[`第 547 行`](../src/C++/Message.cpp#L547) 调用各 FieldMap 的 `sortFields()`<br>
-> [`FieldMap::sortFields()` 定义第 285 行](../include/quickfix/FieldMap.h#L285)

[`setFixedLayoutString()` 第 548 行](../src/C++/Message.cpp#L548) 返回 `true`。

返回 `Message::setString()` 后：

[`Message::setString()` 第 572 行](../src/C++/Message.cpp#L572) 得到 `true`，并在
[`第 573 行`](../src/C++/Message.cpp#L573) 立即返回。

因此命中模板时会跳过：

- 普通逐字段 `extractField()`。
- 普通 repeating-group 字典解析。
- `Message::setString()` 末尾的 `validate()`。

需要注意，这里指的是跳过 `Message::setString()` 内的通用解析与校验；上层 Session 是否执行其他会话或
字典检查，仍由 Session 配置和后续流程决定。

如果长度或 marker 不匹配，`setFixedLayoutString()` 返回 `false`，执行流继续到原始路径：

[`Message::setString()` 第 585 行](../src/C++/Message.cpp#L585) 调用
`extractField(...)`<br>
-> [`Message::extractField()` 定义第 835 行](../src/C++/Message.cpp#L835)

遇到普通字典 repeating group 时：

[`Message::setString()` 第 610 行](../src/C++/Message.cpp#L610)、
[`第 617 行`](../src/C++/Message.cpp#L617) 或
[`第 631 行`](../src/C++/Message.cpp#L631) 调用 `setGroup(...)`<br>
-> [`Message::setGroup()` 定义第 646 行](../src/C++/Message.cpp#L646)

如果 `doValidation=true`：

[`Message::setString()` 第 642 行](../src/C++/Message.cpp#L642) 调用 `validate()`<br>
-> [`Message::validate()` 定义第 803 行](../src/C++/Message.cpp#L803)

这就实现了预期行为：

```text
长度匹配 + marker 匹配
  -> 直接套 offset 模板

长度不匹配或 marker 不匹配
  -> 返回 false
  -> 同一次 Message::setString() 继续普通解析
```

### 6.10 Fixed-layout 严格调用链总表

下面把最主要的运行链压缩成一条连续路线。每个 `->` 都是实际调用位置到函数定义位置。

#### A. Benchmark 构造固定消息

```text
makeApplicationMessage()
  -> fixedNewOrderSingleFields()
     或 fixedOrderCancelRequestFields()
     或 fixedMarketDataSnapshotFields()
  -> buildFixMessage()
  -> 完整 FIX wire string
```

对应调用位置：

- NOS1：
  [`调用第 462 行`](../src/fix_parse_benchmark.cpp#L462)
  -> [`定义第 296 行`](../src/fix_parse_benchmark.cpp#L296)
  -> [`buildFixMessage() 调用第 461 行`](../src/fix_parse_benchmark.cpp#L461)
  -> [`定义第 202 行`](../src/fix_parse_benchmark.cpp#L202)
- CXL1：
  [`调用第 466 行`](../src/fix_parse_benchmark.cpp#L466)
  -> [`定义第 347 行`](../src/fix_parse_benchmark.cpp#L347)
  -> [`buildFixMessage() 调用第 465 行`](../src/fix_parse_benchmark.cpp#L465)
  -> [`定义第 202 行`](../src/fix_parse_benchmark.cpp#L202)
- MDW1：
  [`调用第 470 行`](../src/fix_parse_benchmark.cpp#L470)
  -> [`定义第 412 行`](../src/fix_parse_benchmark.cpp#L412)
  -> [`buildFixMessage() 调用第 469 行`](../src/fix_parse_benchmark.cpp#L469)
  -> [`定义第 202 行`](../src/fix_parse_benchmark.cpp#L202)

#### B. 完整 wire message 进入 fixed parser

parse 模式：

[`runParseBenchmark()` 调用第 1892 行](../src/fix_parse_benchmark.cpp#L1892)<br>
-> [`Message::setString()` 定义第 564 行](../src/C++/Message.cpp#L564)

server 模式：

[`SocketConnection::readMessages()` 调用第 425 行](../src/C++/SocketConnection.cpp#L425)<br>
-> [`Session::next(string)` 定义第 1170 行](../src/C++/Session.cpp#L1170)

[`Session::next(string)` 构造 Message 第 1179 行](../src/C++/Session.cpp#L1179)<br>
-> [`Message` 字典构造函数定义第 225 行](../src/C++/Message.cpp#L225)

[`Message` 构造函数调用第 228 行](../src/C++/Message.cpp#L228)<br>
-> [`Message::setString()` 定义第 564 行](../src/C++/Message.cpp#L564)

#### C. 尝试模板

[`Message::setString()` 调用第 572 行](../src/C++/Message.cpp#L572)<br>
-> [`Message::setFixedLayoutString()` 定义第 482 行](../src/C++/Message.cpp#L482)

[`长度与 marker 判断第 487 行`](../src/C++/Message.cpp#L487)、
[`第 490 行`](../src/C++/Message.cpp#L490) 或
[`第 493 行`](../src/C++/Message.cpp#L493)<br>
-> [`hasFixedLayoutMarker()` 定义第 200 行](../src/C++/Message.cpp#L200)

#### D. offset 构造字段

[`setFixedLayoutString()` 调用 makeField 第 520 行](../src/C++/Message.cpp#L520)<br>
-> [`makeField` lambda 定义第 510 行](../src/C++/Message.cpp#L510)

[`makeField` 调用 FieldBase 第 515 行](../src/C++/Message.cpp#L515)<br>
-> [`FieldBase` iterator 构造函数定义第 69 行](../include/quickfix/Field.h#L69)

[`Header appendField 调用第 522 行`](../src/C++/Message.cpp#L522)、
[`Trailer 调用第 524 行`](../src/C++/Message.cpp#L524) 或
[`Body 调用第 526 行`](../src/C++/Message.cpp#L526)<br>
-> [`FieldMap::appendField()` 定义第 282 行](../include/quickfix/FieldMap.h#L282)

#### E. MDW1 group

[`group makeField/appendField 调用第 539 行`](../src/C++/Message.cpp#L539)<br>
-> [`makeField` lambda 定义第 510 行](../src/C++/Message.cpp#L510)

[`Message::addGroup() 调用第 541 行`](../src/C++/Message.cpp#L541)<br>
-> [`Message::addGroup()` 定义第 163 行](../src/C++/Message.h#L163)

[`Message::addGroup()` 内部调用第 163 行](../src/C++/Message.h#L163)<br>
-> [`FieldMap::addGroup()` 定义第 79 行](../src/C++/FieldMap.cpp#L79)

[`FieldMap::addGroup()` 调用第 82 行](../src/C++/FieldMap.cpp#L82)<br>
-> [`FieldMap::addGroupPtr()` 定义第 85 行](../src/C++/FieldMap.cpp#L85)

#### F. 成功或 fallback

模板成功：

```text
setFixedLayoutString() 返回 true
  -> Message::setString() 第 573 行直接返回
  -> 已得到 Header、Body、Trailer 和可选 MD groups
```

模板失败：

[`Message::setString()` 普通字段调用第 585 行](../src/C++/Message.cpp#L585)<br>
-> [`Message::extractField()` 定义第 835 行](../src/C++/Message.cpp#L835)

[`普通 group 调用第 631 行`](../src/C++/Message.cpp#L631)<br>
-> [`Message::setGroup()` 定义第 646 行](../src/C++/Message.cpp#L646)

[`普通 validation 调用第 642 行`](../src/C++/Message.cpp#L642)<br>
-> [`Message::validate()` 定义第 803 行](../src/C++/Message.cpp#L803)

## 7. 最短阅读路线

第一次阅读时，可以先只按下面顺序点击：

1. [`run_parse_benchmark.sh` 第 13 行](../test/run_parse_benchmark.sh#L13)
2. [`main()` 第 2329 行](../src/fix_parse_benchmark.cpp#L2329)
3. [`runServerBenchmark()` 第 2052 行](../src/fix_parse_benchmark.cpp#L2052)
4. [`runRawServerBenchmark()` 第 1911 行](../src/fix_parse_benchmark.cpp#L1911) 或
   [`runQuickfixServerBenchmark()` 第 1976 行](../src/fix_parse_benchmark.cpp#L1976)
5. [`Acceptor::start()` 第 146 行](../src/C++/Acceptor.cpp#L146)
6. [`Acceptor::startThread()` 第 251 行](../src/C++/Acceptor.cpp#L251)
7. [`SocketAcceptor::onStart()` 第 290 行](../src/C++/SocketAcceptor.cpp#L290)
8. blocking/poll0：
   [`SocketServer::block()` 第 258 行](../src/C++/SocketServer.cpp#L258)
9. blocking/poll0：
   [`SocketMonitor::block()` 第 305 行](../src/C++/SocketMonitor_UNIX.cpp#L305)
10. direct：
    [`SocketAcceptor::runDirectScanOnce()` 第 359 行](../src/C++/SocketAcceptor.cpp#L359)
11. 共同接收入口：
    [`SocketConnection::readMessages()` 第 411 行](../src/C++/SocketConnection.cpp#L411)
12. [`Parser::readFixMessage()` 第 295 行](../src/C++/Parser.cpp#L295)
13. [`Session::next(const std::string&)` 第 1170 行](../src/C++/Session.cpp#L1170)
14. [`Session::fromCallback()` 第 1069 行](../src/C++/Session.cpp#L1069)
15. [`BenchmarkApplication::fromApp()` 第 156 行](../src/fix_parse_benchmark.cpp#L156)

这条最短路线先建立宏观结构；之后再回到第 3.7 节看原始发送队列，或回到第 4.2 到 4.6 节逐项看
direct 的 accept、recv、send、timer 和 disconnect。
