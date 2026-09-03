# Phase 1: Benchmark Code Walkthrough

这份文档是第一阶段导读：只看 benchmark 实验场景本身，不深入 QuickFIX 内部网络、Parser、Message、Session 的实现细节。

目标是先回答这些问题：

- `./test/run_parse_benchmark.sh ...` 到底运行了哪个程序？
- 命令行参数如何变成 C++ 里的 `Options`？
- benchmark 如何生成普通 FIX 消息和 fixed-layout FIX 消息？
- parse mode 和 server mode 分别测了什么？
- 输出里的 `messages_per_second`、`received`、`sink` 这些字段从哪里来？

建议你打开两个文件对照读：

```text
test/run_parse_benchmark.sh
src/fix_parse_benchmark.cpp
```

## 1. 从运行命令开始

我们以这条命令作为第一阶段主线：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=1000 --port=54381
```

这条命令的意思是：

- `--mode=server`：启动 QuickFIX acceptor，通过 socket 发消息给它。
- `--client=raw`：client 端不使用 QuickFIX initiator，而是直接用 raw socket 发送预生成的 FIX wire bytes。
- `--message=new-order-single`：生成下单消息，FIX MsgType 是 `35=D`。
- `--messages=1000`：发送 1000 条 application message。
- `--port=54381`：server 监听端口。

这一阶段先只关心 benchmark 自己如何准备和驱动这个测试。QuickFIX 内部怎么 accept、recv、parse，下一阶段再看。

## 2. 脚本入口

先看：

```text
test/run_parse_benchmark.sh:1-13
```

关键行：

```sh
SCRIPT=$(realpath "$0")
DIR=$(dirname "$SCRIPT")
cd "$DIR" || exit 1
```

这几行保证无论你从哪个目录执行脚本，它都会先切到 `test/` 目录。

再看：

```sh
BENCHMARK="$DIR/fix_parse_benchmark"
if [ ! -x "$BENCHMARK" ] && [ -x "$DIR/../lib/fix_parse_benchmark" ]; then
  BENCHMARK="$DIR/../lib/fix_parse_benchmark"
fi
```

脚本优先运行：

```text
test/fix_parse_benchmark
```

如果它不存在，就尝试：

```text
lib/fix_parse_benchmark
```

最后一行：

```sh
"$BENCHMARK" --data-dictionary "$DIR/../spec/FIX42.xml" "$@"
```

这说明脚本会自动给 benchmark 加上：

```bash
--data-dictionary ../spec/FIX42.xml
```

然后把你输入的其他参数原样传给 C++ 程序。

也就是说，你输入：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw
```

最终等价于运行：

```bash
test/fix_parse_benchmark --data-dictionary spec/FIX42.xml --mode=server --client=raw
```

## 3. 为什么脚本总是跑最后一次 build 的版本

看：

```text
src/CMakeLists.txt:48-61
```

关键行：

```cmake
add_executable(fix_parse_benchmark fix_parse_benchmark.cpp)
target_link_libraries(fix_parse_benchmark ${PROJECT_NAME})
```

这说明 `fix_parse_benchmark` 是一个单独的可执行程序，并且链接 QuickFIX library。

再看：

```cmake
ADD_CUSTOM_TARGET(fix_parse_benchmark_target ALL
                  COMMAND ${CMAKE_COMMAND} -E create_symlink
                    $<TARGET_FILE:fix_parse_benchmark>
                    ${PROJECT_SOURCE_DIR}/test/fix_parse_benchmark
                  DEPENDS fix_parse_benchmark)
```

这会把当前 build 出来的 `fix_parse_benchmark` 链接到：

```text
test/fix_parse_benchmark
```

所以你每次 build 不同 profile，例如：

```bash
build-bench-baseline
build-bench-simd-pattern
build-bench-busy-poll
```

只要 build target 是：

```bash
cmake --build <build-dir> --target fix_parse_benchmark_target
```

那么 `test/fix_parse_benchmark` 就会指向最后一次 build 出来的版本。这也是为什么切换编译期开关后要重新 build。

## 4. 文件顶部：benchmark 依赖了哪些 QuickFIX 组件

看：

```text
src/fix_parse_benchmark.cpp:29-46
```

这里包含了 QuickFIX 的关键头文件：

