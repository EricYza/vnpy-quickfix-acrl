# vn.py QuickFIX Gateway 阶段整理

本文档记录当前 `vnpy_quickfix_gateway` 项目的阶段成果、目录结构、运行方式、核心代码行级说明，以及必要的 vn.py / QuickFIX 背景知识。

当前项目定位是：用 Python 版 QuickFIX 写一个可修改的 vn.py FIX Gateway。现在它已经能作为 FIX Initiator 连接 QuickFIX `ordermatch` Acceptor，并完成下单、成交回报、撤单、行情快照到 vn.py 数据事件的转换。

## 当前状态

当前已经打通的链路：

```text
vn.py OrderRequest
-> QuickfixGateway.send_order()
-> FixApplication.send_new_order_single()
-> QuickFIX SocketInitiator
-> FIX 35=D NewOrderSingle
-> ordermatch
-> FIX 35=8 ExecutionReport
-> FixExecutionReport
-> vn.py OrderData / TradeData
```

```text
vn.py CancelRequest
-> QuickfixGateway.cancel_order()
-> FixApplication.send_order_cancel_request()
-> QuickFIX SocketInitiator
-> FIX 35=F OrderCancelRequest
-> ordermatch
-> FIX 35=8 ExecutionReport CANCELED
-> vn.py OrderData Status.CANCELLED
```

```text
vn.py SubscribeRequest
-> QuickfixGateway.subscribe()
-> FixApplication.send_market_data_request()
-> QuickFIX SocketInitiator
-> FIX 35=V MarketDataRequest
-> ordermatch
-> FIX 35=W MarketDataSnapshotFullRefresh
-> FixMarketDataSnapshot
-> vn.py TickData
```

现在已经新增了两个 vn.py 入口：

```text
vnpy_quickfix_gateway.run_trader
```

这个入口会创建 vn.py `MainEngine` 和 Trader GUI，并执行 `main_engine.add_gateway(QuickfixGateway)`。启动后，菜单里会出现“连接QUICKFIX”，这就是 GUI 方式进入 QuickFIX 网关。

```text
vnpy_quickfix_gateway.test_main_engine
```

这个入口不打开 GUI，但会走 vn.py `MainEngine`，通过 `main_engine.connect()`、`main_engine.send_order()`、`main_engine.cancel_order()`、`main_engine.subscribe()` 调用网关。它用于验证“vn.py 主引擎 -> QuickFIX Gateway -> QuickFIX ordermatch”这条链路。

## 角色关系

本项目里各部分的角色如下：

```text
vn.py
  负责策略、事件、OrderRequest、CancelRequest、SubscribeRequest、OrderData、TradeData、TickData。

vnpy_quickfix_gateway
  负责把 vn.py 的请求翻译成 FIX 业务消息，把 FIX 回报翻译成 vn.py 数据对象。

QuickFIX Python
  负责 FIX Session、Logon、Logout、Heartbeat、MsgSeqNum、BodyLength、CheckSum、Socket 连接、消息收发。

QuickFIX ordermatch
  当前作为本地测试用 FIX Server / Acceptor，模拟撮合、撤单、行情快照。
```

也就是说，`vnpy_quickfix_gateway` 不自己写 socket，不自己计算 FIX 校验和，也不自己维护序号。它只创建 QuickFIX Message 对象并填业务字段，然后调用 `fix.Session.sendToTarget(...)`。从这一步开始，协议封装和 TCP 收发都由 QuickFIX 接管。

## 目录结构

当前目录大致如下：

```text
vnpy-quickfix-acrl/vnpy_quickfix_gateway/
├── README.md
├── pyproject.toml
├── configs/
│   └── ordermatch_local.cfg
├── runtime/
│   ├── client_log/
│   ├── client_store/
│   ├── server_log/
│   └── server_store/
├── vnpy_quickfix_gateway/
│   ├── __init__.py
│   ├── fix_application.py
│   ├── gateway.py
│   ├── mapping.py
│   ├── quickfix_client.cfg
│   ├── run_trader.py
│   ├── test_connect.py
│   ├── test_gateway.py
│   └── test_main_engine.py
└── vnpy_quickfix_gateway.egg-info/
```

### 源码文件

`vnpy_quickfix_gateway/gateway.py`

这是 vn.py 网关层。它继承 `BaseGateway`，实现 `connect()`、`close()`、`send_order()`、`cancel_order()`、`subscribe()` 等 vn.py 期望的接口。

`vnpy_quickfix_gateway/fix_application.py`

这是 QuickFIX 回调层。它继承 `quickfix.Application`，处理 `onLogon()`、`fromApp()` 等 QuickFIX 回调，也负责构造 `NewOrderSingle`、`OrderCancelRequest`、`MarketDataRequest`。

`vnpy_quickfix_gateway/mapping.py`

这是映射层。它把 FIX 字段和 vn.py 枚举互相转换，例如 `Side=1` 转成 `Direction.LONG`，`OrdStatus=2` 转成 `Status.ALLTRADED`，并把 FIX 回报组装成 `OrderData`、`TradeData`、`TickData`。

`vnpy_quickfix_gateway/test_connect.py`

这是 QuickFIX 原生层测试脚本。它不经过 vn.py `BaseGateway`，直接测试 QuickFIX 连接、发单和双边撮合。

`vnpy_quickfix_gateway/test_gateway.py`

这是 vn.py gateway 层测试脚本。它会手动创建 `EventEngine` 和 `QuickfixGateway`，然后通过 vn.py 的 `OrderRequest` / `CancelRequest` / `SubscribeRequest` 来测试完整 gateway 路径。

`vnpy_quickfix_gateway/test_main_engine.py`

这是 vn.py MainEngine 层测试脚本。它会创建 `MainEngine`，执行 `main_engine.add_gateway(QuickfixGateway)`，然后通过 `main_engine.connect()`、`main_engine.send_order()`、`main_engine.cancel_order()`、`main_engine.subscribe()` 走正式主引擎路径。

`vnpy_quickfix_gateway/run_trader.py`

这是 vn.py Trader GUI 启动入口。它会创建 `MainEngine` 和 `MainWindow`，并注册 `QuickfixGateway`，启动后菜单里会出现“连接QUICKFIX”。

### 配置文件

`vnpy_quickfix_gateway/quickfix_client.cfg`

这是 Python QuickFIX Initiator 配置，也就是我们这边的 client 配置。

`configs/ordermatch_local.cfg`

这是 QuickFIX `ordermatch` Acceptor 的本地测试配置。它限制只允许 `127.0.0.1` 连接。

### 生成文件

`runtime/client_log`、`runtime/client_store`、`runtime/server_log`、`runtime/server_store`

这些是 QuickFIX 的运行日志和消息持久化文件。`store` 里保存 FIX 会话序号、消息体、session 状态；`log` 里保存 event 和 message 日志。

`vnpy_quickfix_gateway.egg-info`

这是 `pip install -e .` 生成的 editable 安装元数据，不是手写源码。

`__pycache__`

这是 Python 运行后生成的字节码缓存，不是手写源码。

## 运行命令

每次测试前先启动 `ordermatch`：

```bash
conda activate vnpyfix
cd /path/to/vnpy-quickfix-acrl
./scripts/run_ordermatch.sh
```

测试 QuickFIX 原生连接：

```bash
conda activate vnpyfix
cd /path/to/vnpy-quickfix-acrl
python -m vnpy_quickfix_gateway.test_connect --seconds 10
```

测试 QuickFIX 原生双边撮合：

```bash
python -m vnpy_quickfix_gateway.test_connect --match-test --seconds 20
```

测试 vn.py gateway 双边撮合：

```bash
python -m vnpy_quickfix_gateway.test_gateway --seconds 20
```

测试 vn.py MainEngine 双边撮合：

```bash
python -m vnpy_quickfix_gateway.test_main_engine --seconds 20
```

测试 vn.py gateway 撤单：

```bash
python -m vnpy_quickfix_gateway.test_gateway --cancel-test --seconds 10
```

测试 vn.py MainEngine 撤单：

```bash
python -m vnpy_quickfix_gateway.test_main_engine --cancel-test --seconds 10
```

测试 vn.py gateway 行情订阅：

```bash
python -m vnpy_quickfix_gateway.test_gateway --market-data-test --seconds 10
```

测试 vn.py MainEngine 行情订阅：

```bash
python -m vnpy_quickfix_gateway.test_main_engine --market-data-test --seconds 10
```

启动带 QUICKFIX 网关的 vn.py Trader GUI：

```bash
python -m vnpy_quickfix_gateway.run_trader
```

启动 GUI 后自动连接 QUICKFIX：

```bash
python -m vnpy_quickfix_gateway.run_trader --connect
```

如果希望使用 `pyproject.toml` 里定义的命令行脚本：

```bash
pip install -e .
vnpy-quickfix-main-test --seconds 20
vnpy-quickfix-trader
```

注意：新增或修改 `[project.scripts]` 后，需要重新执行一次 `pip install -e .`，命令行脚本才会刷新。直接用 `python -m ...` 不需要重新安装。

如果修改了 `QuickFIX/examples/ordermatch` 的 C++ 代码，需要重新 build：

```bash
conda activate vnpyfix
cd /path/to/vnpy-quickfix-acrl
cmake --build QuickFIX/build-conda-full --target ordermatch -j2
```

## pyproject.toml 行级说明

当前文件行号：

```text
1-3
定义 build system。这里用 setuptools 和 wheel，让这个目录可以作为 Python 包安装。

5-12
定义项目元数据。包名是 vnpy-quickfix-gateway，Python 要求 >=3.11，依赖 vnpy。
这里没有写 quickfix 依赖，因为 quickfix 是本地编译安装进 conda 环境的，不是普通 PyPI 包。

14-15
告诉 setuptools 查找 vnpy_quickfix_gateway* 这个包。
```