```cpp
#include "Application.h"
#include "DataDictionary.h"
#include "Message.h"
#include "Parser.h"
#include "Session.h"
#include "SessionSettings.h"
#include "SocketAcceptor.h"
#include "SocketInitiator.h"
```

这些头文件对应 benchmark 会用到的几个层次：

- `Message`：直接解析完整 FIX 字符串。
- `Parser`：从连续 byte stream 里切出完整 FIX 消息，主要用于 self-test。
- `SocketAcceptor`：server mode 下启动 QuickFIX acceptor。
- `SocketInitiator`：`--client=quickfix` 时启动 QuickFIX initiator。
- `SessionSettings`：用字符串拼出 QuickFIX 配置。
- `DataDictionary`：`--validate=yes` 时加载 FIX42 XML。

还包含了几类 FIX42 typed message：

```cpp
#include "fix42/MarketDataSnapshotFullRefresh.h"
#include "fix42/NewOrderSingle.h"
#include "fix42/OrderCancelRequest.h"
#include "fix42/QuoteRequest.h"
```

这些主要用于 `--client=quickfix` 路径，让 QuickFIX initiator 构造正常 typed message。raw client 路径不依赖这些 typed message，它直接发送字符串。

## 5. 常量和 Options

看：

```text
src/fix_parse_benchmark.cpp:66-73
```

关键常量：

```cpp
const char SOH = '\001';
const char *BeginString = "FIX.4.2";
const char *ClientCompID = "CLIENT";
const char *ServerCompID = "SERVER";
```

这里定义了 benchmark 消息的基础身份：

- FIX 版本固定为 `FIX.4.2`
- client comp id 是 `CLIENT`
- server comp id 是 `SERVER`
- FIX 字段分隔符是 `SOH`，字节值是 `\001`

再看：

```text
src/fix_parse_benchmark.cpp:71-73
```

三个枚举：

```cpp
enum class Mode { Parse, Server, Both };
enum class ClientMode { Raw, Quickfix };
enum class MessageKind { NewOrderSingle, OrderCancelRequest, MarketDataSnapshot, QuoteRequest };
```

它们对应命令行里的：

- `--mode=parse|server|both`
- `--client=raw|quickfix`
- `--message=new-order-single|order-cancel-request|market-data-snapshot|quote-request`

再看：

```text
src/fix_parse_benchmark.cpp:75-94
```

`Options` 是整个 benchmark 的参数汇总：

```cpp
struct Options {
  Mode mode = Mode::Both;
  ClientMode clientMode = ClientMode::Raw;
  MessageKind messageKind = MessageKind::NewOrderSingle;
  std::uint64_t messages = 100000;
  std::uint64_t warmup = 10000;
  int port = 0;
  int quoteGroups = 10;
  int sendBufferSize = 0;
  int receiveBufferSize = 0;
  int serverWaitSeconds = 30;
  bool validate = false;
  bool selfTestFastScan = false;
  bool selfTestParser = false;
  bool selfTestCorrectness = false;
  bool fixedLayout = false;
  bool busyPoll = false;
  int busyPollCpu = -1;
  std::string dataDictionaryPath = "spec/FIX42.xml";
};
```

理解这个结构很重要，因为后面每个函数基本都只接收一个 `Options`，根据它决定生成什么消息、启动什么模式、是否 validate、是否 fixed-layout、是否 busy-poll。

## 6. BenchmarkApplication：server 怎么知道自己收到了多少消息

看：

```text
src/fix_parse_benchmark.cpp:130-143
```

这个类继承：

```cpp
class BenchmarkApplication : public FIX::NullApplication
```

它只做三件事：

```cpp
std::atomic<std::uint64_t> received{0};
std::atomic<bool> loggedOn{false};
```

- `received`：server 收到 application message 的数量。
- `loggedOn`：session 是否已经 logon。

看：

```text
src/fix_parse_benchmark.cpp:135-136
```

```cpp
void onLogon(...) override { loggedOn.store(true, ...); }
void onLogout(...) override { loggedOn.store(false, ...); }
```

QuickFIX session 登录成功后会调用 `onLogon`。

再看：

```text
src/fix_parse_benchmark.cpp:138-142
```

```cpp
void fromApp(const FIX::Message &, const FIX::SessionID &) override {
  received.fetch_add(1, std::memory_order_relaxed);
}
```

这就是 server benchmark 的最终计数点。每当 QuickFIX 成功解析并把 app message 交给应用层，`received` 加 1。

所以 server mode 输出里的：

```text
received=100000
```

最终就是这里累加出来的。

## 7. FIX 字符串是怎么拼出来的

### 7.1 时间和定长数字

看：

```text
src/fix_parse_benchmark.cpp:145-150
```

```cpp
std::string timestamp()
std::string fixedNumber(std::uint64_t value, int width)
```

`timestamp()` 生成 QuickFIX 格式的 UTC 时间，例如：

```text
20260716-12:34:56
```

`fixedNumber()` 用于 fixed-layout，把数字补零到固定宽度。例如：

```text
fixedNumber(12, 12) -> 000000000012
```

### 7.2 单个字段

看：

```text
src/fix_parse_benchmark.cpp:153-155
```

```cpp
std::string field(int tag, const std::string &value) {
  return std::to_string(tag) + "=" + value + SOH;
}
```

这就是最基本的 FIX 字段拼接：

```text
tag=value<SOH>
```

例如：

```text
35=D<SOH>
55=LNUX<SOH>
```

### 7.3 完整 FIX 消息

看：

```text
src/fix_parse_benchmark.cpp:157-173
```

这是 raw client 路径最重要的函数：

```cpp
std::string buildFixMessage(const std::vector<std::pair<int, std::string>> &fields)
```

它的输入是 body/header-like 字段列表，例如：

```text
35=D
49=CLIENT
56=SERVER
34=2
52=...
11=ORDER-2
...
```

函数内部先拼 body：

```cpp
std::string body;
for (const auto &entry : fields) {
  body += field(entry.first, entry.second);
}
```

然后生成开头：

```cpp
std::string message = field(8, BeginString) + field(9, std::to_string(body.size())) + body;
```

这一步会自动计算：

```text
8=FIX.4.2<SOH>
9=<BodyLength><SOH>
```

`BodyLength` 是 `body.size()`。

之后计算 checksum：

```cpp
unsigned int checksum = 0;
for (unsigned char c : message) {
  checksum += c;
}
```

最后追加：

```cpp
10=<checksum % 256, 三位补零><SOH>
```

因此 benchmark 生成的 raw message 是完整合法的 FIX wire string。

## 8. 普通消息模板

### 8.1 公共 header 字段

看：

```text
src/fix_parse_benchmark.cpp:175-184
```

```cpp
headerFields(msgType, seqNum, now)
```

它生成：

```text
35=<MsgType>
49=CLIENT
56=SERVER
34=<seqNum>
52=<now>
```

注意这里没有 `8=`、`9=`、`10=`。这些由 `buildFixMessage()` 自动补。

### 8.2 fixed-layout 公共 header 字段

看：

```text
src/fix_parse_benchmark.cpp:186-195
```

`fixedHeaderFields()` 和普通 header 的区别是：

```cpp
{34, fixedNumber(seqNum, 12)}
```

也就是 `34=MsgSeqNum` 固定为 12 位补零数字，保证字段长度稳定。

### 8.3 普通下单 NewOrderSingle

看：

```text
src/fix_parse_benchmark.cpp:197-210
```

`newOrderSingleFields()` 生成普通下单：

```text
35=D
49=CLIENT
56=SERVER
34=<seq>
52=<now>
11=ORDER-<seq>
21=1
55=LNUX
54=1
60=<now>
38=100
40=1
59=0
15=USD
```

这里 `35=D` 表示 NewOrderSingle。

### 8.4 fixed-layout 下单 NOS1

看：

```text
src/fix_parse_benchmark.cpp:212-227
```

`fixedNewOrderSingleFields()` 生成固定模板下单：

```text
35=D
49=CLIENT
56=SERVER
34=<12位补零 seq>
52=<now>
9001=NOS1
11=ORDER-<12位补零 seq>
21=1
55=LNUX
54=1
60=<now>
38=0000000100
40=1
59=0
15=USD
```

关键点：

- 多了自定义 tag `9001=NOS1`
- `34`、`11`、`38` 都使用固定长度
- 字段顺序固定

这就是后面 fixed-layout parser 能用 offset 直接取字段的前提。

### 8.5 普通撤单 OrderCancelRequest

看：

```text
src/fix_parse_benchmark.cpp:229-239
```