这个文件的目的：让你可以在项目根目录执行：

```bash
pip install -e .
```

editable 安装后，修改源码会立刻生效。

## __init__.py 行级说明

文件：`vnpy_quickfix_gateway/__init__.py`

```text
1
包说明字符串。

3
从 gateway.py 导出 QuickfixGateway。这样外部可以直接写：
from vnpy_quickfix_gateway import QuickfixGateway

5
当前包版本。

7
__all__ 声明公开 API，目前只公开 QuickfixGateway。
```

## quickfix_client.cfg 行级说明

文件：`vnpy_quickfix_gateway/quickfix_client.cfg`

```text
1
[DEFAULT] 表示默认配置段，后面的 session 会继承这些配置。

2
ConnectionType=initiator，说明我们这边是主动连接方。

3
ReconnectInterval=2，断线后每 2 秒尝试重连。

4
HeartBtInt=30，心跳间隔 30 秒。

5-6
FileStorePath 和 FileLogPath 指向本项目 runtime 下的 client_store/client_log。

7-8
StartTime/EndTime 设置为全天有效。

9-10
UseDataDictionary=Y 并指定 FIX42.xml。开启字典校验后，QuickFIX 会检查必填字段和消息结构。

11-12
SocketConnectHost=127.0.0.1，SocketConnectPort=5002，连接本机 ordermatch。

13
ResetOnLogon=Y，Logon 时重置序号，方便本地反复测试。

14-15
时间精度和字段顺序设置。

17-20
定义 FIX.4.2 会话。SenderCompID=CLIENT1，TargetCompID=ORDERMATCH。
这和 ordermatch server 端配置正好相反。
```

## ordermatch_local.cfg 行级说明

文件：`configs/ordermatch_local.cfg`

```text
1-3
ConnectionType=acceptor，说明 ordermatch 是服务端；SocketAcceptPort=5002，监听 5002。

4
SocketReuseAddress=Y，方便本地重启服务端。

5-6
server 侧 store/log 放在本项目 runtime/server_store 和 runtime/server_log。

9-10
开启 FIX42 字典校验。

14-18
第一个 session：ORDERMATCH -> CLIENT1，只允许 127.0.0.1 连接。

20-24
第二个 session：ORDERMATCH -> CLIENT2，也只允许 127.0.0.1。
当前 Python client 使用 CLIENT1。
```

## gateway.py 行级说明

文件：`vnpy_quickfix_gateway/gateway.py`

### 1-27：导入依赖

```text
1
启用 postponed annotations，避免类型注解在运行时过早求值。

3
导入 copy。vn.py 的 BaseGateway 文档要求推送事件时对象不要再被后续修改，所以 send_order 初始状态推送时用 copy(order)。

4
导入 dataclass，用于 OrderContext。

5
导入 Path，用于处理 cfg 路径。

7
import quickfix as fix。gateway 层需要创建 SocketInitiator、SessionSettings、FileStoreFactory、FileLogFactory。

9-17
导入 vn.py 的 EventEngine、枚举、BaseGateway 和请求/数据对象。

19
导入 FixApplication。它是真正与 QuickFIX 回调和消息发送打交道的类。

20-27
导入 mapping.py 里的标准化数据结构和转换函数。
```

### 30-38：默认配置和订单上下文

```text
30
DEFAULT_CONFIG 指向同目录 quickfix_client.cfg。

33-38
OrderContext 保存 vn.py 下单时的 exchange、order_type、offset、price。
为什么需要它：FIX ExecutionReport 里不一定有 vn.py 需要的全部上下文，例如 offset 和 exchange，所以要在发单时缓存。
```

### 41-56：QuickfixGateway 类和状态

```text
41
QuickfixGateway 继承 BaseGateway。这是 vn.py 对所有网关的抽象要求。

44
default_name = "QUICKFIX"。vn.py 里这个 gateway 的名字。

45-47
default_setting 定义连接设置，目前只有一个“配置文件”。

48
exchanges = [Exchange.LOCAL]。当前 demo 使用 LOCAL 交易所。

50-56
初始化 gateway 状态：
application 是 FixApplication。
initiator 是 QuickFIX SocketInitiator。
orders 缓存 orderid -> OrderData。
order_contexts 缓存 orderid -> OrderContext。
subscription_exchanges 缓存 symbol -> exchange，用于 TickData。
```

### 58-75：connect()