`orderCancelRequestFields()` 生成：

```text
35=F
41=ORIG-<seq-1>
11=CNCL-<seq>
55=LNUX
54=1
60=<now>
38=100
```

其中 `35=F` 表示 OrderCancelRequest。

### 8.6 fixed-layout 撤单 CXL1

看：

```text
src/fix_parse_benchmark.cpp:241-254
```

`fixedOrderCancelRequestFields()` 生成：

```text
35=F
9001=CXL1
41=ORIG-<12位补零 origSeq>
11=CNCL-<12位补零 seq>
55=LNUX
54=1
60=<now>
38=0000000100
```

关键点和 NOS1 类似：

- marker 是 `9001=CXL1`
- 序号、数量字段固定长度
- 字段顺序固定

### 8.7 行情 repeating group

看：

```text
src/fix_parse_benchmark.cpp:256-269
```

`appendMarketDataEntries()` 会添加 3 组行情 entries：

```text
269=<type>
270=<price>
271=<size>
273=12:34:56
```

普通价格和数量：

```text
123.45 / 500
123.46 / 400
123.455 / 100
```

fixed-layout 价格和数量：

```text
00000123.4500 / 0000000500
00000123.4600 / 0000000400
00000123.4550 / 0000000100
```

这也是行情 fixed-layout 模板能定长解析的关键。

### 8.8 普通行情快照

看：

```text
src/fix_parse_benchmark.cpp:271-279
```

`marketDataSnapshotFields()` 生成：

```text
35=W
262=MDREQ-<seq>
55=LNUX
268=3
三组 269/270/271/273
```

`35=W` 表示 MarketDataSnapshotFullRefresh。

### 8.9 fixed-layout 行情快照 MDW1

看：

```text
src/fix_parse_benchmark.cpp:281-291
```

`fixedMarketDataSnapshotFields()` 生成：

```text
35=W
9001=MDW1
262=MDREQ-<12位补零 seq>
55=LNUX
268=3
三组定长 269/270/271/273
```

marker 是：

```text
9001=MDW1
```

### 8.10 QuoteRequest

看：

```text
src/fix_parse_benchmark.cpp:293-310
```

`quoteRequestFields()` 用于构造比较长的普通消息：

```text
35=R
131=QR-<seq>
146=<quoteGroups>
每组包含 55/200/201/202/54/38/40/60/15
```

它目前不支持 fixed-layout，主要用于制造更长、更多 repeating group 的普通解析压力。

## 9. makeApplicationMessage：根据 Options 选择消息模板

看：

```text
src/fix_parse_benchmark.cpp:312-331
```

这是所有 raw/string 消息生成的统一入口：

```cpp
std::string makeApplicationMessage(std::uint64_t seqNum, const Options &options)
```

它先取当前时间：

```cpp
const std::string now = timestamp();
```

然后根据：

```cpp
options.messageKind
options.fixedLayout
```

选择具体模板。

比如：

```cpp
case MessageKind::NewOrderSingle:
  return buildFixMessage(
      options.fixedLayout ? fixedNewOrderSingleFields(...)
                          : newOrderSingleFields(...));
```

因此：

```bash
--message=new-order-single
```

决定用下单模板。

```bash
--fixed-layout
```

决定用 `fixedNewOrderSingleFields()`，也就是带 `9001=NOS1` 的定长模板。

## 10. Logon 消息

看：

```text
src/fix_parse_benchmark.cpp:333-339
```

`makeLogonMessage()` 生成 raw client 登录消息：

```text
35=A
49=CLIENT
56=SERVER
34=1
52=<now>
98=0
108=30
```

raw client 不是 QuickFIX initiator，所以它必须自己先发一条 FIX Logon，让 server acceptor 建立会话。后面的 application message 从 `seqNum=2` 开始，这就是为什么很多地方调用：

```cpp
makeApplicationMessage(i + 2, options)
```

## 11. 配置生成：makeAcceptorConfig

看：

```text
src/fix_parse_benchmark.cpp:363-401
```

`makeAcceptorConfig()` 用字符串拼 QuickFIX acceptor 配置。

基础配置：

```text
ConnectionType=acceptor
SocketAcceptPort=<port>
SocketReuseAddress=Y
SocketNodelay=Y
StartTime=00:00:00
EndTime=00:00:00
CheckLatency=N
PersistMessages=N
UseDataDictionary=Y/N
```