```text
58
实现 BaseGateway.connect。

59-60
读取配置文件路径，并创建 QuickFIX SessionSettings。

62-65
创建 FixApplication，并把两个回调传进去：
on_execution_report -> self.on_fix_execution_report
on_market_data_snapshot -> self.on_fix_market_data_snapshot

66-67
创建 FileStoreFactory 和 FileLogFactory。
store 负责会话消息持久化，log 负责日志。

68-73
创建 SocketInitiator。这里就是 QuickFIX 的 socket/session 封装入口。

74
initiator.start() 启动连接线程，开始连接 ordermatch。

75
通过 vn.py write_log 推送日志。
```

### 77-83：close()

```text
77
实现 BaseGateway.close。

78-81
如果 initiator 存在，就 stop，并写日志。

83
清空 application 引用。
```

### 85-95：subscribe()

```text
85
实现 vn.py 订阅接口。

86
保存 symbol -> exchange，后续 TickData 要用。

88-90
如果 FIX 还没有 Logon，就写日志并返回。

92-95
调用 FixApplication.send_market_data_request(req.symbol)，发送 FIX 35=V。
```

### 97-135：send_order()

```text
97
实现 vn.py 下单接口。

98
生成本地 orderid。当前用时间戳格式 VNyyyymmddHHMMSSffffff。

99
调用 req.create_order_data(orderid, gateway_name)，这是 vn.py 官方推荐做法。

100
先把订单状态设为 Status.SUBMITTING。

101-107
缓存 OrderData 和 OrderContext。

108
推送一条 SUBMITTING 的 OrderData。

110-114
当前只支持限价单。如果不是 LIMIT，直接 REJECTED。

116-120
如果 FIX 未登录，直接 REJECTED。

122-129
调用 FixApplication.send_new_order_single()，构造并发送 FIX 35=D。

123
symbol 来自 vn.py OrderRequest。

125
fix_side_from_direction 把 Direction.LONG/SHORT 转成 FIX Side=1/2。

126-127
价格和数量来自 vn.py。

128
cl_ord_id 使用 gateway 生成的 orderid，后面回报用它关联。

130-133
发送异常时把订单置为 REJECTED。

135
返回 vn.py 需要的 vt_orderid，格式类似 QUICKFIX.VN202607...
```

### 137-158：cancel_order()

```text
137
实现 vn.py 撤单接口。

138-141
从缓存里找原始订单；找不到就写日志。

143-145
如果订单没有 direction，不能构造 FIX Side。

147-149
未登录时不允许撤单。

151-156
调用 FixApplication.send_order_cancel_request()，发送 FIX 35=F。

153
OrigClOrdID 使用原订单 orderid。

154
Symbol 来自 CancelRequest。

155
Side 从原订单 direction 转换而来。
```

### 160-164：账户和持仓占位

```text
160-164
query_account/query_position 目前只写日志。
真实券商 FIX 接口里账户、持仓通常是定制消息，后续再接。
```

### 166-191：ExecutionReport 回调

```text
166
FixApplication 收到 FIX 35=8 后，会调用这里。

167-171
取发单时缓存的 OrderContext。没有上下文时用 LOCAL/LIMIT/NONE/0 兜底。

173-180
把 FixExecutionReport 转成 vn.py OrderData。

181-182
缓存最新订单并推送 on_order。

184-189
如果这条 ExecutionReport 是成交回报，就转换成 TradeData。

190-191
有 TradeData 就推送 on_trade。
```

### 193-200：行情快照回调

```text
193
FixApplication 收到 FIX 35=W 后，会调用这里。

194
从订阅缓存里找 exchange。

195-199
把 FixMarketDataSnapshot 转成 vn.py TickData。

200
推送 on_tick。
```

### 202-210：配置路径解析

```text
202-210
支持 setting 里传“配置文件”、config_path 或 config。
如果不传，就使用 DEFAULT_CONFIG。
```

## fix_application.py 行级说明

文件：`vnpy_quickfix_gateway/fix_application.py`

### 1-17：导入

```text
3
datetime 用于生成 ClOrdID / MDReqID。

4
Callable 用于回调类型注解。

6-7
quickfix 是通用 FIX 字段、Session、Application。
quickfix42 是 FIX 4.2 版本消息类，例如 NewOrderSingle、OrderCancelRequest、MarketDataRequest。

9-17
导入 mapping.py 中的标准化解析函数和显示函数。
```

### 20-39：辅助函数

```text
20
SOH 是 FIX 消息真实分隔符。

23-26
format_fix_message 把 SOH 替换成 |，方便终端阅读。

29-33
split_message_session 兼容 QuickFIX Python 回调参数顺序差异。

36-39
get_msg_type 从 FIX header 里读取 MsgType，例如 8、W、D、F、V。
```

### 42-56：FixApplication 初始化

```text
42
继承 fix.Application，这是 QuickFIX Python 的回调基类。

45-49
构造函数接受两个可选回调：
on_execution_report 给 gateway.py 用。
on_market_data_snapshot 给 gateway.py 用。

50
调用 super().__init__()，完成 QuickFIX Application 初始化。

51
session_id 保存当前 FIX session。

52
logged_on 标记是否已经登录。

53-54
保存收到的 ExecutionReport 和 MarketDataSnapshot，测试脚本用它们判断回报数量。

55-56
保存外部回调。
```