几个参数如何影响配置：

```text
--send-buffer-size=N      -> SendBufferSize=N
--receive-buffer-size=N   -> ReceiveBufferSize=N
--busy-poll               -> SocketBusyPoll=Y
--busy-poll-cpu=N         -> SocketBusyPollCpu=N
--validate=yes            -> DataDictionary=spec/FIX42.xml
--fixed-layout + validate -> ValidateUserDefinedFields=N
```

最后的 session 身份是：

```text
BeginString=FIX.4.2
SenderCompID=SERVER
TargetCompID=CLIENT
HeartBtInt=30
```

这说明 acceptor 视角下，它自己是 `SERVER`，对端是 `CLIENT`。

## 12. makeInitiatorConfig

看：

```text
src/fix_parse_benchmark.cpp:403-436
```

这个只用于：

```bash
--client=quickfix
```

它生成 QuickFIX initiator 配置：

```text
ConnectionType=initiator
SocketConnectHost=127.0.0.1
SocketConnectPort=<port>
SenderCompID=CLIENT
TargetCompID=SERVER
```

raw client 路径不使用这个配置。

第一阶段你可以先略读这里，因为我们当前主要实验是 `--client=raw`。

## 13. makeOutboundStream：raw client 一次性准备全部应用消息

看：

```text
src/fix_parse_benchmark.cpp:438-450
```

`makeOutboundStream()` 先生成一条 sample，检查总大小是否可能溢出：

```cpp
std::string sample = makeApplicationMessage(2, options);
```

然后预留内存：

```cpp
outbound.reserve(options.messages * sample.size());
```

再循环拼接：

```cpp
for (std::uint64_t i = 0; i < options.messages; ++i) {
  outbound += makeApplicationMessage(i + 2, options);
}
```

所以 raw server benchmark 的发送方式是：

```text
先把 N 条 FIX application message 拼成一个大字符串
再通过 socket sendAll 发送出去
```

这也是 raw client 比 quickfix client 更适合隔离 server 解析性能的原因：client 侧没有每条消息都构造 QuickFIX typed object。

## 14. makeQuickfixApplicationMessage：quickfix client 的消息构造

看：

```text
src/fix_parse_benchmark.cpp:460-518
```

这个函数只用于：

```bash
--client=quickfix
```

它不是拼字符串，而是构造 QuickFIX typed message：

```cpp
FIX42::NewOrderSingle
FIX42::OrderCancelRequest
FIX42::MarketDataSnapshotFullRefresh
FIX42::QuoteRequest
```

然后通过：

```cpp
FIX::Session::sendToTarget(message, clientSessionID)
```

发送。

这个路径更接近真实 QuickFIX 双端通信，但它引入了 client 侧对象构造、序列化、session 处理等变量，所以我们主实验用 raw client。

## 15. 输出指标从哪里来

看：

```text
src/fix_parse_benchmark.cpp:520-546
```

`elapsedSeconds()` 计算耗时：

```cpp
std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count()
```

`printResult()` 计算：

```cpp
messagesPerSecond = messages / seconds
mibPerSecond = bytes / seconds / 1024 / 1024
nsPerMessage = seconds * 1e9 / messages
```

然后输出：

```text
mode=
client=
message=
messages=
bytes=
validate=
fixed_layout=
busy_poll=
busy_poll_cpu=
seconds=
messages_per_second=
MiB_per_second=
ns_per_message=
```

所以性能结果不是 QuickFIX 自己输出的，是 benchmark 根据开始/结束时间算出来的。

## 16. self-test 入口概览

第一阶段先知道位置即可，不必全部细读。

### 16.1 fast-scan self-test

看：

```text
src/fix_parse_benchmark.cpp:553-656
```

它验证：

- 单字符 scalar / SIMD / fast scan 返回位置一致。
- `SOH10=` scalar / SIMD / fast scan 返回位置一致。

运行命令：

```bash
./test/run_parse_benchmark.sh --self-test-fast-scan
```

### 16.2 parser self-test

看：

```text
src/fix_parse_benchmark.cpp:658-730
```

它用 `FIX::Parser` 验证：

- 单条消息能切出来。
- 两条粘包消息能切成两条。
- 半条消息不会提前切出。
- 前面有 junk 时能找到真正的 `8=`。
- 长 quote request 能切出来。

运行命令：

```bash
./test/run_parse_benchmark.sh --self-test-parser
```

这些 self-test 是后面看 `Parser.cpp` 的前置背景。

## 17. parse mode：只测 Message::setString

看：

```text
src/fix_parse_benchmark.cpp:1334-1364
```

`runParseBenchmark()` 的流程：

```text
可选加载 DataDictionary
生成一条 sample FIX 消息
warmup 阶段反复 message.setString(sample)
正式计时阶段反复 message.setString(sample)
输出 parse 性能
```

核心代码：

```cpp
message.setString(sample, options.validate, dictionary);
```

这说明 parse mode 直接从完整 FIX 字符串进入 `Message::setString()`。

它不经过：

- socket
- `SocketAcceptor`
- `SocketConnection`
- `Parser::readFixMessage()`
- busy-poll
- SIMD stream parser
- `SOH10=` pattern scan

所以：

```bash
--mode=parse
```

适合看 `Message` 字段解析性能，尤其适合对比 fixed-layout parser。

但它不能用来评估网络层和 stream parser 优化。

输出里额外有：

```text
sample_bytes=
warmup_messages=
sink=
```

其中 `sink` 是为了防止编译器把解析结果完全优化掉：

```cpp
sink += message.getHeader().isSetField(35) ? 1 : 0;
```

## 18. raw server mode：我们当前最重要的实验路径

看：

```text
src/fix_parse_benchmark.cpp:1366-1412
```

`runRawServerBenchmark()` 是当前最重要的入口。

它的流程：

```text
创建 BenchmarkApplication
创建 MemoryStoreFactory
创建 SessionID(FIX.4.2, SERVER, CLIENT)
用 makeAcceptorConfig 拼 acceptor 配置
创建 FIX::SocketAcceptor
acceptor.start()
获取实际监听端口
raw socket connect 到 acceptor
raw socket 发送 Logon
等待 server onLogon
makeOutboundStream 生成全部应用消息
开始计时
sendAll 发送全部应用消息
等待 received == messages
结束计时
输出结果
acceptor.stop()
```

关键行：

```text
src/fix_parse_benchmark.cpp:1371-1374
```

```cpp
std::istringstream configStream(makeAcceptorConfig(options, options.port));
FIX::SessionSettings settings(configStream);
FIX::SocketAcceptor acceptor(application, storeFactory, settings);
acceptor.start();
```

这几行真正启动了 QuickFIX acceptor。

再看：

```text
src/fix_parse_benchmark.cpp:1384-1385
```

```cpp
socket = connectWithRetry(portEntry->second, options);
sendAll(socket.get(), makeLogonMessage());
```

raw client 自己连接 server，然后先发 Logon。

再看：

```text
src/fix_parse_benchmark.cpp:1391-1394
```

```cpp
const std::string outbound = makeOutboundStream(options);
const auto start = std::chrono::steady_clock::now();
sendAll(socket.get(), outbound);
if (!waitForMessages(application, options.messages, options.serverWaitSeconds)) ...
```

计时范围是：

```text
开始发送应用消息
  -> server 解析并 fromApp 计数到 N
结束
```

所以 server mode 的耗时包含：

- raw client 发送时间
- socket 传输
- QuickFIX acceptor 收包
- Parser 切完整消息
- Message 解析字段
- Session 处理
- Application::fromApp 计数

但不包含 Logon 之前的准备时间，也不包含提前生成 outbound 的时间。

输出额外字段：

```text
port=
prepared_app_bytes=
received=
```

- `prepared_app_bytes`：`outbound.size()`，即所有 application FIX 消息总字节数。
- `received`：`BenchmarkApplication::received`，即 server app 层收到的消息数。

## 19. quickfix server mode：参考路径

看：

```text
src/fix_parse_benchmark.cpp:1414-1474
```

`runQuickfixServerBenchmark()` 用于：

```bash
--client=quickfix
```

流程是：

```text
启动 server acceptor
启动 client initiator
等待双方 logon
循环构造 QuickFIX typed Message
Session::sendToTarget()
等待 server received == messages
输出结果
```

关键行：

```text
src/fix_parse_benchmark.cpp:1446-1450
```