### 58-80：QuickFIX 生命周期回调

```text
58-64
onCreate/onLogon。onLogon 时保存 session_id，并把 logged_on 设为 True。

66-68
onLogout 把 logged_on 设为 False。

70-80
toAdmin/fromAdmin/toApp 打印消息。
Admin 消息包括 Logon、Logout、Heartbeat 等。
App 消息包括订单、回报、行情等。
```

### 82-107：fromApp 分发

```text
82
所有应用层入站消息都会进 fromApp。

83
兼容参数顺序，拿到 message 和 session_id。

84
读取 MsgType。

85-89
打印原始 FIX 消息。

90-91
MsgType=8 时当作 ExecutionReport 处理。

92-93
MsgType=W 时当作 MarketDataSnapshotFullRefresh 处理。

95-100
on_execution_report 调用 mapping.execution_report_from_fix 转成 FixExecutionReport，然后触发 gateway 回调。

102-107
on_market_data_snapshot 调用 mapping.market_data_snapshot_from_fix 转成 FixMarketDataSnapshot，然后触发 gateway 回调。
```

### 109-140：发送 NewOrderSingle

```text
109-116
send_new_order_single 接收 symbol、side、price、quantity、可选 cl_ord_id。

117-118
未登录不能发单。

120
没有 cl_ord_id 时生成一个。

121
把 BUY/SELL/1/2 归一成 FIX Side。

122
创建 FIX 4.2 NewOrderSingle，消息类型是 35=D。

123-131
填写必需字段：
11 ClOrdID
21 HandlInst
55 Symbol
54 Side
60 TransactTime
38 OrderQty
40 OrdType
44 Price
59 TimeInForce

133
fix.Session.sendToTarget 交给 QuickFIX Session 和 SocketInitiator 发送。

134-139
打印发送摘要。
```

### 142-168：发送 OrderCancelRequest

```text
142-148
send_order_cancel_request 接收原始订单号、symbol、side、可选撤单请求号。

149-150
未登录不能撤单。

152
生成撤单 ClOrdID。

154
创建 FIX 4.2 OrderCancelRequest，消息类型是 35=F。

155-159
填写必需字段：
41 OrigClOrdID
11 ClOrdID
55 Symbol
54 Side
60 TransactTime

161
交给 QuickFIX 发送。
```

### 170-204：发送 MarketDataRequest

```text
170-174
send_market_data_request 接收 symbol 和可选 md_req_id。

175-176
未登录不能订阅。

178
生成 MDReqID。

179
创建 FIX 4.2 MarketDataRequest，消息类型是 35=V。

180-184
设置：
262 MDReqID
263 SubscriptionRequestType=0，表示 snapshot
264 MarketDepth=5。客户端请求最多五档；当前 ordermatch demo 仍只返回一档固定假行情。

186-193
添加 NoMDEntryTypes 重复组，请求 BID、OFFER、TRADE。

195-197
添加 NoRelatedSym 重复组，请求某个 Symbol。

199
交给 QuickFIX 发送。
```

### 206-208：订单号生成

```text
206-208
生成 VN 开头的时间戳 ID。
当前只是 demo 级别，未来真实生产可以换成更严格的序号生成器。
```

## mapping.py 行级说明

文件：`vnpy_quickfix_gateway/mapping.py`

### 1-10：导入

```text
6-7
quickfix 提供字段类和常量；quickfix42 提供 FIX 4.2 重复组类。

9-10
导入 vn.py 枚举和数据对象。
```

### 13-81：FIX 与 vn.py 枚举表

```text
13-16
FIX Side 显示名。

18-25
FIX ExecType 显示名，例如 NEW、FILL、CANCELED。

27-37
FIX OrdStatus 显示名。

39-47
vn.py Direction 和 FIX Side 双向映射。

49-59
vn.py OrderType 和 FIX OrdType 双向映射。

61-69
FIX OrdStatus 到 vn.py Status 的映射：
0 -> NOTTRADED
1 -> PARTTRADED
2 -> ALLTRADED
4 -> CANCELLED
8 -> REJECTED

71-75
哪些 ExecType 代表成交，需要生成 TradeData。

77-81
行情 entry 类型显示名：BID、OFFER、TRADE。
```

### 84-140：FixExecutionReport

```text
84-101
标准化 ExecutionReport 字段。它不再是原始 FIX Message，而是 Python dataclass。

103-106
orderid 使用 ClOrdID。因为我们下单时用 ClOrdID 作为本地订单号。

108-112
direction 属性把 FIX side 转成 vn.py Direction。

114-116
status 属性把 FIX OrdStatus 转成 vn.py Status。

118-120
is_trade 判断是否应该生成 TradeData。

122-140
summary 用于打印更好读的回报。
```

### 143-162：FixMarketDataSnapshot

```text
143-154
标准化行情快照，保存最多五档买卖和最新成交。一档字段保持兼容。

156-162
summary 用于打印行情摘要。
```

### 165-247：基础字段转换函数

```text
165-173
describe_fix_value 把 2 显示成 2(FILL) 这样的形式。

176-181
get_field_value 安全读取 FIX 字段，字段不存在时返回默认值。

184-188
get_float_field 读取数字字段并转 float。

191-200
get_last_qty 兼容 FIX 4.2 的 LastShares 和更新版本里的 LastQty。

203-223
Side / Direction 互转，以及文本 BUY/SELL/1/2 到 FIX Side 的转换。

226-237
OrdType / OrderType 互转。

240-247
OrdStatus 到 vn.py Status 的转换。
```

### 250-323：ExecutionReport 到 vn.py 对象

```text
250-273
execution_report_from_fix 从原始 FIX Message 里提取字段，生成 FixExecutionReport。

276-299
order_data_from_execution_report 生成 vn.py OrderData。
重点字段：
symbol -> symbol
exchange -> Exchange
orderid -> ClOrdID
direction -> Direction
volume -> OrderQty
traded -> CumQty
status -> Status

302-323
trade_data_from_execution_report 在 report.is_trade 为 True 时生成 TradeData。
tradeid 当前使用 ExecID。
price 使用 LastPx。
volume 使用 LastQty。
```

### 326-387：行情快照到 TickData

```text
326-367
market_data_snapshot_from_fix 解析 FIX 35=W。

331-333
读取 MDReqID 和 NoMDEntries，并创建 NoMDEntries group 对象。

358-370
遍历每个行情 entry：
269=0 -> bid
269=1 -> ask
269=2 -> last trade

372-405
同价聚合后，将 bid 按价格降序、ask 按价格升序排列，截取并补齐五档。

408-427
tick_data_from_market_data_snapshot 生成 vn.py TickData。
现在会映射 bid/ask 的 1 至 5 档字段。
```

## test_connect.py 行级说明

这个文件是 QuickFIX 原生测试，不走 vn.py `QuickfixGateway`。

```text
16-53
解析命令行参数，支持 --send-order 和 --match-test。

56-62
等待 Logon。

65-75
等待 ExecutionReport 数量达到预期。

78-79
生成唯一 symbol，避免和之前 ordermatch 内存里的订单串单。

82-114
run_match_test 直接调用 FixApplication，发送 BUY 和 SELL 两笔撮合单。

117-131
创建 QuickFIX SessionSettings、FixApplication、FileStoreFactory、FileLogFactory、SocketInitiator。

134
initiator.start() 启动 QuickFIX 连接。

137-151
根据参数执行单笔下单或双边撮合测试。

153-157
保持进程运行，让回报有时间到达。

160-162
退出时 stop initiator。
```

## test_gateway.py 行级说明

这个文件是 vn.py gateway 层测试，会走 `QuickfixGateway`。

```text
17-39
解析参数，支持 --cancel-test 和 --market-data-test。

42-73
定义事件打印函数。vn.py 的 on_order/on_trade/on_tick 最终都会变成 EventEngine 事件。

76-82
等待 FIX Logon。

85-97
等待某个订单状态，例如 NOTTRADED 或 CANCELLED。

100-101
生成唯一 symbol。

104-133
send_matching_orders 构造 vn.py OrderRequest，然后调用 gateway.send_order。
这验证 vn.py OrderRequest -> FIX NewOrderSingle -> vn.py OrderData/TradeData。

136-173
run_cancel_test 先发一笔不会成交的买单，等 NOTTRADED 后创建 CancelRequest，再调用 gateway.cancel_order。

176-181
run_market_data_test 创建 SubscribeRequest，然后调用 gateway.subscribe。

184-191
创建 EventEngine，并注册日志、订单、成交、行情事件处理函数。

193
创建 QuickfixGateway。

196
调用 gateway.connect。

197-198
等待 Logon。

200-205
根据参数选择行情、撤单或双边撮合测试。

207-209
关闭 gateway 和 EventEngine。
```

## test_main_engine.py 行级说明

这个文件是 vn.py MainEngine 层测试。它和 `test_gateway.py` 的最大区别是：它不直接调用 `gateway.send_order()`，而是通过 `main_engine.send_order(..., "QUICKFIX")` 调用网关。