```cpp
FIX::Message message = makeQuickfixApplicationMessage(i + 2, options);
FIX::Session::sendToTarget(message, clientSessionID)
```

这个路径更像真实 QuickFIX 使用方式，但它不适合精确隔离 server parser 性能，因为 client 端也在做 QuickFIX 对象构造和 session 发送。

## 20. runServerBenchmark：选择 raw 还是 quickfix

看：

```text
src/fix_parse_benchmark.cpp:1476-1482
```

```cpp
void runServerBenchmark(const Options &options) {
  if (options.clientMode == ClientMode::Raw) {
    runRawServerBenchmark(options);
  } else {
    runQuickfixServerBenchmark(options);
  }
}
```

也就是说：

```bash
--client=raw
```

进入 `runRawServerBenchmark()`。

```bash
--client=quickfix
```

进入 `runQuickfixServerBenchmark()`。

## 21. 命令行解析 parseOptions

看：

```text
src/fix_parse_benchmark.cpp:1484-1521
```

这里是一些基础解析函数：

- `parseUnsigned`
- `parseInt`
- `parseBool`

`parseBool` 支持：

```text
1 / Y / y / yes / true
0 / N / n / no / false
```

所以这些写法等价：

```bash
--fixed-layout
--fixed-layout=yes
--fixed-layout=true
--fixed-layout=Y
```

看：

```text
src/fix_parse_benchmark.cpp:1523-1531
```

`requireValue()` 支持两种参数形式：

```bash
--messages 100000
--messages=100000
```

再看：

```text
src/fix_parse_benchmark.cpp:1533-1556
```

`printUsage()` 是 `--help` 输出。

核心解析入口：

```text
src/fix_parse_benchmark.cpp:1558-1672
```

重点看几个分支：

```text
1575-1585: --mode
1586-1594: --client
1595-1607: --message
1608-1611: --messages / --warmup
1612-1619: --validate / --fixed-layout
1620-1629: --busy-poll / --busy-poll-cpu
1634-1641: --port / socket buffer / server wait timeout
1642-1647: self-test flags
1653-1671: 参数合法性检查
```

两个特别重要的限制：

```text
src/fix_parse_benchmark.cpp:1665-1667
```

```cpp
if (options.fixedLayout && options.messageKind == MessageKind::QuoteRequest) {
  throw std::runtime_error("--fixed-layout is not supported for quote-request");
}
```

所以 quote-request 不支持 fixed-layout。

再看：

```text
src/fix_parse_benchmark.cpp:1668-1671
```

```cpp
if (options.fixedLayout && options.clientMode == ClientMode::Quickfix
    && (options.mode == Mode::Server || options.mode == Mode::Both)) {
  throw std::runtime_error("--fixed-layout is only supported with --client=raw in server mode");
}
```

所以 server mode 下 fixed-layout 必须配 raw client。

原因是 fixed-layout 要保证 wire bytes 字段顺序和长度完全由 benchmark 控制，而 QuickFIX initiator 可能重新序列化字段。

## 22. main：整个 benchmark 的总入口

看：

```text
src/fix_parse_benchmark.cpp:1677-1716
```

程序入口：

```cpp
int main(int argc, char **argv)
```

第一步：

```cpp
std::signal(SIGPIPE, SIG_IGN);
```

Linux/WSL 下忽略 `SIGPIPE`，避免 socket 对端关闭时进程被信号直接杀掉。

然后解析参数：

```cpp
const Options options = parseOptions(argc, argv);
```

接着优先处理 self-test：

```cpp
if (options.selfTestFastScan) { runFastScanSelfTest(); return 0; }
if (options.selfTestParser) { runParserSelfTest(); return 0; }
if (options.selfTestCorrectness) { runCorrectnessSelfTest(options); return 0; }
```

如果不是 self-test，就初始化 socket：

```cpp
FIX::socket_init();
```

然后按 mode 分发：

```cpp
if (options.mode == Mode::Parse || options.mode == Mode::Both) {
  runParseBenchmark(options);
}
if (options.mode == Mode::Server || options.mode == Mode::Both) {
  runServerBenchmark(options);
}
```

这解释了：

```bash
--mode=parse
```

只跑 `runParseBenchmark()`。

```bash
--mode=server
```

只跑 `runServerBenchmark()`。

```bash
--mode=both
```

先跑 parse，再跑 server。

最后：

```cpp
FIX::socket_term();
```

异常时输出：

```text
fix_parse_benchmark: <error>
```

并返回 `1`。

## 23. 用一条命令串起第一阶段代码

命令：

```bash
./test/run_parse_benchmark.sh --mode=server --client=raw --message=new-order-single --messages=1000 --port=54381
```

对应代码路径：

```text
test/run_parse_benchmark.sh:1-13
  -> 找到 test/fix_parse_benchmark
  -> 自动加 --data-dictionary spec/FIX42.xml

src/fix_parse_benchmark.cpp:1677-1716 main()
  -> parseOptions()
  -> mode=server
  -> runServerBenchmark()

src/fix_parse_benchmark.cpp:1558-1672 parseOptions()
  -> clientMode=Raw
  -> messageKind=NewOrderSingle
  -> messages=1000
  -> port=54381

src/fix_parse_benchmark.cpp:1476-1482 runServerBenchmark()
  -> runRawServerBenchmark()

src/fix_parse_benchmark.cpp:1366-1412 runRawServerBenchmark()
  -> makeAcceptorConfig()
  -> SocketAcceptor start()
  -> connectWithRetry()
  -> sendAll(Logon)
  -> makeOutboundStream()
  -> sendAll(outbound)
  -> waitForMessages()
  -> printResult()

src/fix_parse_benchmark.cpp:438-450 makeOutboundStream()
  -> makeApplicationMessage(i + 2)

src/fix_parse_benchmark.cpp:312-331 makeApplicationMessage()
  -> newOrderSingleFields()
  -> buildFixMessage()

src/fix_parse_benchmark.cpp:157-173 buildFixMessage()
  -> 生成 8=
  -> 计算 9=BodyLength
  -> 计算 10=CheckSum
```

这就是第一阶段最重要的完整链路。

## 24. 第一阶段读代码顺序建议

建议你按这个顺序读，不要从文件头一路读到底：

1. `test/run_parse_benchmark.sh:1-13`
2. `src/fix_parse_benchmark.cpp:1677-1716`，先看 `main()`
3. `src/fix_parse_benchmark.cpp:1558-1672`，看 `parseOptions()`
4. `src/fix_parse_benchmark.cpp:75-94`，理解 `Options`
5. `src/fix_parse_benchmark.cpp:157-173`，理解 `buildFixMessage()`
6. `src/fix_parse_benchmark.cpp:175-331`，理解各种消息模板
7. `src/fix_parse_benchmark.cpp:363-401`，理解 acceptor 配置
8. `src/fix_parse_benchmark.cpp:438-450`，理解 raw outbound stream
9. `src/fix_parse_benchmark.cpp:1366-1412`，理解 raw server benchmark
10. `src/fix_parse_benchmark.cpp:1334-1364`，回头理解 parse benchmark
11. `src/fix_parse_benchmark.cpp:520-546`，理解输出指标

读完这 11 步，你就能回答 benchmark 层面的核心问题：

- 测试消息是什么类型？
- 字段是怎么拼出来的？
- fixed-layout 消息是怎么生成的？
- raw client 和 quickfix client 有什么区别？
- parse mode 和 server mode 到底测了哪一段？
- 输出指标是怎么计算的？

## 25. 第一阶段结束后你应该形成的 mental model

这一阶段先记住这张图：

```text
run_parse_benchmark.sh
  -> fix_parse_benchmark main()
  -> parseOptions()
  -> makeApplicationMessage()
  -> buildFixMessage()
  -> runParseBenchmark() 或 runRawServerBenchmark()
```

parse mode：

```text
完整 FIX 字符串
  -> Message::setString()
  -> 计时
```

server/raw mode：

```text
N 条完整 FIX 字符串先拼成 outbound
  -> raw socket sendAll()
  -> QuickFIX acceptor 收到并解析
  -> BenchmarkApplication::fromApp()
  -> received++
```

下一阶段就可以从 `SocketAcceptor` 开始，沿着 QuickFIX 内部真实接收路径继续走：

```text
SocketAcceptor
  -> SocketServer
  -> SocketMonitor_UNIX
  -> SocketConnection::readFromSocket()
  -> Parser::readFixMessage()
  -> Message::setString()
  -> Session::next()
  -> Application::fromApp()
```