```text
1-22
导入 argparse、time、vn.py 的 EventEngine/MainEngine、事件常量、请求对象，以及 QuickfixGateway。

25-49
parse_args 定义命令行参数：
--config 指定 QuickFIX client cfg
--seconds 指定测试后等待多久
--symbol 指定测试合约
--price 指定价格
--quantity 指定数量
--cancel-test 测试撤单
--market-data-test 测试行情订阅

52-83
定义 EVENT_LOG、EVENT_ORDER、EVENT_TRADE、EVENT_TICK 的打印函数。
这一步模拟 GUI monitor 的效果：事件引擎收到数据后，把它们打印到终端。

86-87
generate_symbol 生成 MAIN 开头的唯一 symbol，避免 ordermatch 里已有订单影响本次撮合。

90-96
wait_for_logon 等待 QuickFIX Logon。
虽然调用入口是 MainEngine，但实际 Logon 状态仍然保存在 gateway.application 里。

99-111
wait_for_order_status 等待指定订单状态，例如 NOTTRADED 或 CANCELLED。

114-144
send_matching_orders 通过 MainEngine 发一买一卖：
main_engine.send_order(buy_req, "QUICKFIX")
main_engine.send_order(sell_req, "QUICKFIX")
这就是正式 vn.py 主引擎下单路径。

147-189
run_cancel_test 通过 MainEngine 发单、等待 NOTTRADED，再通过 main_engine.cancel_order 撤单。

192-197
run_market_data_test 通过 main_engine.subscribe 发起行情订阅。

200-210
main 创建 EventEngine、注册事件打印函数、创建 MainEngine，并执行 main_engine.add_gateway(QuickfixGateway)。

212-216
通过 main_engine.connect(setting, "QUICKFIX") 连接 QuickFIX。

218-225
按命令行参数选择行情、撤单或双边撮合测试。

226-227
finally 中调用 main_engine.close()，让 vn.py 主引擎统一关闭 event engine 和 gateway。
```

## run_trader.py 行级说明

这个文件是 GUI 入口。它的作用不是做测试，而是把 QUICKFIX 网关注册进 vn.py Trader 窗口。

```text
1-10
导入 argparse、Path、EventEngine、MainEngine、MainWindow、QtCore、create_qapp，以及 QuickfixGateway。

13-33
parse_args 定义 GUI 启动参数：
--config 给自动连接使用
--connect 表示窗口启动后自动连接 QUICKFIX
--normal-window 表示普通窗口启动，不最大化

36-42
main 创建 qapp、EventEngine、MainEngine，并执行：
main_engine.add_gateway(QuickfixGateway)
这是 GUI 能看到 QUICKFIX 网关的关键。

44-48
创建 MainWindow。默认最大化显示；如果传入 --normal-window，则普通窗口显示。

50-56
如果传入 --connect，用 QtCore.QTimer.singleShot 在 GUI 事件循环启动后调用：
main_engine.connect(setting, "QUICKFIX")
这里不用直接立即 connect，是为了让窗口先创建完成，日志和状态能正常进入 GUI。

58
进入 Qt 事件循环。

61-62
支持 python -m vnpy_quickfix_gateway.run_trader 方式运行。
```

## QuickFIX ordermatch 改动

我们为了让行情链路闭环，改了 QuickFIX demo：

`../QuickFIX/examples/ordermatch/Application.h`

```text
37-40
引入 FIX42 MarketDataRequest、MarketDataSnapshotFullRefresh、NewOrderSingle、OrderCancelRequest。

56-59
声明 MessageCracker 对 FIX42 NewOrderSingle、OrderCancelRequest、MarketDataRequest 的处理函数。

64-65
新增 sendMarketDataSnapshot 和 addMarketDataEntry。
```

`../QuickFIX/examples/ordermatch/Application.cpp`

```text
90-111
收到 FIX42 MarketDataRequest 后读取 MDReqID、SubscriptionRequestType、MarketDepth、NoRelatedSym。
如果请求类型不是 snapshot，就抛 IncorrectTagValue。
对每个 symbol 调用 sendMarketDataSnapshot。

117-131
sendMarketDataSnapshot 创建 FIX42 MarketDataSnapshotFullRefresh。
当前固定返回：
bid 10.0 x 100
ask 10.5 x 100
last 10.25 x 10
然后用 FIX::Session::sendToTarget 发回 client。

133-143
addMarketDataEntry 添加 NoMDEntries 重复组，设置 269/270/271。
```

这只是 demo 行情快照，不是真实订单簿行情。后续如果想让行情来自撮合簿，需要扩展 `Market` / `OrderMatcher` 暴露 best bid/ask，再在 `sendMarketDataSnapshot` 里读取真实值。

## 必要的 QuickFIX 知识

### Initiator 和 Acceptor

FIX 连接里通常有两个角色：

```text
Initiator
主动连接方。当前是 vnpy_quickfix_gateway / QuickFIX SocketInitiator。

Acceptor
被连接方。当前是 QuickFIX ordermatch。
```

配置里对应：

```text
Python client:
ConnectionType=initiator
SenderCompID=CLIENT1
TargetCompID=ORDERMATCH

ordermatch server:
ConnectionType=acceptor
SenderCompID=ORDERMATCH
TargetCompID=CLIENT1
```

### Application 回调

QuickFIX 通过 `fix.Application` 回调把事件交给用户代码：

```text
onCreate
创建 session。

onLogon
登录成功。

onLogout
登出。

toAdmin/fromAdmin
管理类消息，例如 Logon、Logout、Heartbeat。

toApp/fromApp
业务类消息，例如 NewOrderSingle、ExecutionReport、MarketDataSnapshot。
```

### Session 层做了什么

我们调用：

```python
fix.Session.sendToTarget(message, self.session_id)
```

QuickFIX 会负责：

```text
补 BeginString 8
计算 BodyLength 9
维护 MsgSeqNum 34
补 SenderCompID 49
补 SendingTime 52
补 TargetCompID 56
计算 CheckSum 10
使用 SOH 分隔符
通过 TCP socket 发送
处理 Logon/Logout/Heartbeat/重连
```

### 当前用到的 FIX 消息

```text
35=A Logon
35=5 Logout
35=0 Heartbeat
35=D NewOrderSingle
35=8 ExecutionReport
35=F OrderCancelRequest
35=V MarketDataRequest
35=W MarketDataSnapshotFullRefresh
```

### 当前用到的核心 FIX 字段

```text
11 ClOrdID
41 OrigClOrdID
17 ExecID
37 OrderID
39 OrdStatus
150 ExecType
54 Side
55 Symbol
38 OrderQty
44 Price
14 CumQty
151 LeavesQty
31 LastPx
32 LastShares
6 AvgPx
262 MDReqID
263 SubscriptionRequestType
264 MarketDepth
267 NoMDEntryTypes
268 NoMDEntries
269 MDEntryType
270 MDEntryPx
271 MDEntrySize
```

## 必要的 vn.py 知识

### BaseGateway

vn.py 所有交易接口都继承 `BaseGateway`。核心方法包括：

```text
connect(setting)
连接交易系统。

close()
关闭连接。

send_order(req)
发送委托。

cancel_order(req)
撤单。

subscribe(req)
订阅行情。
```

核心回调包括：

```text
on_order(OrderData)
推送订单状态。

on_trade(TradeData)
推送成交。

on_tick(TickData)
推送行情。

write_log(msg)
推送日志。
```

### EventEngine

vn.py 的数据不是直接返回给调用者，而是通过事件引擎广播。比如：

```text
gateway.on_order(order)
-> EVENT_ORDER

gateway.on_trade(trade)
-> EVENT_TRADE

gateway.on_tick(tick)
-> EVENT_TICK
```

`test_gateway.py` 里手动创建 `EventEngine` 并注册打印函数，所以你能看到 `[VNPY] order`、`[VNPY] trade`、`[VNPY] tick`。

### vn.py 请求和数据对象

```text
OrderRequest
vn.py 发单请求。包含 symbol、exchange、direction、type、volume、price。

CancelRequest
vn.py 撤单请求。包含 orderid、symbol、exchange。

SubscribeRequest
vn.py 行情订阅请求。包含 symbol、exchange。

OrderData
订单状态。包含 status、traded、volume、direction 等。

TradeData
成交数据。包含 tradeid、orderid、price、volume。

TickData
行情数据。包含 last_price、bid_price_1、ask_price_1 等。
```

## 当前能力边界

已经支持：

```text
FIX Logon / Logout / Heartbeat
NewOrderSingle 限价单
ExecutionReport NEW / FILL / CANCELED
OrderCancelRequest
MarketDataRequest snapshot
MarketDataSnapshotFullRefresh -> TickData
vn.py OrderData / TradeData / TickData
```

当前限制：

```text
只实现 FIX 4.2。
只支持 LIMIT 订单。
只支持 demo 级 LOCAL exchange。
行情快照是 ordermatch demo 固定返回值，不是实时订单簿。
账户、持仓、合约查询还没实现。
已经有 vn.py MainEngine 和 Trader GUI 入口，但还需要你在本机 GUI 环境里实际操作验证。
真实券商 FIX 通常有自定义字段、登录认证、账户模型，需要单独适配。
```

## 下一阶段建议

建议下一阶段做 GUI 实测和交易体验补齐：

```text
1. 先运行 test_main_engine，确认 MainEngine 路径和前面的 gateway 路径结果一致。
2. 再运行 run_trader，确认菜单里出现“连接QUICKFIX”。
3. 在 GUI 里连接 QUICKFIX，手动输入 LOCAL exchange、symbol、price、volume，下单测试。
4. 观察 GUI 的日志、委托、成交、行情监控是否收到事件。
5. 后续补合约查询、账户查询、持仓查询，让 GUI 体验更接近正式交易接口。
```

现在使用路径已经可以从：

```text
python -m vnpy_quickfix_gateway.test_gateway
```

升级到：

```text
vn.py MainEngine / Trader UI
-> QUICKFIX gateway
-> QuickFIX ordermatch
```
